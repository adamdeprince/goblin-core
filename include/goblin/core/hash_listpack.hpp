#pragma once

// A tiny hash as ONE pooled allocation -- the hash analogue of CompactListpack.
// Unordered field->value table; linear scan (cache-friendly at small n).
//
// Allocation layout:  [ u32 entries_len ][ u16 count ][ u8 pad ][ u8 pad ][ entries... ]
// header = 8 bytes (same shape as CompactListpack so the blob pool is shared).
//
// Each entry (fixed u16 lengths -- goblin strings are capped at 64 KiB):
//   [ field_len: u16 LE ][ value_len: u16 LE ][ field bytes ][ value bytes ]
//
// Empty field/value are legal (length 0). No backlen: the table is unordered and
// only walked forward. max_entries is a store-global passed into set(), not stored
// in the blob (same discipline as zset listpack).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

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
    return p_ ? kHashListpackHeaderBytes + len() : 0;
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
    const std::size_t found = find_field(field, hdr);
    const std::size_t new_elem = entry_size(field.size(), value.size());

    if (found != kNotFound) {
      const auto [flen, vlen] = lengths_at(hdr.entries + found);
      const std::size_t old_elem = entry_size(flen, vlen);
      if (value.size() == vlen) {
        // Same value width: overwrite in place (field bytes untouched).
        char* val = p_ + kHashListpackHeaderBytes + found + 4 + flen;
        if (!value.empty()) {
          std::memcpy(val, value.data(), value.size());
        }
        return {.added = false, .needs_full = false};
      }
      // Different size: project erase + re-append.
      const std::size_t projected = hdr.len - old_elem + new_elem;
      if (kHashListpackHeaderBytes + projected > kHashListpackMaxBlobBytes) {
        return {.added = false, .needs_full = true};
      }
      erase_at(found);
      append_entry(field, value);
      return {.added = false, .needs_full = false};
    }

    // New field.
    if (hdr.count + 1 > max_entries ||
        kHashListpackHeaderBytes + hdr.len + new_elem > kHashListpackMaxBlobBytes) {
      return {.added = false, .needs_full = true};
    }
    append_entry(field, value);
    return {.added = true, .needs_full = false};
  }

  [[nodiscard]] std::optional<std::string_view> get(
      std::string_view field) const {
    const Header hdr = unpack();
    const std::size_t off = find_field(field, hdr);
    if (off == kNotFound) {
      return std::nullopt;
    }
    const auto [flen, vlen] = lengths_at(hdr.entries + off);
    if (vlen == 0) {
      return std::string_view{};
    }
    return std::string_view(hdr.entries + off + 4 + flen, vlen);
  }

  [[nodiscard]] bool contains(std::string_view field) const {
    return find_field(field, unpack()) != kNotFound;
  }

  // HDEL. Returns true if the field was present.
  bool erase(std::string_view field) {
    const Header hdr = unpack();
    const std::size_t off = find_field(field, hdr);
    if (off == kNotFound) {
      return false;
    }
    erase_at(off);
    return true;
  }

  // HSETNX helper: true if field is absent (caller then set()).
  [[nodiscard]] bool absent(std::string_view field) const {
    return find_field(field, unpack()) == kNotFound;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    const Header hdr = unpack();
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

 private:
  static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

  struct Header {
    std::uint32_t len{0};
    std::uint16_t count{0};
    char* entries{nullptr};
  };

  [[nodiscard]] Header unpack() const noexcept {
    Header hdr;
    if (!p_) {
      return hdr;
    }
    std::memcpy(&hdr.len, p_, sizeof(hdr.len));
    std::memcpy(&hdr.count, p_ + 4, sizeof(hdr.count));
    hdr.entries = p_ + kHashListpackHeaderBytes;
    return hdr;
  }

  [[nodiscard]] std::size_t count() const noexcept { return unpack().count; }
  [[nodiscard]] std::uint32_t len() const noexcept { return unpack().len; }

  static void write_header(char* p, std::uint32_t entries_len,
                           std::uint16_t n) noexcept {
    std::memcpy(p, &entries_len, sizeof(entries_len));
    std::memcpy(p + 4, &n, sizeof(n));
    p[6] = 0;
    p[7] = 0;
  }

  [[nodiscard]] static char* alloc_blob(std::uint32_t entries_len) {
    return static_cast<char*>(
        blob_pool().allocate(kHashListpackHeaderBytes + entries_len, 1));
  }
  static void free_blob(char* p) noexcept {
    if (p) {
      std::uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      blob_pool().deallocate(p, kHashListpackHeaderBytes + v, 1);
    }
  }

  [[nodiscard]] static std::size_t entry_size(std::size_t field_len,
                                              std::size_t value_len) noexcept {
    return 4 + field_len + value_len;
  }

  [[nodiscard]] static std::pair<std::size_t, std::size_t> lengths_at(
      const char* e) noexcept {
    std::uint16_t flen = 0;
    std::uint16_t vlen = 0;
    std::memcpy(&flen, e, sizeof(flen));
    std::memcpy(&vlen, e + 2, sizeof(vlen));
    return {flen, vlen};
  }

  [[nodiscard]] std::size_t find_field(std::string_view field,
                                       const Header& hdr) const noexcept {
    std::size_t off = 0;
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [flen, vlen] = lengths_at(e + off);
      if (simd::bytes_equal(std::string_view(e + off + 4, flen), field)) {
        return off;
      }
      off += entry_size(flen, vlen);
    }
    return kNotFound;
  }

  void erase_at(std::size_t off) {
    const Header hdr = unpack();
    const auto [flen, vlen] = lengths_at(hdr.entries + off);
    const std::size_t elem = entry_size(flen, vlen);
    const std::uint32_t new_len = hdr.len - static_cast<std::uint32_t>(elem);
    if (new_len == 0) {
      free_blob(p_);
      p_ = nullptr;
      return;
    }
    char* np = alloc_blob(new_len);
    write_header(np, new_len, static_cast<std::uint16_t>(hdr.count - 1));
    std::memcpy(np + kHashListpackHeaderBytes, hdr.entries, off);
    std::memcpy(np + kHashListpackHeaderBytes + off, hdr.entries + off + elem,
                hdr.len - off - elem);
    free_blob(p_);
    p_ = np;
  }

  void append_entry(std::string_view field, std::string_view value) {
    const Header hdr = unpack();
    const std::size_t elem = entry_size(field.size(), value.size());
    const std::uint32_t new_len = hdr.len + static_cast<std::uint32_t>(elem);
    char* np = alloc_blob(new_len);
    write_header(np, new_len, static_cast<std::uint16_t>(hdr.count + 1));
    char* dst = np + kHashListpackHeaderBytes;
    if (hdr.len != 0) {
      std::memcpy(dst, hdr.entries, hdr.len);
    }
    char* el = dst + hdr.len;
    const auto flen = static_cast<std::uint16_t>(field.size());
    const auto vlen = static_cast<std::uint16_t>(value.size());
    std::memcpy(el, &flen, sizeof(flen));
    std::memcpy(el + 2, &vlen, sizeof(vlen));
    if (!field.empty()) {
      std::memcpy(el + 4, field.data(), field.size());
    }
    if (!value.empty()) {
      std::memcpy(el + 4 + field.size(), value.data(), value.size());
    }
    free_blob(p_);
    p_ = np;
  }

  char* p_ = nullptr;
};

}  // namespace goblin::core
