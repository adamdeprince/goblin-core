#pragma once

// A compact hash as ONE pooled allocation -- the hash analogue of
// CompactListpack. Field/value bytes stay densely packed while a 3-byte entry
// directory avoids reparsing every preceding variable-length entry on lookup.
//
// Allocation layout:
//   [ u32 entries_len ][ u16 count ][ u8 pad ][ u8 pad ]
//   [ fingerprints: count * u8 ][ entry_offsets: count * u16 ][ entries... ]
//
// The header remains 8 bytes (same shape as CompactListpack so the blob pool is
// shared). Entry offsets are relative to entries and fit u16 because the whole
// blob is capped at 65,535 bytes. Fingerprints are scanned with the build ISA's
// widest byte vector; an actual field compare resolves the expected 1/256 false
// positives.
//
// Each entry (fixed u16 lengths -- goblin strings are capped at 64 KiB):
//   [ field_len: u16 LE ][ value_len: u16 LE ][ field bytes ][ value bytes ]
//
// Empty field/value are legal (length 0). No backlen: the table is unordered and
// only walked forward. max_entries is a store-global passed into set(), not stored
// in the blob (same discipline as zset listpack).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/blob_pool.hpp"
#include "goblin/core/simd_ops.hpp"

namespace goblin::core {

inline constexpr std::size_t kHashListpackHeaderBytes = 8;
// Match CompactListpack's wire-size ceiling so a tiny hash stays one small blob.
inline constexpr std::size_t kHashListpackMaxBlobBytes = 0xFFFF;
// u16 length fields; goblin's string ceiling is the same 64 KiB - 1.
inline constexpr std::size_t kHashListpackMaxStringBytes =
    std::numeric_limits<std::uint16_t>::max();

class HashListpack {
 public:
  struct SetResult {
    bool added{false};       // true if the field was new
    bool needs_full{false};  // would exceed listpack limits; no mutation
  };

  struct SetManyResult {
    long long added{0};
    bool needs_full{false};
  };

