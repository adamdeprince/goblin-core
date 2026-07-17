#pragma once

// Compact Redis set: one pooled allocation with a fingerprint directory, the
// hash listpack without values. Layout:
//
//   [ u16 entries_len ][ u16 count ]
//   [ fingerprints: count * u8 ][ entry_offsets: count * u16 ][ entries... ]
//
// Each entry:
//   [ encoded_member_len: packed 1-2 B ][ shared-string encoded member ]
//
// Members use the shared exact encoder with LZ4 forbidden (same as hash
// fields). Fingerprints are taken from the logical member and resolved by
// encoded compare. Promote to the full Swiss form when the entry count or blob
// ceiling is exceeded.

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/blob_pool.hpp"
#include "goblin/core/simd_ops.hpp"
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

inline constexpr std::size_t kSetListpackHeaderBytes = 4;
inline constexpr std::size_t kSetListpackMaxBlobBytes = 0xFFFF;
inline constexpr std::size_t kSetListpackMaxStringBytes = 0x7FFF;

class SetListpack {
 public:
  struct AddResult {
    bool added{false};
    bool needs_full{false};
  };

  struct AddManyResult {
    long long added{0};
    bool needs_full{false};
  };

  SetListpack() = default;
  ~SetListpack() { free_blob(p_); }
  SetListpack(const SetListpack&) = delete;
  SetListpack& operator=(const SetListpack&) = delete;
  SetListpack(SetListpack&& other) noexcept
      : p_(std::exchange(other.p_, nullptr)) {}
  SetListpack& operator=(SetListpack&& other) noexcept {
    if (this != &other) {
      free_blob(p_);
      p_ = std::exchange(other.p_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return count(); }
  [[nodiscard]] bool empty() const noexcept { return count() == 0; }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return p_ ? blob_size(len(), count()) : 0;
  }

  // Cheap sufficient fit check before demoting the full form.
  [[nodiscard]] static bool can_encode(std::size_t entry_count,
                                       std::size_t payload_bytes) noexcept {
    // Worst-case per entry: raw tag + 2-byte length + 3-byte directory slot.
    constexpr std::size_t kPerEntryOverhead = 6;
    if (entry_count > std::numeric_limits<std::uint16_t>::max() ||
        entry_count >
            (kSetListpackMaxBlobBytes - kSetListpackHeaderBytes) /
                kPerEntryOverhead) {
      return false;
    }
    return payload_bytes <= kSetListpackMaxBlobBytes -
                                kSetListpackHeaderBytes -
                                entry_count * kPerEntryOverhead;
  }

  [[nodiscard]] AddResult add(std::string_view member, std::size_t max_entries,
                              StringEncodingOptions encoding = {}) {
    if (member.size() > encoding.max_value_bytes()) {
      return {.needs_full = true};
    }
    const EncodedString encoded(member, encoding,
                                StringCompressionMode::NeverLz4);
    if (encoded.size() > kSetListpackMaxStringBytes) {
      return {.needs_full = true};
    }
    const Header hdr = unpack();
    const auto fp = fingerprint(member);
    if (find_member(encoded, hdr, fp).found()) {
      return {.added = false, .needs_full = false};
    }
    const auto new_count = static_cast<std::size_t>(hdr.count) + 1;
    const std::size_t elem = entry_size(encoded.size());
    if (new_count > max_entries ||
        blob_size(hdr.len + elem, new_count) > kSetListpackMaxBlobBytes) {
      return {.added = false, .needs_full = true};
    }
    append_entry(encoded, fp, hdr);
    return {.added = true, .needs_full = false};
  }

  [[nodiscard]] AddManyResult add_many(std::span<const std::string_view> members,
                                       std::size_t max_entries,
                                       StringEncodingOptions encoding = {}) {
    if (members.empty()) {
      return {};
    }
    const Header hdr = unpack();
    static thread_local std::vector<PendingEntry> merged;
    static thread_local std::vector<EncodedString> incoming;
    merged.clear();
    incoming.clear();
    merged.reserve(static_cast<std::size_t>(hdr.count) + members.size());
    incoming.reserve(members.size());

    for_each_encoded_hdr(hdr, [&](std::size_t index, std::string_view enc) {
      merged.push_back(PendingEntry{.logical = {},
                                    .encoded = enc,
                                    .incoming = nullptr,
                                    .fingerprint = hdr.fingerprints[index]});
    });

    long long added = 0;
    for (const auto member : members) {
      if (member.size() > encoding.max_value_bytes()) {
        return {.needs_full = true};
      }
      incoming.emplace_back(member, encoding, StringCompressionMode::NeverLz4);
      const auto* encoded = &incoming.back();
      if (encoded->size() > kSetListpackMaxStringBytes) {
        return {.needs_full = true};
      }
      const auto fp = fingerprint(member);
      bool exists = false;
      for (const auto& entry : merged) {
        if (entry.fingerprint == fp && entry.equals(member, *encoded)) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        merged.push_back(PendingEntry{.logical = member,
                                      .encoded = {},
                                      .incoming = encoded,
                                      .fingerprint = fp});
        ++added;
      }
    }
    if (merged.size() > max_entries || !entries_fit_in_blob(merged)) {
      return {.needs_full = true};
    }
    replace_with_entries(merged);
    return {.added = added};
  }

  [[nodiscard]] bool contains(std::string_view member,
                              StringEncodingOptions encoding = {}) const {
    if (member.size() > encoding.max_value_bytes()) {
      return false;
    }
    const EncodedString encoded(member, encoding,
                                StringCompressionMode::NeverLz4);
    if (encoded.size() > kSetListpackMaxStringBytes) {
      return false;
    }
    const Header hdr = unpack();
    return find_member(encoded, hdr, fingerprint(member)).found();
  }

  bool erase(std::string_view member, StringEncodingOptions encoding = {}) {
    if (member.size() > encoding.max_value_bytes()) {
      return false;
    }
    const EncodedString encoded(member, encoding,
                                StringCompressionMode::NeverLz4);
    if (encoded.size() > kSetListpackMaxStringBytes) {
      return false;
    }
    const Header hdr = unpack();
    const FindResult found =
        find_member(encoded, hdr, fingerprint(member));
    if (!found.found()) {
      return false;
    }
    erase_at(found.entry_offset, found.index, hdr);
    return true;
  }

  // Erase by dense directory index (SPOP).
  bool erase_at_index(std::size_t index) {
    const Header hdr = unpack();
    if (index >= hdr.count) {
      return false;
    }
    erase_at(offset_at(hdr, index), index, hdr);
    return true;
  }

  [[nodiscard]] EncodedStringView at(
      std::size_t index, StringEncodingOptions encoding = {}) const noexcept {
    const Header hdr = unpack();
    assert(index < hdr.count);
    const auto entry = entry_at(hdr.entries + offset_at(hdr, index));
    return EncodedStringView(entry.member, encoding.encoding_enabled());
  }

  template <class Fn>
  void for_each(Fn&& fn, StringEncodingOptions encoding = {}) const {
    for_each_encoded_hdr(unpack(), [&](std::size_t, std::string_view enc) {
      fn(EncodedStringView(enc, encoding.encoding_enabled()));
    });
  }

 private:
  static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

  struct FindResult {
    std::size_t entry_offset{kNotFound};
    std::size_t index{0};
    [[nodiscard]] bool found() const noexcept {
      return entry_offset != kNotFound;
    }
  };

  struct PendingEntry {
    std::string_view logical;
    std::string_view encoded;
    const EncodedString* incoming{nullptr};
    std::uint8_t fingerprint{0};

    [[nodiscard]] bool equals(std::string_view logical_member,
                              const EncodedString& enc) const noexcept {
      return incoming == nullptr ? enc.equals_encoded(encoded)
                                 : simd::bytes_equal(logical, logical_member);
    }
    [[nodiscard]] std::size_t size() const noexcept {
      return incoming == nullptr ? encoded.size() : incoming->size();
    }
    void write_to(char* destination) const noexcept {
      if (incoming == nullptr) {
        if (!encoded.empty()) {
          std::memcpy(destination, encoded.data(), encoded.size());
        }
      } else {
        incoming->write_to(destination);
      }
    }
  };

  struct Header {
    std::uint16_t len{0};
    std::uint16_t count{0};
    std::uint8_t* fingerprints{nullptr};
    char* offsets{nullptr};
    char* entries{nullptr};
  };

  struct PackedLength {
    std::size_t value{0};
    std::size_t bytes{1};
  };

  struct EntryView {
    std::string_view member;
    std::size_t header_bytes{0};
    [[nodiscard]] std::size_t size() const noexcept {
      return header_bytes + member.size();
    }
  };

  [[nodiscard]] static Header unpack_blob(char* p) noexcept {
    Header hdr;
    if (!p) {
      return hdr;
    }
    std::memcpy(&hdr.len, p, sizeof(hdr.len));
    std::memcpy(&hdr.count, p + 2, sizeof(hdr.count));
    hdr.fingerprints =
        reinterpret_cast<std::uint8_t*>(p + kSetListpackHeaderBytes);
    hdr.offsets = reinterpret_cast<char*>(hdr.fingerprints + hdr.count);
    hdr.entries = hdr.offsets + static_cast<std::size_t>(hdr.count) * 2;
    return hdr;
  }

  [[nodiscard]] Header unpack() const noexcept { return unpack_blob(p_); }
  [[nodiscard]] std::size_t count() const noexcept { return unpack().count; }
  [[nodiscard]] std::uint16_t len() const noexcept { return unpack().len; }

  template <class Fn>
  static void for_each_encoded_hdr(const Header& hdr, Fn&& fn) {
    std::size_t off = 0;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto entry = entry_at(hdr.entries + off);
      fn(i, entry.member);
      off += entry.size();
    }
  }

