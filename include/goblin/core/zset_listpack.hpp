#pragma once

// A tiny sorted set stored as one contiguous blob -- our own "listpack" for small
// zsets, so a handful of members costs ~a hundred bytes instead of the full swiss
// + score index + SoA + CoW machinery. Not Redis's listpack format.
//
// Layout (in memory data_ holds the entries; the 4-byte header is added only when
// serialized): entries sorted by (score, member), each:
//
//     [enc][score][member bytes][backlen]
//
//   enc   member-length varint: first byte in [0,127] (MSB 0) -> length in [1,128];
//         MSB 1 -> ((low 7 bits) << 8 | next byte) + 128 (two bytes).
//   score inline at the blob's width -- i16 / i32 / f64 (three variants), widened
//         one-way like the full zset so chess/integer scores stay 2 bytes.
//   backlen the same member-length varint with its bytes REVERSED, so a reverse
//         walk reads the trailing byte first and can tell 1- vs 2-byte; the whole
//         element size is derived (2*enc_size + score_size + member_len).
//
// It promotes to the full arena-shaped zset when a member exceeds the varint max,
// the blob would exceed 64 KiB, or the element count would exceed a configurable
// max. Operations are linear scans / in-place memmoves, cache-optimal at small n.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "goblin/core/score_width.hpp"

namespace goblin::core {

// Largest member a listpack entry can encode (128 + 15-bit varint). Longer members
// force a promotion to the full zset.
inline constexpr std::size_t kListpackMaxMemberLen = 128 + 0x7FFF;
// The 4-byte header's total_bytes is a u16, so the whole blob is capped at 64 KiB.
inline constexpr std::size_t kListpackMaxBlobBytes = 0xFFFF;
inline constexpr std::size_t kListpackHeaderBytes = 4;
inline constexpr std::size_t kDefaultListpackMaxEntries = 512;

class ZSetListpack {
 public:
  // Result of an add: whether the set changed, and whether the caller must convert
  // to the full zset first (the add did NOT happen in that case).
  struct AddResult {
    bool changed{false};
    bool needs_full{false};
  };

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
  [[nodiscard]] ScoreWidth score_width() const noexcept { return width_; }
  [[nodiscard]] std::size_t blob_bytes() const noexcept {
    return kListpackHeaderBytes + data_.size();
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return data_.capacity();
  }
  void set_max_entries(std::size_t max_entries) noexcept {
    max_entries_ = max_entries;
  }

  // ZADD semantics for one member. Returns needs_full=true (without mutating) if the
  // member/blob/count would exceed the listpack's limits -- the caller then rebuilds
  // as a full zset. Widens the score width in place when required.
  [[nodiscard]] AddResult add(double score, std::string_view member) {
    // The 1-byte length form maps to [1, 128] (byte = len - 1), so a length-0
    // (empty) member has no encoding; over-long members don't fit. Both promote
    // to the full structure rather than corrupt the blob.
    if (member.empty() || member.size() > kListpackMaxMemberLen) {
      return {.changed = false, .needs_full = true};
    }
    const ScoreWidth target = score_width_for(score, width_);
    const std::size_t found = find_member(member);
    const bool exists = found != kNotFound;
    if (exists && score_at_element(found) == score) {
      return {.changed = false, .needs_full = false};  // unchanged
    }

    // Blob size after: existing scores grow by the width delta; a brand-new member
    // adds a whole element (an update keeps the same element size).
    const std::size_t width_delta =
        score_width_bytes(target) - score_width_bytes(width_);
    std::size_t projected = data_.size() + count_ * width_delta;
    if (!exists) {
      projected += element_size(member.size(), target);
    }
    if (kListpackHeaderBytes + projected > kListpackMaxBlobBytes ||
        (!exists && count_ + 1 > max_entries_)) {
      return {.changed = false, .needs_full = true};
    }

    if (target != width_) {
      rewiden(target);
    }
    if (exists) {
      erase_at(find_member(member));  // offset may have moved after rewiden
    }
    insert_sorted(score, member);
    return {.changed = true, .needs_full = false};
  }

  [[nodiscard]] std::optional<double> score(std::string_view member) const {
    const std::size_t off = find_member(member);
    if (off == kNotFound) {
      return std::nullopt;
    }
    return score_at_element(off);
  }

  bool remove(std::string_view member) {
    const std::size_t off = find_member(member);
    if (off == kNotFound) {
      return false;
    }
    erase_at(off);
    return true;
  }

