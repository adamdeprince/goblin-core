#pragma once

// Hot-path SIMD helpers for Swiss-table probes, member compare, and RESP CRLF
// scan. No runtime CPU dispatch: the SIMD path is selected at compile time for
// the target ISA (AVX2 on x86-64 when enabled, else SSE2; NEON on AArch64)
// with a scalar fallback otherwise.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace goblin::core::simd {

// Swiss-table group probe: bit i set when group[i] == needle. Low 16 bits valid.
// Always 128-bit on x86 (AVX2 builds use SSE2 here; wider groups measured slower).
[[nodiscard]] inline std::uint64_t match_control_group_16(
    const std::uint8_t* group, std::uint8_t needle) noexcept {
#if defined(__SSE2__)
  const __m128i control =
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(group));
  const __m128i equal =
      _mm_cmpeq_epi8(control, _mm_set1_epi8(static_cast<char>(needle)));
  return static_cast<std::uint64_t>(
      static_cast<unsigned>(_mm_movemask_epi8(equal)) & 0xFFFFU);
#elif defined(__aarch64__)
  static constexpr std::uint8_t kSlotBits[16] = {
      1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t control = vld1q_u8(group);
  const uint8x16_t equal = vceqq_u8(control, vdupq_n_u8(needle));
  const uint8x16_t masked = vandq_u8(equal, vld1q_u8(kSlotBits));
  const std::uint32_t low = vaddv_u8(vget_low_u8(masked));
  const std::uint32_t high = vaddv_u8(vget_high_u8(masked));
  return static_cast<std::uint64_t>(low | (high << 8));
#else
  std::uint64_t mask = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    mask |= static_cast<std::uint64_t>(group[i] == needle) << i;
  }
  return mask;
#endif
}

// Length-tiered compare: AVX2 32 B, then SSE2/NEON 16 B, then memcmp tail.
// Lengths below 16 skip SIMD to avoid over-reading past the buffer end.
[[nodiscard]] inline bool bytes_equal_n(const void* a, const void* b,
                                        std::size_t len) noexcept {
  if (len == 0) {
    return true;
  }
  const auto* pa = static_cast<const std::uint8_t*>(a);
  const auto* pb = static_cast<const std::uint8_t*>(b);
  std::size_t offset = 0;
#if defined(__AVX2__)
  while (offset + 32 <= len) {
    const __m256i va =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pa + offset));
    const __m256i vb =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(pb + offset));
    const __m256i cmp = _mm256_cmpeq_epi8(va, vb);
    if (static_cast<unsigned>(_mm256_movemask_epi8(cmp)) != 0xFFFFFFFFU) {
      return false;
    }
    offset += 32;
  }
#endif
#if defined(__SSE2__)
  while (offset + 16 <= len) {
    const __m128i va =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(pa + offset));
    const __m128i vb =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(pb + offset));
    const __m128i cmp = _mm_cmpeq_epi8(va, vb);
    if (_mm_movemask_epi8(cmp) != 0xFFFF) {
      return false;
    }
    offset += 16;
  }
#elif defined(__aarch64__)
  while (offset + 16 <= len) {
    const uint8x16_t va = vld1q_u8(pa + offset);
    const uint8x16_t vb = vld1q_u8(pb + offset);
    if (vmaxvq_u8(veorq_u8(va, vb)) != 0) {
      return false;
    }
    offset += 16;
  }
#endif
  return std::memcmp(pa + offset, pb + offset, len - offset) == 0;
}

[[nodiscard]] inline bool bytes_equal(std::string_view a,
                                      std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  return bytes_equal_n(a.data(), b.data(), a.size());
}