  HashListpack() = default;
  ~HashListpack() { free_blob(p_); }
  HashListpack(const HashListpack&) = delete;
  HashListpack& operator=(const HashListpack&) = delete;
  HashListpack(HashListpack&& other) noexcept
      : p_(std::exchange(other.p_, nullptr)) {}
  HashListpack& operator=(HashListpack&& other) noexcept {
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

  // Exact fit check for converting the full representation back to a listpack.
  // `payload_bytes` is the sum of field and value lengths; each entry also
  // needs four length bytes plus the three-byte fingerprint/offset directory.
  [[nodiscard]] static bool can_encode(std::size_t entry_count,
                                       std::size_t payload_bytes) noexcept {
    constexpr std::size_t kPerEntryOverhead = 7;
    if (entry_count > std::numeric_limits<std::uint16_t>::max() ||
        entry_count >
            (kHashListpackMaxBlobBytes - kHashListpackHeaderBytes) /
                kPerEntryOverhead) {
      return false;
    }
    return payload_bytes <= kHashListpackMaxBlobBytes -
                                kHashListpackHeaderBytes -
                                entry_count * kPerEntryOverhead;
  }

  // HSET one field. needs_full=true (no mutation) if it would exceed max_entries
  // or the blob-size ceiling -- caller promotes to the full swiss+arena form.
  [[nodiscard]] SetResult set(std::string_view field, std::string_view value,
                              std::size_t max_entries) {
    if (field.size() > kHashListpackMaxStringBytes ||
        value.size() > kHashListpackMaxStringBytes) {
      return {.needs_full = true};
    }
    const Header hdr = unpack();
    const auto fp = fingerprint(field);
    const FindResult found = find_field(field, hdr, fp);
    const std::size_t new_elem = entry_size(field.size(), value.size());

    if (found.found()) {
      const auto [flen, vlen] =
          lengths_at(hdr.entries + found.entry_offset);
      const std::size_t old_elem = entry_size(flen, vlen);
      if (value.size() == vlen) {
        // Same value width: overwrite in place (field bytes untouched).
        char* val = hdr.entries + found.entry_offset + 4 + flen;
        if (!value.empty()) {
          std::memcpy(val, value.data(), value.size());
        }
        return {.added = false, .needs_full = false};
      }
      // Different size: one exact-fit rebuild (not erase + append).
      const std::size_t projected = hdr.len - old_elem + new_elem;
      if (blob_size(projected, hdr.count) > kHashListpackMaxBlobBytes) {
        return {.added = false, .needs_full = true};
      }
      replace_entry_at(found.entry_offset, found.index, field, value, hdr,
                       old_elem, new_elem);
      return {.added = false, .needs_full = false};
    }

    // New field.
    const auto new_count = static_cast<std::size_t>(hdr.count) + 1;
    if (new_count > max_entries ||
        blob_size(hdr.len + new_elem, new_count) >
            kHashListpackMaxBlobBytes) {
      return {.added = false, .needs_full = true};
    }
    append_entry(field, value, fp, hdr);
    return {.added = true, .needs_full = false};
  }

  // Merge a command-sized batch and rebuild the blob once. The temporary views
  // remain valid until the replacement has copied every old and incoming byte.
  [[nodiscard]] SetManyResult set_many(
      std::span<const std::pair<std::string_view, std::string_view>> fields,
      std::size_t max_entries) {
    if (fields.empty()) {
      return {};
    }

    const Header hdr = unpack();
    static thread_local std::vector<PendingEntry> merged;
    merged.clear();
    merged.reserve(static_cast<std::size_t>(hdr.count) + fields.size());
    for_each_hdr(hdr, [&](std::string_view field, std::string_view value) {
      merged.push_back(PendingEntry{field, value, fingerprint(field)});
    });

    long long added = 0;
    for (const auto& [field, value] : fields) {
      if (field.size() > kHashListpackMaxStringBytes ||
          value.size() > kHashListpackMaxStringBytes) {
        return {.needs_full = true};
      }
      const auto fp = fingerprint(field);
      auto* existing = static_cast<PendingEntry*>(nullptr);
      for (auto& entry : merged) {
        if (entry.fingerprint == fp &&
            simd::bytes_equal(entry.field, field)) {
          existing = &entry;
          break;
        }
      }
      if (existing != nullptr) {
        existing->value = value;
      } else {
        merged.push_back(PendingEntry{field, value, fp});
        ++added;
      }
    }

    if (merged.size() > max_entries ||
        !entries_fit_in_blob(merged)) {
      return {.needs_full = true};
    }
    replace_with_entries(merged);
    return {.added = added};
  }

  [[nodiscard]] std::optional<std::string_view> get(
      std::string_view field) const {
    const Header hdr = unpack();
    const FindResult found = find_field(field, hdr, fingerprint(field));
    if (!found.found()) {
      return std::nullopt;
    }
    const auto [flen, vlen] =
        lengths_at(hdr.entries + found.entry_offset);
    if (vlen == 0) {
      return std::string_view{};
    }
    return std::string_view(hdr.entries + found.entry_offset + 4 + flen, vlen);
  }

  [[nodiscard]] bool contains(std::string_view field) const {
    const Header hdr = unpack();
    return find_field(field, hdr, fingerprint(field)).found();
  }

  // HDEL. Returns true if the field was present.
  bool erase(std::string_view field) {
    const Header hdr = unpack();
    const FindResult found = find_field(field, hdr, fingerprint(field));
    if (!found.found()) {
      return false;
    }
    erase_at(found.entry_offset, found.index, hdr);
    return true;
  }

  // HSETNX helper: true if field is absent (caller then set()).
  [[nodiscard]] bool absent(std::string_view field) const {
    const Header hdr = unpack();
    return !find_field(field, hdr, fingerprint(field)).found();
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for_each_hdr(unpack(), std::forward<Fn>(fn));
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
    std::string_view field;
    std::string_view value;
    std::uint8_t fingerprint{0};
  };

  struct Header {
    std::uint32_t len{0};
    std::uint16_t count{0};
    std::uint8_t* fingerprints{nullptr};
    char* offsets{nullptr};
    char* entries{nullptr};
  };

  [[nodiscard]] static Header unpack_blob(char* p) noexcept {
    Header hdr;
    if (!p) {
      return hdr;
    }
    std::memcpy(&hdr.len, p, sizeof(hdr.len));
    std::memcpy(&hdr.count, p + 4, sizeof(hdr.count));
    hdr.fingerprints = reinterpret_cast<std::uint8_t*>(
        p + kHashListpackHeaderBytes);
    hdr.offsets = reinterpret_cast<char*>(hdr.fingerprints + hdr.count);
    hdr.entries = hdr.offsets + static_cast<std::size_t>(hdr.count) * 2;
    return hdr;
  }

  [[nodiscard]] Header unpack() const noexcept { return unpack_blob(p_); }

  [[nodiscard]] std::size_t count() const noexcept { return unpack().count; }
  [[nodiscard]] std::uint32_t len() const noexcept { return unpack().len; }

  template <class Fn>
  static void for_each_hdr(const Header& hdr, Fn&& fn) {
    std::size_t off = 0;
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [flen, vlen] = lengths_at(e + off);
      const std::string_view f(e + off + 4, flen);
      const std::string_view v(e + off + 4 + flen, vlen);
      fn(f, v);
      off += entry_size(flen, vlen);
    }
  }

