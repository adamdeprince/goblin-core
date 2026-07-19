#pragma once

// A compact hash as ONE pooled allocation -- the hash analogue of
// CompactListpack. Field/value bytes stay densely packed while a 3-byte entry
// directory avoids reparsing every preceding variable-length entry on lookup.
//
// Allocation layout:
//   [ u16 entries_len ][ u16 count ]
//   [ fingerprints: count * u8 ][ entry_offsets: count * u16 ][ entries... ]
//
// The byte-packed header and entry offsets are memcpy-accessed, so alignment adds
// no padding. Entry offsets are relative to entries and fit u16 because the whole
// blob is capped at 65,535 bytes. Fingerprints are scanned with the build ISA's
// widest byte vector; an actual field compare resolves the expected 1/256 false
// positives.
//
// Each entry:
//   [ encoded_field_len: packed 1-2 B ][ encoded_value_len: packed 1-2 B ]
//   [ shared-string encoded field ][ shared-string encoded value ]
//
// Lengths 0..127 use one byte. Larger canonical lengths use a high-bit byte
// containing bits 14..8 followed by bits 7..0, for a 32,767-byte ceiling. A
// larger field/value promotes the hash to its full representation. Fields use
// the shared exact string encoder with LZ4 forbidden. Empty field/value are
// legal. No backlen: the table is unordered and only walked forward.

#include <bit>
#include <cassert>
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
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

inline constexpr std::size_t kHashListpackHeaderBytes = 4;
// Match CompactListpack's wire-size ceiling so a tiny hash stays one small blob.
inline constexpr std::size_t kHashListpackMaxBlobBytes = 0xFFFF;
inline constexpr std::size_t kHashListpackMaxStringBytes = 0x7FFF;