[[nodiscard]] inline std::optional<std::size_t> find_byte(
    std::string_view buffer, char needle, std::size_t offset = 0) noexcept {
  if (offset >= buffer.size()) {
    return std::nullopt;
  }
  const char* data = buffer.data();
  const std::size_t size = buffer.size();
  std::size_t i = offset;

#if defined(__AVX2__)
  {
    const __m256i needle_v = _mm256_set1_epi8(needle);
    while (i + 32 <= size) {
      const __m256i chunk =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
      std::uint32_t bits = static_cast<std::uint32_t>(
          _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, needle_v)));
      if (bits != 0) {
        return i + static_cast<std::size_t>(std::countr_zero(bits));
      }
      i += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
  const __m128i needle_v = _mm_set1_epi8(needle);
  while (i + 16 <= size) {
    const __m128i chunk =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
    std::uint32_t bits = static_cast<std::uint32_t>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, needle_v)));
    if (bits != 0) {
      return i + static_cast<std::size_t>(std::countr_zero(bits));
    }
    i += 16;
  }
  }
#elif defined(__aarch64__)
  const uint8x16_t needle_v = vdupq_n_u8(static_cast<std::uint8_t>(needle));
  static constexpr std::uint8_t kSlotBits[16] = {
      1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t slot_bits = vld1q_u8(kSlotBits);
  while (i + 16 <= size) {
    const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
    const uint8x16_t is_eq = vceqq_u8(chunk, needle_v);
    const uint8x16_t masked = vandq_u8(is_eq, slot_bits);
    std::uint32_t bits = vaddv_u8(vget_low_u8(masked)) |
                         (vaddv_u8(vget_high_u8(masked)) << 8);
    if (bits != 0) {
      return i + static_cast<std::size_t>(std::countr_zero(bits));
    }
    i += 16;
  }
#endif

  for (; i < size; ++i) {
    if (data[i] == needle) {
      return i;
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::size_t> find_crlf(
    std::string_view buffer, std::size_t offset = 0) noexcept {
  if (offset >= buffer.size()) {
    return std::nullopt;
  }
  const char* data = buffer.data();
  const std::size_t size = buffer.size();
  std::size_t i = offset;

#if defined(__AVX2__)
  {
    const __m256i cr = _mm256_set1_epi8('\r');
    while (i + 32 <= size) {
      const __m256i chunk =
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
      std::uint32_t bits = static_cast<std::uint32_t>(
          _mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, cr)));
      while (bits != 0) {
        const auto pos = static_cast<std::size_t>(std::countr_zero(bits));
        bits &= bits - 1U;
        const std::size_t at = i + pos;
        if (at + 1 < size && data[at + 1] == '\n') {
          return at;
        }
      }
      i += 32;
    }
  }
#endif
#if defined(__SSE2__)
  {
  const __m128i cr = _mm_set1_epi8('\r');
  while (i + 16 <= size) {
    const __m128i chunk =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
    std::uint32_t bits = static_cast<std::uint32_t>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(chunk, cr)));
    while (bits != 0) {
      const auto pos = static_cast<std::size_t>(std::countr_zero(bits));
      bits &= bits - 1U;
      const std::size_t at = i + pos;
      if (at + 1 < size && data[at + 1] == '\n') {
        return at;
      }
    }
    i += 16;
  }
  }
#elif defined(__aarch64__)
  const uint8x16_t cr = vdupq_n_u8('\r');
  static constexpr std::uint8_t kSlotBits[16] = {
      1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t slot_bits = vld1q_u8(kSlotBits);
  while (i + 16 <= size) {
    const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
    const uint8x16_t is_cr = vceqq_u8(chunk, cr);
    const uint8x16_t masked = vandq_u8(is_cr, slot_bits);
    std::uint32_t bits = vaddv_u8(vget_low_u8(masked)) |
                         (vaddv_u8(vget_high_u8(masked)) << 8);
    while (bits != 0) {
      const auto pos = static_cast<std::size_t>(std::countr_zero(bits));
      bits &= bits - 1U;
      const std::size_t at = i + pos;
      if (at + 1 < size && data[at + 1] == '\n') {
        return at;
      }
    }
    i += 16;
  }
#endif

  while (i + 1 < size) {
    if (data[i] == '\r' && data[i + 1] == '\n') {
      return i;
    }
    ++i;
  }
  return std::nullopt;
}

}  // namespace goblin::core::simd