  // 0-based rank (position in sorted order), or nullopt if absent.
  [[nodiscard]] std::optional<std::size_t> rank(std::string_view member) const {
    std::size_t off = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const auto [len_bytes, member_len] = decode_len(&data_[off]);
      const std::size_t member_off = off + len_bytes + score_width_bytes(width_);
      if (member_len == member.size() &&
          std::memcmp(&data_[member_off], member.data(), member_len) == 0) {
        return i;
      }
      off = member_off + member_len + len_bytes;
    }
    return std::nullopt;
  }

  // Visit entries in sorted order: fn(double score, string_view member).
  template <class Fn>
  void for_each(Fn&& fn) const {
    std::size_t off = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const auto [len_bytes, member_len] = decode_len(&data_[off]);
      const double s = read_score(&data_[off + len_bytes]);
      const std::size_t member_off = off + len_bytes + score_width_bytes(width_);
      fn(s, std::string_view(&data_[member_off], member_len));
      off = member_off + member_len + len_bytes;
    }
  }

  // Visit positions [first, first+count) forward (reverse=false) or in descending
  // order from the end (reverse=true, using the backlen for O(1) reverse steps):
  // fn(double score, string_view member).
  template <class Fn>
  void for_range(std::size_t first, std::size_t count, bool reverse,
                 Fn&& fn) const {
    if (count == 0) {
      return;
    }
    const std::size_t score_bytes = score_width_bytes(width_);
    if (!reverse) {
      std::size_t off = offset_of_position(first);
      for (std::size_t i = 0; i < count; ++i) {
        const auto [len_bytes, member_len] = decode_len(&data_[off]);
        const double s = read_score(&data_[off + len_bytes]);
        const std::size_t member_off = off + len_bytes + score_bytes;
        fn(s, std::string_view(&data_[member_off], member_len));
        off = member_off + member_len + len_bytes;
      }
    } else {
      std::size_t end = data_.size();
      for (std::size_t i = 0; i < first && end > 0; ++i) {
        end = prev_element_start(end);
      }
      for (std::size_t i = 0; i < count && end > 0; ++i) {
        const std::size_t start = prev_element_start(end);
        const auto [len_bytes, member_len] = decode_len(&data_[start]);
        const double s = read_score(&data_[start + len_bytes]);
        const std::size_t member_off = start + len_bytes + score_bytes;
        fn(s, std::string_view(&data_[member_off], member_len));
        end = start;
      }
    }
  }

  // Re-derive the narrowest width that holds every score and re-encode if it
  // narrowed -- GOBLIN.OPTIMIZE's demote, mirroring the full zset's rebuild --
  // then return the blob's geometric-growth slack to the allocator.
  void optimize() {
    if (count_ == 0) {
      data_.shrink_to_fit();
      return;
    }
    ScoreWidth narrowest = ScoreWidth::I16;
    for_each([&narrowest](double score, std::string_view) {
      narrowest = score_width_for(score, narrowest);
    });
    if (narrowest != width_) {
      rewiden(narrowest);
    }
    data_.shrink_to_fit();
  }

 private:
  static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

  [[nodiscard]] static std::size_t len_size(std::size_t member_len) noexcept {
    return member_len <= 128 ? 1 : 2;
  }
  [[nodiscard]] static std::size_t element_size(std::size_t member_len,
                                                ScoreWidth width) noexcept {
    return 2 * len_size(member_len) + score_width_bytes(width) + member_len;
  }

  // Forward member-length varint. Returns bytes written (1 or 2).
  static std::size_t encode_len(char* out, std::size_t member_len) noexcept {
    auto* u = reinterpret_cast<unsigned char*>(out);
    if (member_len <= 128) {
      u[0] = static_cast<unsigned char>(member_len - 1);
      return 1;
    }
    const std::size_t number = member_len - 128;  // 1..0x7FFF
    u[0] = static_cast<unsigned char>(0x80 | (number >> 8));
    u[1] = static_cast<unsigned char>(number & 0xFF);
    return 2;
  }
  // Returns {bytes, member_len}.
  static std::pair<std::size_t, std::size_t> decode_len(const char* p) noexcept {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    if ((u[0] & 0x80) == 0) {
      return {1, static_cast<std::size_t>(u[0]) + 1};
    }
    const std::size_t number = (static_cast<std::size_t>(u[0] & 0x7F) << 8) | u[1];
    return {2, number + 128};
  }
  // The forward varint with its bytes reversed (for reverse traversal).
  static std::size_t encode_backlen(char* out, std::size_t member_len) noexcept {
    char forward[2];
    const std::size_t n = encode_len(forward, member_len);
    for (std::size_t i = 0; i < n; ++i) {
      out[i] = forward[n - 1 - i];
    }
    return n;
  }

  [[nodiscard]] double read_score(const char* p) const noexcept {
    switch (width_) {
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
  static void write_score(char* p, ScoreWidth width, double score) noexcept {
    switch (width) {
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

  // data_ offset of the element at sorted position `pos`.
  [[nodiscard]] std::size_t offset_of_position(std::size_t pos) const noexcept {
    std::size_t off = 0;
    const std::size_t score_bytes = score_width_bytes(width_);
    for (std::size_t i = 0; i < pos; ++i) {
      const auto [len_bytes, member_len] = decode_len(&data_[off]);
      off += 2 * len_bytes + score_bytes + member_len;
    }
    return off;
  }

  // Given the end offset of an element, return its start offset via the reversed
  // backlen (its trailing byte reveals 1- vs 2-byte).
  [[nodiscard]] std::size_t prev_element_start(std::size_t end) const noexcept {
    const auto* last = reinterpret_cast<const unsigned char*>(&data_[end - 1]);
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
    return end - (2 * backlen_size + score_width_bytes(width_) + member_len);
  }

  // The inline score of the element starting at data_ offset `off`.
  [[nodiscard]] double score_at_element(std::size_t off) const noexcept {
    const auto [len_bytes, member_len] = decode_len(&data_[off]);
    (void)member_len;
    return read_score(&data_[off + len_bytes]);
  }

  // Offset in data_ of the entry whose member equals `member`, else kNotFound.
  [[nodiscard]] std::size_t find_member(std::string_view member) const noexcept {
    std::size_t off = 0;
    const std::size_t score_bytes = score_width_bytes(width_);
    for (std::size_t i = 0; i < count_; ++i) {
      const auto [len_bytes, member_len] = decode_len(&data_[off]);
      const std::size_t member_off = off + len_bytes + score_bytes;
      if (member_len == member.size() &&
          std::memcmp(&data_[member_off], member.data(), member_len) == 0) {
        return off;
      }
      off = member_off + member_len + len_bytes;
    }
    return kNotFound;
  }

  // Remove the whole element starting at data_ offset `off`.
  void erase_at(std::size_t off) {
    const auto [len_bytes, member_len] = decode_len(&data_[off]);
    const std::size_t elem = 2 * len_bytes + score_width_bytes(width_) + member_len;
    data_.erase(off, elem);
    data_.shrink_to_fit();
    --count_;
  }

  // Insert (score, member) at its sorted (score, member) position.
  void insert_sorted(double score, std::string_view member) {
    std::size_t off = 0;
    const std::size_t score_bytes = score_width_bytes(width_);
    for (std::size_t i = 0; i < count_; ++i) {
      const auto [len_bytes, member_len] = decode_len(&data_[off]);
      const double s = read_score(&data_[off + len_bytes]);
      const std::size_t member_off = off + len_bytes + score_bytes;
      const std::string_view cur(&data_[member_off], member_len);
      if (s > score || (s == score && cur > member)) {
        break;  // insert before this entry
      }
      off = member_off + member_len + len_bytes;
    }

    const std::size_t member_len = member.size();
    const std::size_t lb = len_size(member_len);
    std::string element(element_size(member_len, width_), '\0');
    std::size_t w = 0;
    w += encode_len(&element[w], member_len);
    write_score(&element[w], width_, score);
    w += score_bytes;
    std::memcpy(&element[w], member.data(), member_len);
    w += member_len;
    w += encode_backlen(&element[w], member_len);
    (void)lb;
    data_.insert(off, element);
    data_.shrink_to_fit();
    ++count_;
  }

  // Re-encode every entry's inline score to a wider width (one-way).
  void rewiden(ScoreWidth target) {
    const std::size_t old_score_bytes = score_width_bytes(width_);
    const std::size_t new_score_bytes = score_width_bytes(target);
    std::string out;
    // Written so narrowing (new < old) can't underflow the unsigned subtraction.
    out.reserve(data_.size() + count_ * new_score_bytes - count_ * old_score_bytes);
    std::size_t off = 0;
    for (std::size_t i = 0; i < count_; ++i) {
      const auto [len_bytes, member_len] = decode_len(&data_[off]);
      const double s = read_score(&data_[off + len_bytes]);
      const std::size_t member_off = off + len_bytes + old_score_bytes;
      // enc
      out.append(&data_[off], len_bytes);
      // score at the new width
      char score_buf[8];
      write_score(score_buf, target, s);
      out.append(score_buf, new_score_bytes);
      // member + backlen
      out.append(&data_[member_off], member_len + len_bytes);
      off = member_off + member_len + len_bytes;
    }
    data_ = std::move(out);
    width_ = target;
  }

  std::string data_;  // entries only; the 4-byte header is synthesized on save
  std::size_t max_entries_{kDefaultListpackMaxEntries};
  std::uint16_t count_{0};
  ScoreWidth width_{ScoreWidth::I16};
};

}  // namespace goblin::core