class HashListpackBlobStorage {
 public:
  virtual ~HashListpackBlobStorage() = default;
  [[nodiscard]] virtual char* allocate(std::size_t bytes) = 0;
  virtual void deallocate(char* p, std::size_t bytes) noexcept = 0;
};

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

  HashListpack()
      : storage_flags_(reinterpret_cast<std::uintptr_t>(&pool_storage()) |
                       kOwnsBlob) {}
  HashListpack(char* p, HashListpackBlobStorage& storage) noexcept
      : p_(p), storage_flags_(reinterpret_cast<std::uintptr_t>(&storage)) {
    assert((storage_flags_ & kOwnsBlob) == 0);
  }
  ~HashListpack() {
    if (owns_blob()) {
      free_blob(p_);
    }
  }
  HashListpack(const HashListpack&) = delete;
  HashListpack& operator=(const HashListpack&) = delete;
  HashListpack(HashListpack&& other) noexcept
      : p_(std::exchange(other.p_, nullptr)),
        storage_flags_(std::exchange(other.storage_flags_,
                                     default_storage_flags())) {}
  HashListpack& operator=(HashListpack&& other) noexcept {
    if (this != &other) {
      if (owns_blob()) {
        free_blob(p_);
      }
      p_ = std::exchange(other.p_, nullptr);
      storage_flags_ =
          std::exchange(other.storage_flags_, default_storage_flags());
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return count(); }
  [[nodiscard]] bool empty() const noexcept { return count() == 0; }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return p_ ? blob_size(len(), count()) : 0;
  }
  [[nodiscard]] char* data() noexcept { return p_; }
  [[nodiscard]] const char* data() const noexcept { return p_; }
  [[nodiscard]] static std::size_t blob_bytes(const char* p) noexcept {
    if (p == nullptr) {
      return 0;
    }
    const auto hdr = unpack_blob(const_cast<char*>(p));
    return blob_size(hdr.len, hdr.count);
  }

  // Cheap sufficient fit check before converting the full representation back
  // to a listpack. `payload_bytes` is raw fields plus already-encoded values.
  // Budget the worst compact overhead: a raw-field tag, two two-byte lengths,
  // and the three-byte fingerprint/offset directory. Special encodings and
  // one-byte lengths can only make the attempted rebuild smaller.
  [[nodiscard]] static bool can_encode(std::size_t entry_count,
                                       std::size_t payload_bytes) noexcept {
    constexpr std::size_t kPerEntryOverhead = 8;
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
                              std::size_t max_entries,
                              StringEncodingOptions encoding = {}) {
    if (field.size() > encoding.max_value_bytes()) {
      return {.needs_full = true};
    }
    const EncodedString encoded_field(
        field, encoding, StringCompressionMode::NeverLz4);
    const EncodedString encoded_value(value, encoding);
    if (encoded_field.size() > kHashListpackMaxStringBytes ||
        encoded_value.size() > kHashListpackMaxStringBytes) {
      return {.needs_full = true};
    }
    const Header hdr = unpack();
    const auto fp = fingerprint(field);
    const FindResult found = find_field(encoded_field, hdr, fp);
    const std::size_t new_elem =
        entry_size(encoded_field.size(), encoded_value.size());

    if (found.found()) {
      const auto old = entry_at(hdr.entries + found.entry_offset);
      const std::size_t old_elem = old.size();
      if (encoded_value.size() == old.value.size()) {
        // Same value width: overwrite in place (field bytes untouched).
        encoded_value.write_to(const_cast<char*>(old.value.data()));
        return {.added = false, .needs_full = false};
      }
      // Different size: one exact-fit rebuild (not erase + append).
      const std::size_t projected = hdr.len - old_elem + new_elem;
      if (blob_size(projected, hdr.count) > kHashListpackMaxBlobBytes) {
        return {.added = false, .needs_full = true};
      }
      replace_entry_at(found.entry_offset, found.index, encoded_field,
                       encoded_value, hdr, old_elem, new_elem);
      return {.added = false, .needs_full = false};
    }

    // New field.
    const auto new_count = static_cast<std::size_t>(hdr.count) + 1;
    if (new_count > max_entries ||
        blob_size(hdr.len + new_elem, new_count) >
            kHashListpackMaxBlobBytes) {
      return {.added = false, .needs_full = true};
    }
    append_entry(encoded_field, encoded_value, fp, hdr);
    return {.added = true, .needs_full = false};
  }

  // Merge a command-sized batch and rebuild the blob once. The temporary views
  // remain valid until the replacement has copied every old and incoming byte.
  [[nodiscard]] SetManyResult set_many(
      std::span<const std::pair<std::string_view, std::string_view>> fields,
      std::size_t max_entries, StringEncodingOptions encoding = {}) {
    if (fields.empty()) {
      return {};
    }

    const Header hdr = unpack();
    static thread_local std::vector<PendingEntry> merged;
    static thread_local std::vector<EncodedString> incoming_fields;
    static thread_local std::vector<EncodedString> incoming_values;
    merged.clear();
    incoming_fields.clear();
    incoming_values.clear();
    merged.reserve(static_cast<std::size_t>(hdr.count) + fields.size());
    incoming_fields.reserve(fields.size());
    incoming_values.reserve(fields.size());
    for_each_encoded_hdr(
        hdr, [&](std::size_t index, std::string_view encoded_field,
                 std::string_view encoded_value) {
          merged.push_back(PendingEntry{.logical_field = {},
                                        .encoded_field = encoded_field,
                                        .encoded_value = encoded_value,
                                        .incoming_field = nullptr,
                                        .incoming_value = nullptr,
                                        .fingerprint =
                                            hdr.fingerprints[index]});
        });

    long long added = 0;
    for (const auto& [field, value] : fields) {
      if (field.size() > encoding.max_value_bytes()) {
        return {.needs_full = true};
      }
      incoming_fields.emplace_back(
          field, encoding, StringCompressionMode::NeverLz4);
      incoming_values.emplace_back(value, encoding);
      const auto* encoded_field = &incoming_fields.back();
      const auto* encoded_value = &incoming_values.back();
      if (encoded_field->size() > kHashListpackMaxStringBytes ||
          encoded_value->size() > kHashListpackMaxStringBytes) {
        return {.needs_full = true};
      }
      const auto fp = fingerprint(field);
      auto* existing = static_cast<PendingEntry*>(nullptr);
      for (auto& entry : merged) {
        if (entry.fingerprint == fp &&
            entry.field_equals(field, *encoded_field)) {
          existing = &entry;
          break;
        }
      }
      if (existing != nullptr) {
        existing->encoded_value = {};
        existing->incoming_value = encoded_value;
      } else {
        merged.push_back(PendingEntry{.logical_field = field,
                                      .encoded_field = {},
                                      .encoded_value = {},
                                      .incoming_field = encoded_field,
                                      .incoming_value = encoded_value,
                                      .fingerprint = fp});
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

  [[nodiscard]] std::optional<EncodedStringView> get(
      std::string_view field, StringEncodingOptions encoding = {}) const {
    if (field.size() > encoding.max_value_bytes()) {
      return std::nullopt;
    }
    const EncodedString encoded_field(
        field, encoding, StringCompressionMode::NeverLz4);
    if (encoded_field.size() > kHashListpackMaxStringBytes) {
      return std::nullopt;
    }
    const Header hdr = unpack();
    const FindResult found =
        find_field(encoded_field, hdr, fingerprint(field));
    if (!found.found()) {
      return std::nullopt;
    }
    const auto entry = entry_at(hdr.entries + found.entry_offset);
    return EncodedStringView(entry.value, encoding.encoding_enabled());
  }

  [[nodiscard]] bool contains(
      std::string_view field, StringEncodingOptions encoding = {}) const {
    return get(field, encoding).has_value();
  }

  // HDEL. Returns true if the field was present.
  bool erase(std::string_view field, StringEncodingOptions encoding = {}) {
    if (field.size() > encoding.max_value_bytes()) {
      return false;
    }
    const EncodedString encoded_field(
        field, encoding, StringCompressionMode::NeverLz4);
    if (encoded_field.size() > kHashListpackMaxStringBytes) {
      return false;
    }
    const Header hdr = unpack();
    const FindResult found =
        find_field(encoded_field, hdr, fingerprint(field));
    if (!found.found()) {
      return false;
    }
    erase_at(found.entry_offset, found.index, hdr);
    return true;
  }

  // HSETNX helper: true if field is absent (caller then set()).
  [[nodiscard]] bool absent(
      std::string_view field, StringEncodingOptions encoding = {}) const {
    return !contains(field, encoding);
  }

  template <class Fn>
  void for_each(Fn&& fn, StringEncodingOptions encoding = {}) const {
    for_each_encoded_hdr(
        unpack(), [&](std::size_t, std::string_view field,
                      std::string_view value) {
          auto logical_field =
              EncodedStringView(field, encoding.encoding_enabled()).to_string();
          fn(logical_field,
             EncodedStringView(value, encoding.encoding_enabled()));
        });
  }

  // Visit a bounded insertion-order range. The entry-offset directory makes
  // the first seek O(1); callers such as HSCAN do not reparse earlier entries.
  template <class Fn>
  void for_range(std::size_t first, std::size_t count, Fn&& fn,
                 StringEncodingOptions encoding = {}) const {
    const Header hdr = unpack();
    const auto end = std::min<std::size_t>(hdr.count, first + count);
    for (std::size_t index = first; index < end; ++index) {
      const auto entry = entry_at(hdr.entries + offset_at(hdr, index));
      auto logical_field =
          EncodedStringView(entry.field, encoding.encoding_enabled()).to_string();
      fn(std::string_view(logical_field),
         EncodedStringView(entry.value, encoding.encoding_enabled()));
    }
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
    std::string_view logical_field;
    std::string_view encoded_field;
    std::string_view encoded_value;
    const EncodedString* incoming_field{nullptr};
    const EncodedString* incoming_value{nullptr};
    std::uint8_t fingerprint{0};

    [[nodiscard]] bool field_equals(
        std::string_view logical, const EncodedString& encoded) const noexcept {
      return incoming_field == nullptr
                 ? encoded.equals_encoded(encoded_field)
                 : simd::bytes_equal(logical_field, logical);
    }
    [[nodiscard]] std::size_t field_size() const noexcept {
      return incoming_field == nullptr ? encoded_field.size()
                                       : incoming_field->size();
    }
    [[nodiscard]] std::size_t value_size() const noexcept {
      return incoming_value == nullptr ? encoded_value.size()
                                       : incoming_value->size();
    }
    void write_field(char* destination) const noexcept {
      if (incoming_field == nullptr) {
        if (!encoded_field.empty()) {
          std::memcpy(destination, encoded_field.data(), encoded_field.size());
        }
      } else {
        incoming_field->write_to(destination);
      }
    }
    void write_value(char* destination) const noexcept {
      if (incoming_value == nullptr) {
        if (!encoded_value.empty()) {
          std::memcpy(destination, encoded_value.data(), encoded_value.size());
        }
      } else {
        incoming_value->write_to(destination);
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
    std::string_view field;
    std::string_view value;
    std::size_t header_bytes{0};

    [[nodiscard]] std::size_t size() const noexcept {
      return header_bytes + field.size() + value.size();
    }
  };

  [[nodiscard]] static Header unpack_blob(char* p) noexcept {
    Header hdr;
    if (!p) {
      return hdr;
    }
    std::memcpy(&hdr.len, p, sizeof(hdr.len));
    std::memcpy(&hdr.count, p + 2, sizeof(hdr.count));
    hdr.fingerprints = reinterpret_cast<std::uint8_t*>(
        p + kHashListpackHeaderBytes);
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
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto entry = entry_at(e + off);
      fn(i, entry.field, entry.value);
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
    return kHashListpackHeaderBytes + count * 3 + entries_len;
  }

  class PoolStorage final : public HashListpackBlobStorage {
   public:
    [[nodiscard]] char* allocate(std::size_t bytes) override {
      return static_cast<char*>(blob_pool().allocate(bytes, 1));
    }
    void deallocate(char* p, std::size_t bytes) noexcept override {
      blob_pool().deallocate(p, bytes, 1);
    }
  };

  [[nodiscard]] static HashListpackBlobStorage& pool_storage() noexcept {
    static PoolStorage storage;
    return storage;
  }

  static constexpr std::uintptr_t kOwnsBlob = 1;

  [[nodiscard]] static std::uintptr_t default_storage_flags() noexcept {
    return reinterpret_cast<std::uintptr_t>(&pool_storage()) | kOwnsBlob;
  }

  [[nodiscard]] HashListpackBlobStorage& storage() const noexcept {
    return *reinterpret_cast<HashListpackBlobStorage*>(storage_flags_ &
                                                       ~kOwnsBlob);
  }

  [[nodiscard]] bool owns_blob() const noexcept {
    return (storage_flags_ & kOwnsBlob) != 0;
  }

  [[nodiscard]] char* alloc_blob(std::size_t entries_len,
                                 std::uint16_t count) {
    return storage().allocate(blob_size(entries_len, count));
  }
  void free_blob(char* p) noexcept {
    if (p) {
      const Header hdr = unpack_blob(p);
      storage().deallocate(p, blob_size(hdr.len, hdr.count));
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
    return {.value =
                (static_cast<std::size_t>(first & 0x7F) << 8) |
                static_cast<std::uint8_t>(p[1]),
            .bytes = 2};
  }

  [[nodiscard]] static char* write_length(char* p,
                                          std::size_t length) noexcept {
    assert(length <= kHashListpackMaxStringBytes);
    if (length <= 0x7F) {
      p[0] = static_cast<char>(length);
      return p + 1;
    }
    p[0] = static_cast<char>(0x80 | ((length >> 8) & 0x7F));
    p[1] = static_cast<char>(length & 0xFF);
    return p + 2;
  }

  [[nodiscard]] static EntryView entry_at(const char* e) noexcept {
    const auto field_len = read_length(e);
    const auto value_len = read_length(e + field_len.bytes);
    const auto header_bytes = field_len.bytes + value_len.bytes;
    const auto* field = e + header_bytes;
    return {.field = std::string_view(field, field_len.value),
            .value = std::string_view(field + field_len.value,
                                      value_len.value),
            .header_bytes = header_bytes};
  }

  [[nodiscard]] static std::size_t entry_size(std::size_t field_len,
                                              std::size_t value_len) noexcept {
    return packed_length_size(field_len) + packed_length_size(value_len) +
           field_len + value_len;
  }

  [[nodiscard]] static bool entries_fit_in_blob(
      std::span<const PendingEntry> entries) noexcept {
    std::size_t entries_len = 0;
    for (const auto& entry : entries) {
      entries_len += entry_size(entry.field_size(), entry.value_size());
    }
    return blob_size(entries_len, entries.size()) <= kHashListpackMaxBlobBytes;
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

  static void write_entry_bytes(char* dst, const EncodedString& field,
                                const EncodedString& value) noexcept {
    auto* payload = write_length(dst, field.size());
    payload = write_length(payload, value.size());
    field.write_to(payload);
    value.write_to(payload + field.size());
  }

  static void write_entry_bytes(char* dst,
                                const PendingEntry& entry) noexcept {
    auto* payload = write_length(dst, entry.field_size());
    payload = write_length(payload, entry.value_size());
    entry.write_field(payload);
    entry.write_value(payload + entry.field_size());
  }

  [[nodiscard]] static std::uint8_t fingerprint(
      std::string_view field) noexcept {
    auto hash = std::hash<std::string_view>{}(field);
    if constexpr (sizeof(hash) > sizeof(std::uint32_t)) {
      hash ^= hash >> 32;
    }
    return static_cast<std::uint8_t>(hash);
  }

  [[nodiscard]] FindResult find_field(const EncodedString& field,
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
        if (field.equals_encoded(entry.field)) {
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
      if (field.equals_encoded(entry.field)) {
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
                        const EncodedString& field,
                        const EncodedString& value, const Header& hdr,
                        std::size_t old_elem, std::size_t new_elem) {
    const std::size_t new_len = hdr.len - old_elem + new_elem;
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

  void append_entry(const EncodedString& field, const EncodedString& value,
                    std::uint8_t fp, const Header& hdr) {
    const std::size_t elem = entry_size(field.size(), value.size());
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
    write_entry_bytes(next.entries + hdr.len, field, value);
    free_blob(p_);
    p_ = np;
  }

  void replace_with_entries(std::span<const PendingEntry> entries) {
    std::size_t entries_len = 0;
    for (const auto& entry : entries) {
      entries_len += entry_size(entry.field_size(), entry.value_size());
    }
    const auto count = static_cast<std::uint16_t>(entries.size());
    const auto packed_len = entries_len;
    char* np = alloc_blob(packed_len, count);
    write_header(np, packed_len, count);
    const Header next = unpack_blob(np);
    std::uint16_t offset = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto& entry = entries[index];
      next.fingerprints[index] = entry.fingerprint;
      write_offset(next, index, offset);
      write_entry_bytes(next.entries + offset, entry);
      offset = static_cast<std::uint16_t>(
          offset + entry_size(entry.field_size(), entry.value_size()));
    }
    free_blob(p_);
    p_ = np;
  }

  char* p_ = nullptr;
  std::uintptr_t storage_flags_{default_storage_flags()};
};

}  // namespace goblin::core