  static void write_header(char* p, std::uint32_t entries_len,
                           std::uint16_t n) noexcept {
    std::memcpy(p, &entries_len, sizeof(entries_len));
    std::memcpy(p + 4, &n, sizeof(n));
    p[6] = 0;
    p[7] = 0;
  }

  [[nodiscard]] static std::size_t blob_size(std::size_t entries_len,
                                             std::size_t count) noexcept {
    return kHashListpackHeaderBytes + count * 3 + entries_len;
  }

  [[nodiscard]] static char* alloc_blob(std::uint32_t entries_len,
                                        std::uint16_t count) {
    return static_cast<char*>(blob_pool().allocate(
        blob_size(entries_len, count), 1));
  }
  static void free_blob(char* p) noexcept {
    if (p) {
      const Header hdr = unpack_blob(p);
      blob_pool().deallocate(p, blob_size(hdr.len, hdr.count), 1);
    }
  }

  [[nodiscard]] static std::size_t entry_size(std::size_t field_len,
                                              std::size_t value_len) noexcept {
    return 4 + field_len + value_len;
  }

  [[nodiscard]] static bool entries_fit_in_blob(
      std::span<const PendingEntry> entries) noexcept {
    std::size_t entries_len = 0;
    for (const auto& entry : entries) {
      entries_len += entry_size(entry.field.size(), entry.value.size());
    }
    return blob_size(entries_len, entries.size()) <= kHashListpackMaxBlobBytes;
  }

  [[nodiscard]] static std::pair<std::size_t, std::size_t> lengths_at(
      const char* e) noexcept {
    std::uint16_t flen = 0;
    std::uint16_t vlen = 0;
    std::memcpy(&flen, e, sizeof(flen));
    std::memcpy(&vlen, e + 2, sizeof(vlen));
    return {flen, vlen};
  }

