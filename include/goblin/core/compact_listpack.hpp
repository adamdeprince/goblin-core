#pragma once

// A tiny sorted set as ONE pooled allocation -- a compact listpack encoding where
// the count/width/length live in the allocation's header, so the object itself is
// a single pointer. That lets the store hold a tagged pointer per key (key + 8 B)
// instead of a 48 B by-value handle in every swiss slot.
//
// Allocation layout:  [ u32 len ][ u16 count ][ u8 width ][ pad ][ entries... ]
// header = 8 bytes; `entries` (len bytes):
//   [enc: member-length varint][score @ width][member bytes][backlen: reversed enc]
// sorted by (score, member). p_ == nullptr means empty (no allocation yet).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

#include "goblin/core/blob_pool.hpp"
#include "goblin/core/score_width.hpp"
#include "goblin/core/simd_ops.hpp"

namespace goblin::core {

inline constexpr std::size_t kCompactHeaderBytes = 8;  // len(4)+count(2)+width(1)+pad
// The on-wire listpack header (total_bytes u16 + count u16) still caps the blob.
inline constexpr std::size_t kCompactMaxMemberLen = 128 + 0x7FFF;
inline constexpr std::size_t kCompactMaxBlobBytes = 0xFFFF;
inline constexpr std::size_t kCompactWireHeaderBytes = 4;

class CompactListpack {
 public:
  struct AddResult {
    bool changed{false};
    bool needs_full{false};
    bool existed{false};
  };

  CompactListpack() = default;
  ~CompactListpack() { free_blob(p_); }
  CompactListpack(const CompactListpack&) = delete;
  CompactListpack& operator=(const CompactListpack&) = delete;
  CompactListpack(CompactListpack&& other) noexcept
      : p_(std::exchange(other.p_, nullptr)) {}
  CompactListpack& operator=(CompactListpack&& other) noexcept {
    if (this != &other) {
      free_blob(p_);
      p_ = std::exchange(other.p_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return count(); }
  [[nodiscard]] bool empty() const noexcept { return count() == 0; }
  [[nodiscard]] ScoreWidth score_width() const noexcept { return width(); }
  [[nodiscard]] std::size_t blob_bytes() const noexcept {
    return kCompactWireHeaderBytes + len();
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return p_ ? kCompactHeaderBytes + len() : 0;
  }

  // ZADD for one member; needs_full=true (no mutation) if it would exceed the
  // listpack limits (caller rebuilds as a full zset). max_entries is passed by the
  // store (it is a store-global, not stored per blob).
  [[nodiscard]] AddResult add(double score, std::string_view member,
                              std::size_t max_entries) {
    if (member.empty() || member.size() > kCompactMaxMemberLen) {
      return {.needs_full = true};
    }

    Header hdr = unpack();
    const ScoreWidth target = score_width_for(score, hdr.w);
    LocateResult loc = locate(member, score, hdr);

    if (loc.found && loc.old_score == score) {
      return {.changed = false, .needs_full = false, .existed = true};
    }

    const bool exists = loc.found;
    const std::size_t width_delta =
        score_width_bytes(target) - score_width_bytes(hdr.w);
    std::size_t projected = hdr.len + hdr.count * width_delta;
    if (!exists) {
      projected += element_size(member.size(), target);
    }
    if (kCompactWireHeaderBytes + projected > kCompactMaxBlobBytes ||
        (!exists && hdr.count + 1 > max_entries)) {
      return {.changed = false, .needs_full = true, .existed = exists};
    }

    if (exists && target == hdr.w && sort_order_unchanged(score, member, loc)) {
      const auto [len_bytes, member_len] = decode_len(hdr.entries + loc.off);
      (void)member_len;
      write_score(hdr.entries + loc.off + len_bytes, target, score);
      return {.changed = true, .needs_full = false, .existed = true};
    }

    if (target != hdr.w) {
      rewiden(target);
      hdr = unpack();
      if (exists) {
        loc = locate(member, score, hdr);
      }
    }

    if (exists) {
      const std::size_t off = find_member(member, hdr);
      if (target == hdr.w && sort_order_unchanged(score, member, loc)) {
        const auto [len_bytes, member_len] = decode_len(hdr.entries + off);
        (void)member_len;
        write_score(hdr.entries + off + len_bytes, target, score);
        return {.changed = true, .needs_full = false, .existed = true};
      }
      erase_at(off);
    }

    insert_sorted_at(insert_offset(score, member), score, member);
    return {.changed = true, .needs_full = false, .existed = exists};
  }

  [[nodiscard]] std::optional<double> score(std::string_view member) const {
    const Header hdr = unpack();
    const std::size_t off = find_member(member, hdr);
    if (off == kNotFound) {
      return std::nullopt;
    }
    return score_at_element(off, hdr);
  }

  bool remove(std::string_view member) {
    const Header hdr = unpack();
    const std::size_t off = find_member(member, hdr);
    if (off == kNotFound) {
      return false;
    }
    erase_at(off);
    return true;
  }

  [[nodiscard]] std::optional<std::size_t> rank(std::string_view member) const {
    const Header hdr = unpack();
    std::size_t off = 0;
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [len_bytes, member_len] = decode_len(e + off);
      const std::size_t member_off = off + len_bytes + sb;
      if (simd::bytes_equal(std::string_view(e + member_off, member_len),
                            member)) {
        return i;
      }
      off = member_off + member_len + len_bytes;
    }
    return std::nullopt;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    const Header hdr = unpack();
    std::size_t off = 0;
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [len_bytes, member_len] = decode_len(e + off);
      const double s = read_score_at(e + off + len_bytes, hdr.w);
      const std::size_t member_off = off + len_bytes + sb;
      fn(s, std::string_view(e + member_off, member_len));
      off = member_off + member_len + len_bytes;
    }
  }