  static void write_header(char* p, std::size_t entries_len,
                           std::uint16_t n) noexcept {
    assert(entries_len <= std::numeric_limits<std::uint16_t>::max());
    const auto packed_len = static_cast<std::uint16_t>(entries_len);
    std::memcpy(p, &packed_len, sizeof(packed_len));
    std::memcpy(p + 2, &n, sizeof(n));
  }

  [[nodiscard]] static std::size_t blob_size(std::size_t entries_len,
                                             std::size_t count) noexcept {
    return kSetListpackHeaderBytes + count * 3 + entries_len;
  }

  [[nodiscard]] char* alloc_blob(std::size_t entries_len, std::uint16_t count) {
    return static_cast<char*>(
        blob_pool().allocate(blob_size(entries_len, count), 1));
  }
  void free_blob(char* p) noexcept {
    if (p) {
      const Header hdr = unpack_blob(p);
      blob_pool().deallocate(p, blob_size(hdr.len, hdr.count), 1);
    }
  }

  [[nodiscard]] static std::size_t packed_length_size(
      std::size_t length) noexcept {
    return length <= 0x7F ? 1 : 2;
  }

  [[nodiscard]] static PackedLength read_length(const char* p) noexcept {
    const auto first = static_cast<std::uint8_t>(p[0]);
    if ((first & 0x80) == 0) {
      return {.value = first, .bytes = 1};
    }
    return {.value = (static_cast<std::size_t>(first & 0x7F) << 8) |
                     static_cast<std::uint8_t>(p[1]),
            .bytes = 2};
  }

