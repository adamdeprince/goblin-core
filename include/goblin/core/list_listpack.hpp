#pragma once

// A compact Redis list in one pooled allocation.
//
// Layout: [ u32 entries_len ][ u16 count ][ u16 reserved ][ entries... ]
//
// The entry header combines stored length and representation:
//   0x00..0x7e  full encoded payload, length 0..126
//   0x80..0xfe  raw payload with the redundant 0xff omitted, length 0..126
//   0x7f u16    full encoded payload, extended length
//   0xff u16    raw payload, extended length
//
// Thus the common raw 16-byte value occupies 17 bytes total. There is no
// per-entry pointer, allocator header, or back-length. Mutations rebuild the
// one blob; callers cap the entry count before that copy becomes expensive.

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/blob_pool.hpp"
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

inline constexpr std::size_t kListListpackHeaderBytes = 8;
inline constexpr std::size_t kListListpackMaxValueBytes =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::size_t kListListpackMaxBlobBytes =
    kListListpackHeaderBytes + kListListpackMaxValueBytes + 3;

class ListListpack {
 public:
  ListListpack() = default;
  ~ListListpack() { free_blob(p_); }
  ListListpack(const ListListpack&) = delete;
  ListListpack& operator=(const ListListpack&) = delete;
  ListListpack(ListListpack&& other) noexcept
      : p_(std::exchange(other.p_, nullptr)) {}
  ListListpack& operator=(ListListpack&& other) noexcept {
    if (this != &other) {
      free_blob(p_);
      p_ = std::exchange(other.p_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return unpack().count; }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return p_ == nullptr ? 0 : kListListpackHeaderBytes + unpack().len;
  }

  [[nodiscard]] EncodedStringView at(
      std::size_t index, StringEncodingOptions encoding = {}) const noexcept {
    const auto hdr = unpack();
    assert(index < hdr.count);
    return view_entry(hdr.entries + offset_of(index, hdr), encoding);
  }

  [[nodiscard]] bool insert(
      std::size_t index, std::string_view value, std::size_t max_entries,
      StringEncodingOptions encoding = {},
      StringCompressionMode compression = StringCompressionMode::AllowLz4,
      std::size_t max_blob_bytes = kListListpackMaxBlobBytes) {
    const EncodedString encoded(value, encoding, compression);
    const bool raw = encoded.is_raw();
    const bool skip_prefix = raw && encoded.encoding_enabled();
    const auto stored_size =
        encoded.size() - static_cast<std::size_t>(skip_prefix);
    return insert_prepared(
        index, stored_size, raw, max_entries, max_blob_bytes,
        [&encoded, skip_prefix](char* destination) {
          encoded.write_range_to(static_cast<std::size_t>(skip_prefix),
                                 encoded.size() - skip_prefix, destination);
        });
  }

  [[nodiscard]] bool insert_encoded(
      std::size_t index, EncodedStringView value, std::size_t max_entries,
      std::size_t max_blob_bytes = kListListpackMaxBlobBytes) {
    if (!value.valid()) {
      return false;
    }
    const bool raw = value.is_raw();
    const bool skip_prefix = raw && value.encoding_enabled();
    const auto stored_size =
        value.encoded_size() - static_cast<std::size_t>(skip_prefix);
    return insert_prepared(
        index, stored_size, raw, max_entries, max_blob_bytes,
        [&value, skip_prefix](char* destination) {
          value.copy_encoded_range_to(
              static_cast<std::size_t>(skip_prefix),
              value.encoded_size() - skip_prefix, destination);
        });
  }

  // Build a complete listpack with one allocation. This is the hot path for
  // pipelined pushes into segmented lists; repeated insert() would copy the
  // growing leaf once per command argument.
  [[nodiscard]] bool assign(
      std::span<const std::string_view> values, std::size_t max_entries,
      StringEncodingOptions encoding = {},
      StringCompressionMode compression = StringCompressionMode::AllowLz4,
      std::size_t max_blob_bytes = kListListpackMaxBlobBytes) {
    if (values.size() > max_entries ||
        values.size() > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }

    std::vector<EncodedString> encoded;
    encoded.reserve(values.size());
    std::size_t entries_len = 0;
    for (const auto value : values) {
      encoded.emplace_back(value, encoding, compression);
      const auto& item = encoded.back();
      const bool skip_prefix = item.is_raw() && item.encoding_enabled();
      const auto stored_size =
          item.size() - static_cast<std::size_t>(skip_prefix);
      if (stored_size > kListListpackMaxValueBytes ||
          entry_size(stored_size) >
              std::numeric_limits<std::size_t>::max() - entries_len) {
        return false;
      }
      entries_len += entry_size(stored_size);
      if (kListListpackHeaderBytes + entries_len > max_blob_bytes) {
        return false;
      }
    }

    if (encoded.empty()) {
      replace_blob(nullptr);
      return true;
    }

    char* next = alloc_blob(static_cast<std::uint32_t>(entries_len));
    write_header(next, static_cast<std::uint32_t>(entries_len),
                 static_cast<std::uint16_t>(encoded.size()));
    char* destination = next + kListListpackHeaderBytes;
    for (const auto& item : encoded) {
      const bool raw = item.is_raw();
      const bool skip_prefix = raw && item.encoding_enabled();
      const auto stored_size =
          item.size() - static_cast<std::size_t>(skip_prefix);
      write_entry(destination, stored_size, raw,
                  [&item, skip_prefix](char* payload) {
                    item.write_range_to(
                        static_cast<std::size_t>(skip_prefix),
                        item.size() - static_cast<std::size_t>(skip_prefix),
                        payload);
                  });
      destination += entry_size(stored_size);
    }
    replace_blob(next);
    return true;
  }

  // Build directly from values known to use the raw string representation.
  // Their canonical snapshot bytes are already the final stored payload.
  [[nodiscard]] bool assign_raw(
      std::span<const std::string_view> values, std::size_t max_entries,
      std::size_t max_blob_bytes = kListListpackMaxBlobBytes) {
    if (values.size() > max_entries ||
        values.size() > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }

    std::size_t entries_len = 0;
    for (const auto value : values) {
      const auto stored_size = value.size();
      if (stored_size > kListListpackMaxValueBytes ||
          entry_size(stored_size) >
              std::numeric_limits<std::size_t>::max() - entries_len) {
        return false;
      }
      entries_len += entry_size(stored_size);
      if (kListListpackHeaderBytes + entries_len > max_blob_bytes) {
        return false;
      }
    }

    if (values.empty()) {
      replace_blob(nullptr);
      return true;
    }

    char* next = alloc_blob(static_cast<std::uint32_t>(entries_len));
    write_header(next, static_cast<std::uint32_t>(entries_len),
                 static_cast<std::uint16_t>(values.size()));
    char* destination = next + kListListpackHeaderBytes;
    for (const auto value : values) {
      write_entry(destination, value.size(), true,
                  [value](char* payload) {
                    if (!value.empty()) {
                      std::memcpy(payload, value.data(), value.size());
                    }
                  });
      destination += entry_size(value.size());
    }
    replace_blob(next);
    return true;
  }

  [[nodiscard]] bool set(
      std::size_t index, std::string_view value,
      StringEncodingOptions encoding = {},
      StringCompressionMode compression = StringCompressionMode::AllowLz4,
      std::size_t max_blob_bytes = kListListpackMaxBlobBytes) {
    const EncodedString encoded(value, encoding, compression);
    const bool raw = encoded.is_raw();
    const bool skip_prefix = raw && encoded.encoding_enabled();
    const auto stored_size =
        encoded.size() - static_cast<std::size_t>(skip_prefix);
    return set_prepared(
        index, stored_size, raw, max_blob_bytes,
        [&encoded, skip_prefix](char* destination) {
          encoded.write_range_to(static_cast<std::size_t>(skip_prefix),
                                 encoded.size() - skip_prefix, destination);
        });
  }

  [[nodiscard]] std::string erase(std::size_t index,
                                  StringEncodingOptions encoding = {}) {
    const auto hdr = unpack();
    assert(index < hdr.count);
    const auto offset = offset_of(index, hdr);
    const auto entry = read_entry(hdr.entries + offset);
    const auto removed = view_entry(hdr.entries + offset, encoding).to_string();
    erase_at(offset, entry.bytes(), hdr);
    return removed;
  }

  // Concatenate another listpack's entries onto this one with a single
  // allocation. Preserves stored encodings; used by segmented leaf merges.
  [[nodiscard]] bool concat(
      const ListListpack& other, std::size_t max_entries,
      std::size_t max_blob_bytes = kListListpackMaxBlobBytes) {
    const auto self = unpack();
    const auto tail = other.unpack();
    const auto total_count =
        static_cast<std::size_t>(self.count) + tail.count;
    if (total_count > max_entries ||
        total_count > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }
    const auto total_len =
        static_cast<std::size_t>(self.len) + static_cast<std::size_t>(tail.len);
    if (kListListpackHeaderBytes + total_len > max_blob_bytes) {
      return false;
    }
    if (tail.count == 0) {
      return true;
    }
    if (self.count == 0) {
      // Copy other wholesale.
      if (other.p_ == nullptr) {
        replace_blob(nullptr);
        return true;
      }
      char* next = alloc_blob(tail.len);
      std::memcpy(next, other.p_, kListListpackHeaderBytes + tail.len);
      replace_blob(next);
      return true;
    }
    char* next = alloc_blob(static_cast<std::uint32_t>(total_len));
    write_header(next, static_cast<std::uint32_t>(total_len),
                 static_cast<std::uint16_t>(total_count));
    char* dst = next + kListListpackHeaderBytes;
    std::memcpy(dst, self.entries, self.len);
    std::memcpy(dst + self.len, tail.entries, tail.len);
    replace_blob(next);
    return true;
  }

  template <class Fn>
  void for_each(Fn&& fn, StringEncodingOptions encoding = {}) const {
    const auto hdr = unpack();
    std::size_t offset = 0;
    for (std::size_t index = 0; index < hdr.count; ++index) {
      const auto entry = read_entry(hdr.entries + offset);
      fn(view_entry(hdr.entries + offset, encoding));
      offset += entry.bytes();
    }
  }

  template <class Fn>
  void for_range(std::size_t first, std::size_t count, Fn&& fn,
                 StringEncodingOptions encoding = {}) const {
    const auto hdr = unpack();
    if (first >= hdr.count || count == 0) {
      return;
    }
    const auto stop = std::min<std::size_t>(hdr.count, first + count);
    std::size_t offset = offset_of(first, hdr);
    for (std::size_t index = first; index < stop; ++index) {
      const auto entry = read_entry(hdr.entries + offset);
      fn(view_entry(hdr.entries + offset, encoding));
      offset += entry.bytes();
    }
  }

  [[nodiscard]] std::optional<std::size_t> find_first(
      std::string_view value, std::size_t first = 0,
      StringEncodingOptions encoding = {}) const {
    const auto hdr = unpack();
    if (first >= hdr.count) {
      return std::nullopt;
    }
    std::size_t offset = offset_of(first, hdr);
    for (std::size_t index = first; index < hdr.count; ++index) {
      const auto entry = read_entry(hdr.entries + offset);
      if (view_entry(hdr.entries + offset, encoding) == value) {
        return index;
      }
      offset += entry.bytes();
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> find_last(
      std::string_view value, std::size_t end,
      StringEncodingOptions encoding = {}) const {
    const auto hdr = unpack();
    end = std::min<std::size_t>(end, hdr.count);
    std::optional<std::size_t> result;
    std::size_t offset = 0;
    for (std::size_t index = 0; index < end; ++index) {
      const auto entry = read_entry(hdr.entries + offset);
      if (view_entry(hdr.entries + offset, encoding) == value) {
        result = index;
      }
      offset += entry.bytes();
    }
    return result;
  }

 private:
  struct Header {
    std::uint32_t len{0};
    std::uint16_t count{0};
    const char* entries{nullptr};
  };

  struct Entry {
    std::uint16_t length{0};
    std::uint8_t header_bytes{1};
    bool raw{false};

    [[nodiscard]] std::size_t bytes() const noexcept {
      return header_bytes + length;
    }
  };

  [[nodiscard]] Header unpack() const noexcept {
    Header hdr;
    if (p_ == nullptr) {
      return hdr;
    }
    std::memcpy(&hdr.len, p_, sizeof(hdr.len));
    std::memcpy(&hdr.count, p_ + 4, sizeof(hdr.count));
    hdr.entries = p_ + kListListpackHeaderBytes;
    return hdr;
  }

  [[nodiscard]] static Entry read_entry(const char* p) noexcept {
    const auto code = static_cast<std::uint8_t>(*p);
    if (code == 0x7f || code == 0xff) {
      std::uint16_t length = 0;
      std::memcpy(&length, p + 1, sizeof(length));
      return {.length = length, .header_bytes = 3, .raw = code == 0xff};
    }
    return {.length = static_cast<std::uint16_t>(code & 0x7f),
            .header_bytes = 1,
            .raw = (code & 0x80) != 0};
  }

  static void write_header(char* p, std::uint32_t entries_len,
                           std::uint16_t count) noexcept {
    std::memcpy(p, &entries_len, sizeof(entries_len));
    std::memcpy(p + 4, &count, sizeof(count));
    p[6] = 0;
    p[7] = 0;
  }

  [[nodiscard]] static std::size_t entry_header_bytes(
      std::size_t stored_size) noexcept {
    return stored_size <= 126 ? 1 : 3;
  }

  [[nodiscard]] static std::size_t entry_size(
      std::size_t stored_size) noexcept {
    return entry_header_bytes(stored_size) + stored_size;
  }

  template <class Write>
  static void write_entry(char* p, std::size_t stored_size, bool raw,
                          Write&& write) {
    if (stored_size <= 126) {
      p[0] = static_cast<char>(static_cast<std::uint8_t>(stored_size) |
                               (raw ? 0x80 : 0));
      std::forward<Write>(write)(p + 1);
      return;
    }
    p[0] = static_cast<char>(raw ? 0xff : 0x7f);
    const auto length = static_cast<std::uint16_t>(stored_size);
    std::memcpy(p + 1, &length, sizeof(length));
    std::forward<Write>(write)(p + 3);
  }

  [[nodiscard]] static EncodedStringView view_entry(
      const char* p, StringEncodingOptions encoding) noexcept {
    const auto entry = read_entry(p);
    const auto payload =
        std::string_view(p + entry.header_bytes, entry.length);
    if (entry.raw && encoding.encoding_enabled()) {
      return EncodedStringView(std::string_view(&kRawPrefix, 1), payload, true);
    }
    return EncodedStringView(payload, encoding.encoding_enabled());
  }

  [[nodiscard]] static char* alloc_blob(std::uint32_t entries_len) {
    return static_cast<char*>(
        blob_pool().allocate(kListListpackHeaderBytes + entries_len, 1));
  }

  static void free_blob(char* p) noexcept {
    if (p == nullptr) {
      return;
    }
    std::uint32_t entries_len = 0;
    std::memcpy(&entries_len, p, sizeof(entries_len));
    blob_pool().deallocate(p, kListListpackHeaderBytes + entries_len, 1);
  }

  void replace_blob(char* next) noexcept {
    free_blob(p_);
    p_ = next;
  }

  [[nodiscard]] static std::size_t offset_of(std::size_t index,
                                             const Header& hdr) noexcept {
    std::size_t offset = 0;
    for (std::size_t current = 0; current < index; ++current) {
      offset += read_entry(hdr.entries + offset).bytes();
    }
    return offset;
  }

  template <class Write>
  [[nodiscard]] bool insert_prepared(
      std::size_t index, std::size_t stored_size, bool raw,
      std::size_t max_entries, std::size_t max_blob_bytes, Write&& write) {
    const auto hdr = unpack();
    assert(index <= hdr.count);
    const auto added = entry_size(stored_size);
    if (stored_size > kListListpackMaxValueBytes ||
        static_cast<std::size_t>(hdr.count) + 1 > max_entries ||
        kListListpackHeaderBytes + hdr.len + added > max_blob_bytes ||
        hdr.count == std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }

    const auto insert_offset = offset_of(index, hdr);
    const auto new_len = hdr.len + static_cast<std::uint32_t>(added);
    char* next = alloc_blob(new_len);
    write_header(next, new_len, static_cast<std::uint16_t>(hdr.count + 1));
    char* dst = next + kListListpackHeaderBytes;
    if (insert_offset != 0) {
      std::memcpy(dst, hdr.entries, insert_offset);
    }
    write_entry(dst + insert_offset, stored_size, raw,
                std::forward<Write>(write));
    if (insert_offset != hdr.len) {
      std::memcpy(dst + insert_offset + added, hdr.entries + insert_offset,
                  hdr.len - insert_offset);
    }
    replace_blob(next);
    return true;
  }

  template <class Write>
  [[nodiscard]] bool set_prepared(std::size_t index, std::size_t stored_size,
                                  bool raw, std::size_t max_blob_bytes,
                                  Write&& write) {
    const auto hdr = unpack();
    assert(index < hdr.count);
    if (stored_size > kListListpackMaxValueBytes) {
      return false;
    }
    const auto offset = offset_of(index, hdr);
    const auto old = read_entry(hdr.entries + offset);
    const auto new_size = entry_size(stored_size);
    const auto projected = hdr.len - old.bytes() + new_size;
    if (kListListpackHeaderBytes + projected > max_blob_bytes) {
      return false;
    }
    if (old.length == stored_size && old.raw == raw &&
        old.header_bytes == entry_header_bytes(stored_size)) {
      std::forward<Write>(write)(p_ + kListListpackHeaderBytes + offset +
                                 old.header_bytes);
      return true;
    }

    char* next = alloc_blob(static_cast<std::uint32_t>(projected));
    write_header(next, static_cast<std::uint32_t>(projected), hdr.count);
    char* dst = next + kListListpackHeaderBytes;
    if (offset != 0) {
      std::memcpy(dst, hdr.entries, offset);
    }
    write_entry(dst + offset, stored_size, raw, std::forward<Write>(write));
    const auto tail = hdr.len - offset - old.bytes();
    if (tail != 0) {
      std::memcpy(dst + offset + new_size, hdr.entries + offset + old.bytes(),
                  tail);
    }
    replace_blob(next);
    return true;
  }

  void erase_at(std::size_t offset, std::size_t bytes, const Header& hdr) {
    const auto new_len = hdr.len - static_cast<std::uint32_t>(bytes);
    if (new_len == 0) {
      free_blob(p_);
      p_ = nullptr;
      return;
    }
    char* next = alloc_blob(new_len);
    write_header(next, new_len, static_cast<std::uint16_t>(hdr.count - 1));
    char* dst = next + kListListpackHeaderBytes;
    if (offset != 0) {
      std::memcpy(dst, hdr.entries, offset);
    }
    const auto tail = hdr.len - offset - bytes;
    if (tail != 0) {
      std::memcpy(dst + offset, hdr.entries + offset + bytes, tail);
    }
    replace_blob(next);
  }

  char* p_{nullptr};
  static inline constexpr char kRawPrefix = static_cast<char>(kStringRaw);
};

}  // namespace goblin::core