  template <class Fn>
  void for_range(std::size_t first, std::size_t count_n, bool reverse,
                 Fn&& fn) const {
    if (count_n == 0) {
      return;
    }
    const Header hdr = unpack();
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    if (!reverse) {
      std::size_t off = offset_of_position(first, hdr);
      for (std::size_t i = 0; i < count_n; ++i) {
        const auto [len_bytes, member_len] = decode_len(e + off);
        const double s = read_score_at(e + off + len_bytes, hdr.w);
        const std::size_t member_off = off + len_bytes + sb;
        fn(s, std::string_view(e + member_off, member_len));
        off = member_off + member_len + len_bytes;
      }
    } else {
      std::size_t end = hdr.len;
      for (std::size_t i = 0; i < first && end > 0; ++i) {
        end = prev_element_start(end, hdr);
      }
      for (std::size_t i = 0; i < count_n && end > 0; ++i) {
        const std::size_t start = prev_element_start(end, hdr);
        const auto [len_bytes, member_len] = decode_len(e + start);
        const double s = read_score_at(e + start + len_bytes, hdr.w);
        const std::size_t member_off = start + len_bytes + sb;
        fn(s, std::string_view(e + member_off, member_len));
        end = start;
      }
    }
  }

  void optimize() {
    const Header hdr = unpack();
    if (hdr.count == 0) {
      return;
    }
    ScoreWidth narrowest = ScoreWidth::I16;
    for_each([&narrowest](double s, std::string_view) {
      narrowest = score_width_for(s, narrowest);
    });
    if (narrowest != hdr.w) {
      rewiden(narrowest);
    }
  }

 private:
  static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

  struct Header {
    std::uint32_t len{0};
    std::uint16_t count{0};
    ScoreWidth w{ScoreWidth::I16};
    char* entries{nullptr};
  };

  struct LocateResult {
    bool found{false};
    std::size_t off{kNotFound};
    std::size_t insert_off{0};
    double old_score{0.0};
    bool has_prev{false};
    double prev_score{0.0};
    std::string_view prev_member{};
    bool has_next{false};
    double next_score{0.0};
    std::string_view next_member{};
  };

