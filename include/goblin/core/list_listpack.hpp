#pragma once

// A small Redis list in one pooled allocation.
//
// Layout: [ u32 entries_len ][ u16 count ][ u16 reserved ][ entries... ]
// Entry:  [ u16 value_len ][ value bytes ]
//
// Goblin's 64 KiB string ceiling makes the fixed two-byte length sufficient.
// There is no per-entry pointer, allocator header, or back-length. Small-list
// mutations rebuild the one blob; the representation promotes before that
// linear copy becomes expensive.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "goblin/core/blob_pool.hpp"

namespace goblin::core {

inline constexpr std::size_t kListListpackHeaderBytes = 8;
inline constexpr std::size_t kListListpackMaxBlobBytes = 0xFFFF;
inline constexpr std::size_t kListListpackMaxValueBytes =
    std::numeric_limits<std::uint16_t>::max();

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

  [[nodiscard]] std::string_view at(std::size_t index) const noexcept {
    const auto hdr = unpack();
    assert(index < hdr.count);
    const char* entry = hdr.entries + offset_of(index, hdr);
    const auto length = read_u16(entry);
    return std::string_view(entry + sizeof(std::uint16_t), length);
  }

  // Returns false without mutation when the entry/count/blob ceiling would be
  // crossed; the caller then promotes to the full PMA representation.
  [[nodiscard]] bool insert(std::size_t index, std::string_view value,
                            std::size_t max_entries) {
    const auto hdr = unpack();
    assert(index <= hdr.count);
    if (value.size() > kListListpackMaxValueBytes ||
        static_cast<std::size_t>(hdr.count) + 1 > max_entries ||
        kListListpackHeaderBytes + hdr.len + entry_size(value.size()) >
            kListListpackMaxBlobBytes) {
      return false;
    }

    const auto insert_offset = offset_of(index, hdr);
    const auto added = entry_size(value.size());
    const auto new_len = hdr.len + static_cast<std::uint32_t>(added);
    char* next = alloc_blob(new_len);
    write_header(next, new_len, static_cast<std::uint16_t>(hdr.count + 1));
    char* dst = next + kListListpackHeaderBytes;
    if (insert_offset != 0) {
      std::memcpy(dst, hdr.entries, insert_offset);
    }
    write_entry(dst + insert_offset, value);
    if (insert_offset != hdr.len) {
      std::memcpy(dst + insert_offset + added, hdr.entries + insert_offset,
                  hdr.len - insert_offset);
    }
    replace_blob(next);
    return true;
  }

  [[nodiscard]] bool set(std::size_t index, std::string_view value) {
    const auto hdr = unpack();
    assert(index < hdr.count);
    if (value.size() > kListListpackMaxValueBytes) {
      return false;
    }
    const auto offset = offset_of(index, hdr);
    const auto old_length = read_u16(hdr.entries + offset);
    const auto old_size = entry_size(old_length);
    const auto new_size = entry_size(value.size());
    const auto projected = hdr.len - old_size + new_size;
    if (kListListpackHeaderBytes + projected > kListListpackMaxBlobBytes) {
      return false;
    }
    if (old_length == value.size()) {
      if (!value.empty()) {
        std::memcpy(p_ + kListListpackHeaderBytes + offset + 2, value.data(),
                    value.size());
      }
      return true;
    }

    char* next = alloc_blob(static_cast<std::uint32_t>(projected));
    write_header(next, static_cast<std::uint32_t>(projected), hdr.count);
    char* dst = next + kListListpackHeaderBytes;
    if (offset != 0) {
      std::memcpy(dst, hdr.entries, offset);
    }
    write_entry(dst + offset, value);
    const auto tail = hdr.len - offset - old_size;
    if (tail != 0) {
      std::memcpy(dst + offset + new_size, hdr.entries + offset + old_size, tail);
    }
    replace_blob(next);
    return true;
  }

  [[nodiscard]] std::string erase(std::size_t index) {
    const auto hdr = unpack();
    assert(index < hdr.count);
    const auto offset = offset_of(index, hdr);
    const auto length = read_u16(hdr.entries + offset);
    std::string removed(hdr.entries + offset + 2, length);
    erase_at(offset, entry_size(length), hdr);
    return removed;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    const auto hdr = unpack();
    std::size_t offset = 0;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto length = read_u16(hdr.entries + offset);
      fn(std::string_view(hdr.entries + offset + 2, length));
      offset += entry_size(length);
    }
  }

 private:
  struct Header {
    std::uint32_t len{0};
    std::uint16_t count{0};
    const char* entries{nullptr};
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

  [[nodiscard]] static std::uint16_t read_u16(const char* p) noexcept {
    std::uint16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
  }

  static void write_header(char* p, std::uint32_t entries_len,
                           std::uint16_t count) noexcept {
    std::memcpy(p, &entries_len, sizeof(entries_len));
    std::memcpy(p + 4, &count, sizeof(count));
    p[6] = 0;
    p[7] = 0;
  }

  static void write_entry(char* p, std::string_view value) noexcept {
    const auto length = static_cast<std::uint16_t>(value.size());
    std::memcpy(p, &length, sizeof(length));
    if (!value.empty()) {
      std::memcpy(p + sizeof(length), value.data(), value.size());
    }
  }

  [[nodiscard]] static std::size_t entry_size(std::size_t value_length) noexcept {
    return sizeof(std::uint16_t) + value_length;
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
    for (std::size_t i = 0; i < index; ++i) {
      offset += entry_size(read_u16(hdr.entries + offset));
    }
    return offset;
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
};

}  // namespace goblin::core