  [[nodiscard]] static char* write_length(char* p,
                                          std::size_t length) noexcept {
    assert(length <= kSetListpackMaxStringBytes);
    if (length <= 0x7F) {
      p[0] = static_cast<char>(length);
      return p + 1;
    }
    p[0] = static_cast<char>(0x80 | ((length >> 8) & 0x7F));
    p[1] = static_cast<char>(length & 0xFF);
    return p + 2;
  }

  [[nodiscard]] static EntryView entry_at(const char* e) noexcept {
    const auto member_len = read_length(e);
    return {.member = std::string_view(e + member_len.bytes, member_len.value),
            .header_bytes = member_len.bytes};
  }

  [[nodiscard]] static std::size_t entry_size(std::size_t member_len) noexcept {
    return packed_length_size(member_len) + member_len;
  }

  [[nodiscard]] static bool entries_fit_in_blob(
      std::span<const PendingEntry> entries) noexcept {
    std::size_t entries_len = 0;
    for (const auto& entry : entries) {
      entries_len += entry_size(entry.size());
    }
    return blob_size(entries_len, entries.size()) <= kSetListpackMaxBlobBytes;
  }

  [[nodiscard]] static std::uint16_t offset_at(const Header& hdr,
                                               std::size_t index) noexcept {
    std::uint16_t offset = 0;
    std::memcpy(&offset, hdr.offsets + index * 2, sizeof(offset));
    return offset;
  }

  static void write_offset(const Header& hdr, std::size_t index,
                           std::uint16_t offset) noexcept {
    std::memcpy(hdr.offsets + index * 2, &offset, sizeof(offset));
  }

  [[nodiscard]] static std::uint8_t fingerprint(
      std::string_view member) noexcept {
    auto hash = std::hash<std::string_view>{}(member);
    if constexpr (sizeof(hash) > sizeof(std::uint32_t)) {
      hash ^= hash >> 32;
    }
    return static_cast<std::uint8_t>(hash);
  }

