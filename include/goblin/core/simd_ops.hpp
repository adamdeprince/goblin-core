#pragma once

// Hot paths call the backend selected once at startup; no CPU probing per call.
#include "goblin/core/simd.hpp"
#include <bit>
#include <cstring>

namespace goblin::core::simd {

[[nodiscard]] inline std::size_t fingerprint_group_width() noexcept {
  return detail::dispatch().fingerprint_width;
}
[[nodiscard]] inline std::uint64_t match_control_group_16(
    const std::uint8_t* group, std::uint8_t needle) noexcept {
  return detail::dispatch().match_control(group, needle);
}
[[nodiscard]] inline std::uint64_t match_hash_group_16(
    const std::uint32_t* hashes, std::uint32_t needle) noexcept {
  return detail::dispatch().match_hash(hashes, needle);
}
[[nodiscard]] inline std::uint64_t match_fingerprint_group(
    const std::uint8_t* group, std::uint8_t needle) noexcept {
  return detail::dispatch().match_fingerprint(group, needle);
}
[[nodiscard]] inline bool bytes_equal_n(const void* a, const void* b,
                                       std::size_t len) noexcept {
  return detail::dispatch().bytes_equal(a, b, len);
}
[[nodiscard]] inline bool bytes_equal(std::string_view a,
                                      std::string_view b) noexcept {
  return a.size() == b.size() && bytes_equal_n(a.data(), b.data(), a.size());
}
[[nodiscard]] inline std::optional<std::size_t> find_byte(
    std::string_view buffer, char needle, std::size_t offset = 0) noexcept {
  return detail::dispatch().find_byte(buffer, needle, offset);
}
[[nodiscard]] inline std::optional<std::size_t> find_crlf(
    std::string_view buffer, std::size_t offset = 0) noexcept {
  return detail::dispatch().find_crlf(buffer, offset);
}
inline void or_u64_words(std::uint64_t* dst, const std::uint64_t* src,
                         std::size_t n) noexcept {
  detail::dispatch().or_words(dst, src, n);
}
[[nodiscard]] inline std::uint64_t hash_bytes(const void* data,
                                             std::size_t size) noexcept {
  return detail::dispatch().hash_bytes(data, size);
}

// Write `dst = src << shift_bits` for a bitset of `nwords` uint64 words.
// Bits at or above `nwords * 64` are dropped. Used with a separate OR pass so
// the 0/1 knapsack step is `dst = bits; shl(dst, bits, w); or(bits, dst)`.
inline void shl_u64_bitset(std::uint64_t* dst, const std::uint64_t* src,
                           std::size_t nwords, std::size_t shift_bits) noexcept {
  if (nwords == 0) {
    return;
  }
  if (shift_bits == 0) {
    std::memcpy(dst, src, nwords * sizeof(std::uint64_t));
    return;
  }
  const std::size_t word_shift = shift_bits / 64;
  const std::size_t bit_shift = shift_bits % 64;
  if (word_shift >= nwords) {
    std::memset(dst, 0, nwords * sizeof(std::uint64_t));
    return;
  }
  // High → low so we can theoretically alias, but we require dst != src.
  std::memset(dst, 0, word_shift * sizeof(std::uint64_t));
  if (bit_shift == 0) {
    std::memcpy(dst + word_shift, src,
                (nwords - word_shift) * sizeof(std::uint64_t));
    return;
  }
  // dst[i] = src[i - word_shift] << bit_shift
  //        | src[i - word_shift - 1] >> (64 - bit_shift)
  for (std::size_t i = nwords; i-- > word_shift;) {
    const std::size_t base = i - word_shift;
    std::uint64_t word = src[base] << bit_shift;
    if (base > 0) {
      word |= src[base - 1] >> (64 - bit_shift);
    }
    dst[i] = word;
  }
}

// bits |= bits << shift_bits into a pre-sized workspace (same nwords).
// Returns via `bits` (reachable sums after adding one 0/1 item of that weight).
inline void bitset_or_shl_u64(std::uint64_t* bits, std::uint64_t* workspace,
                              std::size_t nwords,
                              std::size_t shift_bits) noexcept {
  if (shift_bits == 0 || nwords == 0) {
    return;
  }
  shl_u64_bitset(workspace, bits, nwords, shift_bits);
  or_u64_words(bits, workspace, nwords);
}

// Highest set bit index in [0, max_bit] inclusive, or npos if none.
[[nodiscard]] inline std::size_t bitset_msb_u64(const std::uint64_t* bits,
                                                std::size_t max_bit) noexcept {
  if (bits == nullptr) {
    return static_cast<std::size_t>(-1);
  }
  std::size_t word = max_bit / 64;
  const std::size_t bit = max_bit % 64;
  // Mask off bits above max_bit in the top word (bit==63 → full word).
  std::uint64_t top = bits[word];
  if (bit < 63) {
    top &= (std::uint64_t{1} << (bit + 1)) - 1u;
  }
  if (top != 0) {
    return word * 64 + static_cast<std::size_t>(63 - std::countl_zero(top));
  }
  while (word > 0) {
    --word;
    if (bits[word] != 0) {
      return word * 64 +
             static_cast<std::size_t>(63 - std::countl_zero(bits[word]));
    }
  }
  return static_cast<std::size_t>(-1);
}

[[nodiscard]] inline bool bitset_test_u64(const std::uint64_t* bits,
                                          std::size_t bit) noexcept {
  return (bits[bit / 64] >> (bit % 64)) & 1u;
}

}  // namespace goblin::core::simd