  [[nodiscard]] Header unpack() const noexcept {
    Header hdr;
    if (!p_) {
      return hdr;
    }
    std::memcpy(&hdr.len, p_, sizeof(hdr.len));
    std::memcpy(&hdr.count, p_ + 4, sizeof(hdr.count));
    hdr.w = static_cast<ScoreWidth>(static_cast<unsigned char>(p_[6]));
    hdr.entries = p_ + kCompactHeaderBytes;
    return hdr;
  }

  [[nodiscard]] std::size_t count() const noexcept { return unpack().count; }
  [[nodiscard]] std::uint32_t len() const noexcept { return unpack().len; }
  [[nodiscard]] ScoreWidth width() const noexcept { return unpack().w; }

  static void write_header(char* p, std::uint32_t len_bytes, std::uint16_t n,
                           ScoreWidth w) noexcept {
    std::memcpy(p, &len_bytes, sizeof(len_bytes));
    std::memcpy(p + 4, &n, sizeof(n));
    p[6] = static_cast<char>(static_cast<unsigned char>(w));
    p[7] = 0;
  }
  [[nodiscard]] static char* alloc_blob(std::uint32_t entries_len) {
    return static_cast<char*>(
        blob_pool().allocate(kCompactHeaderBytes + entries_len, 1));
  }
  static void free_blob(char* p) noexcept {
    if (p) {
      std::uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      blob_pool().deallocate(p, kCompactHeaderBytes + v, 1);
    }
  }

  [[nodiscard]] static std::size_t len_size(std::size_t member_len) noexcept {
    return member_len <= 128 ? 1 : 2;
  }
  [[nodiscard]] static std::size_t element_size(std::size_t member_len,
                                                ScoreWidth w) noexcept {
    return 2 * len_size(member_len) + score_width_bytes(w) + member_len;
  }

  static std::size_t encode_len(char* out, std::size_t member_len) noexcept {
    auto* u = reinterpret_cast<unsigned char*>(out);
    if (member_len <= 128) {
      u[0] = static_cast<unsigned char>(member_len - 1);
      return 1;
    }
    const std::size_t number = member_len - 128;
    u[0] = static_cast<unsigned char>(0x80 | (number >> 8));
    u[1] = static_cast<unsigned char>(number & 0xFF);
    return 2;
  }
  static std::pair<std::size_t, std::size_t> decode_len(const char* p) noexcept {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    if ((u[0] & 0x80) == 0) {
      return {1, static_cast<std::size_t>(u[0]) + 1};
    }
    const std::size_t number = (static_cast<std::size_t>(u[0] & 0x7F) << 8) | u[1];
    return {2, number + 128};
  }
  static std::size_t encode_backlen(char* out, std::size_t member_len) noexcept {
    char forward[2];
    const std::size_t n = encode_len(forward, member_len);
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = forward[n - 1 - i];
    }
    return n;
  }

  [[nodiscard]] static double read_score_at(const char* p,
                                            ScoreWidth w) noexcept {
    switch (w) {
      case ScoreWidth::I16: {
        std::int16_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<double>(v);
      }
      case ScoreWidth::I32: {
        std::int32_t v;
        std::memcpy(&v, p, sizeof(v));
        return static_cast<double>(v);
      }
      case ScoreWidth::F64: {
        double v;
        std::memcpy(&v, p, sizeof(v));
        return v;
      }
    }
    return 0.0;
  }
  static void write_score(char* p, ScoreWidth w, double score) noexcept {
    switch (w) {
      case ScoreWidth::I16: {
        const auto v = static_cast<std::int16_t>(score);
        std::memcpy(p, &v, sizeof(v));
        return;
      }
      case ScoreWidth::I32: {
        const auto v = static_cast<std::int32_t>(score);
        std::memcpy(p, &v, sizeof(v));
        return;
      }
      case ScoreWidth::F64:
        std::memcpy(p, &score, sizeof(score));
        return;
    }
  }