  [[nodiscard]] FindResult find_member(const EncodedString& member,
                                       const Header& hdr,
                                       std::uint8_t needle) const noexcept {
    std::size_t base = 0;
    while (base + simd::kFingerprintGroupWidth <= hdr.count) {
      auto matches =
          simd::match_fingerprint_group(hdr.fingerprints + base, needle);
      while (matches != 0) {
        const auto index =
            base + static_cast<std::size_t>(std::countr_zero(matches));
        matches &= matches - 1;
        const auto off = offset_at(hdr, index);
        const auto entry = entry_at(hdr.entries + off);
        if (member.equals_encoded(entry.member)) {
          return {.entry_offset = off, .index = index};
        }
      }
      base += simd::kFingerprintGroupWidth;
    }
    for (; base < hdr.count; ++base) {
      if (hdr.fingerprints[base] != needle) {
        continue;
      }
      const auto off = offset_at(hdr, base);
      const auto entry = entry_at(hdr.entries + off);
      if (member.equals_encoded(entry.member)) {
        return {.entry_offset = off, .index = base};
      }
    }
    return {};
  }

  void erase_at(std::size_t off, std::size_t index, const Header& hdr) {
    const auto entry = entry_at(hdr.entries + off);
    const std::size_t elem = entry.size();
    const std::size_t new_len = hdr.len - elem;
    const auto new_count = static_cast<std::uint16_t>(hdr.count - 1);
    if (new_count == 0) {
      free_blob(p_);
      p_ = nullptr;
      return;
    }
    char* np = alloc_blob(new_len, new_count);
    write_header(np, new_len, new_count);
    const Header next = unpack_blob(np);
    if (index > 0) {
      std::memcpy(next.fingerprints, hdr.fingerprints, index);
      std::memcpy(next.offsets, hdr.offsets, index * 2);
    }
    if (index + 1 < hdr.count) {
      const auto tail = static_cast<std::size_t>(hdr.count) - index - 1;
      std::memcpy(next.fingerprints + index, hdr.fingerprints + index + 1,
                  tail);
      std::memcpy(next.offsets + index * 2, hdr.offsets + (index + 1) * 2,
                  tail * 2);
    }
    for (std::size_t dst = 0; dst < new_count; ++dst) {
      auto entry_offset = offset_at(next, dst);
      if (entry_offset > off) {
        write_offset(next, dst,
                     static_cast<std::uint16_t>(entry_offset - elem));
      }
    }
    std::memcpy(next.entries, hdr.entries, off);
    std::memcpy(next.entries + off, hdr.entries + off + elem,
                hdr.len - off - elem);
    free_blob(p_);
    p_ = np;
  }

  void append_entry(const EncodedString& member, std::uint8_t fp,
                    const Header& hdr) {
    const std::size_t elem = entry_size(member.size());
    const std::size_t new_len = hdr.len + elem;
    const auto new_count = static_cast<std::uint16_t>(hdr.count + 1);
    char* np = alloc_blob(new_len, new_count);
    write_header(np, new_len, new_count);
    const Header next = unpack_blob(np);
    if (hdr.count != 0) {
      std::memcpy(next.fingerprints, hdr.fingerprints, hdr.count);
      std::memcpy(next.offsets, hdr.offsets,
                  static_cast<std::size_t>(hdr.count) * 2);
    }
    next.fingerprints[hdr.count] = fp;
    write_offset(next, hdr.count, static_cast<std::uint16_t>(hdr.len));
    if (hdr.len != 0) {
      std::memcpy(next.entries, hdr.entries, hdr.len);
    }
    auto* payload = write_length(next.entries + hdr.len, member.size());
    member.write_to(payload);
    free_blob(p_);
    p_ = np;
  }

  void replace_with_entries(std::span<const PendingEntry> entries) {
    std::size_t entries_len = 0;
    for (const auto& entry : entries) {
      entries_len += entry_size(entry.size());
    }
    const auto count = static_cast<std::uint16_t>(entries.size());
    char* np = alloc_blob(entries_len, count);
    write_header(np, entries_len, count);
    const Header next = unpack_blob(np);
    std::uint16_t offset = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto& entry = entries[index];
      next.fingerprints[index] = entry.fingerprint;
      write_offset(next, index, offset);
      auto* payload = write_length(next.entries + offset, entry.size());
      entry.write_to(payload);
      offset = static_cast<std::uint16_t>(offset + entry_size(entry.size()));
    }
    free_blob(p_);
    p_ = np;
  }

  char* p_{nullptr};
};

}  // namespace goblin::core