  // Compare path only needs the field length (value length is unused).
  [[nodiscard]] static std::size_t field_len_at(const char* e) noexcept {
    std::uint16_t flen = 0;
    std::memcpy(&flen, e, sizeof(flen));
    return flen;
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

  static void write_entry_bytes(char* dst, std::string_view field,
                                std::string_view value) noexcept {
    const auto flen = static_cast<std::uint16_t>(field.size());
    const auto vlen = static_cast<std::uint16_t>(value.size());
    std::memcpy(dst, &flen, sizeof(flen));
    std::memcpy(dst + 2, &vlen, sizeof(vlen));
    if (!field.empty()) {
      std::memcpy(dst + 4, field.data(), field.size());
    }
    if (!value.empty()) {
      std::memcpy(dst + 4 + field.size(), value.data(), value.size());
    }
  }

  [[nodiscard]] static std::uint8_t fingerprint(
      std::string_view field) noexcept {
    auto hash = std::hash<std::string_view>{}(field);
    if constexpr (sizeof(hash) > sizeof(std::uint32_t)) {
      hash ^= hash >> 32;
    }
    return static_cast<std::uint8_t>(hash);
  }

  [[nodiscard]] FindResult find_field(std::string_view field, const Header& hdr,
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
        const auto flen = field_len_at(hdr.entries + off);
        if (simd::bytes_equal(
                std::string_view(hdr.entries + off + 4, flen), field)) {
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
      const auto flen = field_len_at(hdr.entries + off);
      if (simd::bytes_equal(
              std::string_view(hdr.entries + off + 4, flen), field)) {
        return {.entry_offset = off, .index = base};
      }
    }
    return {};
  }

  void erase_at(std::size_t off, std::size_t index, const Header& hdr) {
    const auto [flen, vlen] = lengths_at(hdr.entries + off);
    const std::size_t elem = entry_size(flen, vlen);
    const std::uint32_t new_len = hdr.len - static_cast<std::uint32_t>(elem);
    const auto new_count = static_cast<std::uint16_t>(hdr.count - 1);
    if (new_count == 0) {
      free_blob(p_);
      p_ = nullptr;
      return;
    }
    char* np = alloc_blob(new_len, new_count);
    write_header(np, new_len, new_count);
    const Header next = unpack_blob(np);

    // Bulk-copy the directory around the removed slot.
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
    // Offsets after the erased entry shrink by elem.
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

  // Rewrite one entry at a known slot in a single exact-fit allocation. Field
  // identity (and therefore fingerprint) is unchanged; only the value size
  // differs from the in-place path.
  void replace_entry_at(std::size_t off, std::size_t index,
                        std::string_view field, std::string_view value,
                        const Header& hdr, std::size_t old_elem,
                        std::size_t new_elem) {
    const std::uint32_t new_len =
        hdr.len - static_cast<std::uint32_t>(old_elem) +
        static_cast<std::uint32_t>(new_elem);
    char* np = alloc_blob(new_len, hdr.count);
    write_header(np, new_len, hdr.count);
    const Header next = unpack_blob(np);

    std::memcpy(next.fingerprints, hdr.fingerprints, hdr.count);
    std::memcpy(next.offsets, hdr.offsets,
                static_cast<std::size_t>(hdr.count) * 2);
    const auto delta = static_cast<std::int32_t>(new_elem) -
                       static_cast<std::int32_t>(old_elem);
    if (delta != 0) {
      for (std::size_t i = 0; i < hdr.count; ++i) {
        if (i == index) {
          continue;
        }
        auto entry_offset = offset_at(next, i);
        if (entry_offset > off) {
          write_offset(
              next, i,
              static_cast<std::uint16_t>(
                  static_cast<std::int32_t>(entry_offset) + delta));
        }
      }
    }

    if (off != 0) {
      std::memcpy(next.entries, hdr.entries, off);
    }
    write_entry_bytes(next.entries + off, field, value);
    const std::size_t tail = hdr.len - off - old_elem;
    if (tail != 0) {
      std::memcpy(next.entries + off + new_elem, hdr.entries + off + old_elem,
                  tail);
    }
    free_blob(p_);
    p_ = np;
  }

  void append_entry(std::string_view field, std::string_view value,
                    std::uint8_t fp, const Header& hdr) {
    const std::size_t elem = entry_size(field.size(), value.size());
    const std::uint32_t new_len = hdr.len + static_cast<std::uint32_t>(elem);
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
    write_entry_bytes(next.entries + hdr.len, field, value);
    free_blob(p_);
    p_ = np;
  }

  void replace_with_entries(std::span<const PendingEntry> entries) {
    std::size_t entries_len = 0;
    for (const auto& entry : entries) {
      entries_len += entry_size(entry.field.size(), entry.value.size());
    }
    const auto count = static_cast<std::uint16_t>(entries.size());
    const auto packed_len = static_cast<std::uint32_t>(entries_len);
    char* np = alloc_blob(packed_len, count);
    write_header(np, packed_len, count);
    const Header next = unpack_blob(np);
    std::uint16_t offset = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto& entry = entries[index];
      next.fingerprints[index] = entry.fingerprint;
      write_offset(next, index, offset);
      write_entry_bytes(next.entries + offset, entry.field, entry.value);
      offset = static_cast<std::uint16_t>(
          offset + entry_size(entry.field.size(), entry.value.size()));
    }
    free_blob(p_);
    p_ = np;
  }

  char* p_ = nullptr;
};

}  // namespace goblin::core