  [[nodiscard]] static bool less_than(double score_a, std::string_view member_a,
                                      double score_b,
                                      std::string_view member_b) noexcept {
    return score_a < score_b || (score_a == score_b && member_a < member_b);
  }

  [[nodiscard]] static bool sort_order_unchanged(
      double new_score, std::string_view member, const LocateResult& loc) noexcept {
    if (loc.has_prev) {
      if (loc.prev_score > new_score) {
        return false;
      }
      if (loc.prev_score == new_score && loc.prev_member >= member) {
        return false;
      }
    }
    if (loc.has_next) {
      if (new_score > loc.next_score) {
        return false;
      }
      if (new_score == loc.next_score && member >= loc.next_member) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] LocateResult locate(std::string_view member, double score,
                                    const Header& hdr) const noexcept {
    LocateResult result;
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    std::size_t off = 0;
    bool has_prev = false;
    bool insert_placed = false;
    double prev_score = 0.0;
    std::string_view prev_member{};

    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [len_bytes, member_len] = decode_len(e + off);
      const double s = read_score_at(e + off + len_bytes, hdr.w);
      const std::size_t member_off = off + len_bytes + sb;
      const std::string_view cur(e + member_off, member_len);

      if (!result.found && !insert_placed && less_than(score, member, s, cur)) {
        result.insert_off = off;
        insert_placed = true;
      }

      if (simd::bytes_equal(std::string_view(e + member_off, member_len),
                            member)) {
        result.found = true;
        result.off = off;
        result.old_score = s;
        result.has_prev = has_prev;
        if (has_prev) {
          result.prev_score = prev_score;
          result.prev_member = prev_member;
        }
        if (i + 1 < hdr.count) {
          const std::size_t next_off =
              member_off + member_len + len_bytes;
          const auto [next_len_bytes, next_member_len] =
              decode_len(e + next_off);
          result.has_next = true;
          result.next_score = read_score_at(e + next_off + next_len_bytes, hdr.w);
          result.next_member =
              std::string_view(e + next_off + next_len_bytes + sb, next_member_len);
        }
        return result;
      }

      has_prev = true;
      prev_score = s;
      prev_member = cur;
      off = member_off + member_len + len_bytes;
    }

    if (!result.found && !insert_placed) {
      result.insert_off = off;
    }
    return result;
  }

  [[nodiscard]] std::size_t insert_offset(double score,
                                          std::string_view member) const {
    const Header hdr = unpack();
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    std::size_t off = 0;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [len_bytes, member_len] = decode_len(e + off);
      const double s = read_score_at(e + off + len_bytes, hdr.w);
      const std::size_t member_off = off + len_bytes + sb;
      const std::string_view cur(e + member_off, member_len);
      if (less_than(score, member, s, cur)) {
        return off;
      }
      off = member_off + member_len + len_bytes;
    }
    return off;
  }

  [[nodiscard]] std::size_t offset_of_position(std::size_t pos,
                                               const Header& hdr) const noexcept {
    std::size_t off = 0;
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < pos; ++i) {
      const auto [len_bytes, member_len] = decode_len(e + off);
      off += 2 * len_bytes + sb + member_len;
    }
    return off;
  }

  [[nodiscard]] std::size_t prev_element_start(std::size_t end,
                                                 const Header& hdr) const noexcept {
    const auto* e = hdr.entries;
    const auto* last = reinterpret_cast<const unsigned char*>(e + end - 1);
    std::size_t member_len;
    std::size_t backlen_size;
    if ((last[0] & 0x80) == 0) {
      member_len = static_cast<std::size_t>(last[0]) + 1;
      backlen_size = 1;
    } else {
      const std::size_t number =
          (static_cast<std::size_t>(last[0] & 0x7F) << 8) | last[-1];
      member_len = number + 128;
      backlen_size = 2;
    }
    return end - (2 * backlen_size + score_width_bytes(hdr.w) + member_len);
  }

  [[nodiscard]] double score_at_element(std::size_t off,
                                        const Header& hdr) const noexcept {
    const auto [len_bytes, member_len] = decode_len(hdr.entries + off);
    (void)member_len;
    return read_score_at(hdr.entries + off + len_bytes, hdr.w);
  }

  [[nodiscard]] std::size_t find_member(std::string_view member) const noexcept {
    return find_member(member, unpack());
  }

  [[nodiscard]] std::size_t find_member(std::string_view member,
                                        const Header& hdr) const noexcept {
    std::size_t off = 0;
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto* e = hdr.entries;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [len_bytes, member_len] = decode_len(e + off);
      const std::size_t member_off = off + len_bytes + sb;
      if (simd::bytes_equal(std::string_view(e + member_off, member_len),
                            member)) {
        return off;
      }
      off = member_off + member_len + len_bytes;
    }
    return kNotFound;
  }

  void erase_at(std::size_t off) {
    const Header hdr = unpack();
    const std::size_t sb = score_width_bytes(hdr.w);
    const auto [len_bytes, member_len] = decode_len(hdr.entries + off);
    const std::size_t elem = 2 * len_bytes + sb + member_len;
    const std::uint32_t new_len = hdr.len - static_cast<std::uint32_t>(elem);
    char* np = alloc_blob(new_len);
    write_header(np, new_len, static_cast<std::uint16_t>(hdr.count - 1), hdr.w);
    std::memcpy(np + kCompactHeaderBytes, hdr.entries, off);
    std::memcpy(np + kCompactHeaderBytes + off, hdr.entries + off + elem,
                hdr.len - off - elem);
    free_blob(p_);
    p_ = np;
  }

  void insert_sorted_at(std::size_t off, double score, std::string_view member) {
    const Header hdr = unpack();
    const std::size_t sb = score_width_bytes(hdr.w);
    const std::size_t member_len = member.size();
    const std::size_t elem = element_size(member_len, hdr.w);
    const std::uint32_t new_len = hdr.len + static_cast<std::uint32_t>(elem);
    char* np = alloc_blob(new_len);
    write_header(np, new_len, static_cast<std::uint16_t>(hdr.count + 1), hdr.w);
    char* dst = np + kCompactHeaderBytes;
    if (off != 0) {
      std::memcpy(dst, hdr.entries, off);
    }
    char* el = dst + off;
    std::size_t k = 0;
    k += encode_len(el + k, member_len);
    write_score(el + k, hdr.w, score);
    k += sb;
    std::memcpy(el + k, member.data(), member_len);
    k += member_len;
    k += encode_backlen(el + k, member_len);
    if (hdr.len > off) {
      std::memcpy(dst + off + elem, hdr.entries + off, hdr.len - off);
    }
    free_blob(p_);
    p_ = np;
  }

  void rewiden(ScoreWidth target) {
    const Header hdr = unpack();
    const std::size_t old_sb = score_width_bytes(hdr.w);
    const std::size_t new_sb = score_width_bytes(target);
    const std::uint32_t new_len =
        hdr.len + static_cast<std::uint32_t>(hdr.count * new_sb) -
        static_cast<std::uint32_t>(hdr.count * old_sb);
    char* np = alloc_blob(new_len);
    write_header(np, new_len, hdr.count, target);
    const auto* src = hdr.entries;
    char* dst = np + kCompactHeaderBytes;
    std::size_t so = 0;
    std::size_t doff = 0;
    for (std::size_t i = 0; i < hdr.count; ++i) {
      const auto [len_bytes, member_len] = decode_len(src + so);
      const double s = read_score_at(src + so + len_bytes, hdr.w);
      const std::size_t member_off = so + len_bytes + old_sb;
      std::memcpy(dst + doff, src + so, len_bytes);
      doff += len_bytes;
      write_score(dst + doff, target, s);
      doff += new_sb;
      std::memcpy(dst + doff, src + member_off, member_len + len_bytes);
      doff += member_len + len_bytes;
      so = member_off + member_len + len_bytes;
    }
    free_blob(p_);
    p_ = np;
  }

  char* p_ = nullptr;
};

}  // namespace goblin::core