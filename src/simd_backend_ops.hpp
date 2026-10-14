#pragma once

// Hot-path SIMD helpers for Swiss-table probes, member compare, and RESP CRLF
// scan. Private implementation compiled separately for each runtime backend.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#if defined(GOBLIN_SIMD_AVX512) || defined(GOBLIN_SIMD_AVX2)
#include <immintrin.h>
#elif defined(GOBLIN_SIMD_SSE2)
#include <emmintrin.h>
#elif defined(GOBLIN_SIMD_LASX)
#include <lasxintrin.h>
#include <lsxintrin.h>
#elif defined(GOBLIN_SIMD_LSX)
#include <lsxintrin.h>
#elif defined(GOBLIN_SIMD_NEON)
#include <arm_neon.h>
#endif

namespace goblin::core::simd::GOBLIN_SIMD_BACKEND {

// Swiss-table group probe: bit i set when group[i] == needle. Low 16 bits valid.
// Always 128-bit on x86 (AVX2 builds use SSE2 here; wider groups measured slower).
[[nodiscard]] inline std::uint64_t match_control_group_16(
    const std::uint8_t* group, std::uint8_t needle) noexcept {
#if defined(GOBLIN_SIMD_SSE2)
  const __m128i control =
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(group));
  const __m128i equal =
      _mm_cmpeq_epi8(control, _mm_set1_epi8(static_cast<char>(needle)));
  return static_cast<std::uint64_t>(
      static_cast<unsigned>(_mm_movemask_epi8(equal)) & 0xFFFFU);
#elif defined(GOBLIN_SIMD_NEON)
  static constexpr std::uint8_t kSlotBits[16] = {
      1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x16_t control = vld1q_u8(group);
  const uint8x16_t equal = vceqq_u8(control, vdupq_n_u8(needle));
  const uint8x16_t masked = vandq_u8(equal, vld1q_u8(kSlotBits));
  const std::uint32_t low = vaddv_u8(vget_low_u8(masked));
  const std::uint32_t high = vaddv_u8(vget_high_u8(masked));
  return static_cast<std::uint64_t>(low | (high << 8));
#elif defined(GOBLIN_SIMD_LSX)
  const __m128i control =
      __lsx_vld(const_cast<std::uint8_t*>(group), 0);
  const __m128i equal =
      __lsx_vseq_b(control, __lsx_vreplgr2vr_b(needle));
  const __m128i packed = __lsx_vmskltz_b(equal);
  return static_cast<std::uint64_t>(
      __lsx_vpickve2gr_hu(packed, 0) & 0xFFFFU);
#else
  std::uint64_t mask = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    mask |= static_cast<std::uint64_t>(group[i] == needle) << i;
  }
  return mask;
#endif
}

// RT linear-hash bucket probe: bit i is set when hashes[i] == needle. The
// Arena hash planes are 64-byte aligned; the one per-index inline group is not.
// AVX-512 reads either form once and AVX2 twice.
[[nodiscard]] inline std::uint64_t match_hash_group_16(
    const std::uint32_t* hashes, std::uint32_t needle) noexcept {
#if defined(GOBLIN_SIMD_AVX512)
  const __m512i values =
      _mm512_loadu_si512(reinterpret_cast<const void*>(hashes));
  return static_cast<std::uint64_t>(
      _mm512_cmpeq_epi32_mask(values, _mm512_set1_epi32(needle)));
#elif defined(GOBLIN_SIMD_AVX2)
  const __m256i expected = _mm256_set1_epi32(static_cast<int>(needle));
  const __m256i low =
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hashes));
  const __m256i high =
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hashes + 8));
  const auto low_mask = static_cast<unsigned>(_mm256_movemask_ps(
      _mm256_castsi256_ps(_mm256_cmpeq_epi32(low, expected))));
  const auto high_mask = static_cast<unsigned>(_mm256_movemask_ps(
      _mm256_castsi256_ps(_mm256_cmpeq_epi32(high, expected))));
  return static_cast<std::uint64_t>(low_mask | (high_mask << 8));
#elif defined(GOBLIN_SIMD_SSE2)
  const __m128i expected = _mm_set1_epi32(static_cast<int>(needle));
  std::uint64_t mask = 0;
  for (std::size_t group = 0; group < 4; ++group) {
    const __m128i values = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(hashes + group * 4));
    const auto group_mask = static_cast<unsigned>(
        _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(values, expected))));
    mask |= static_cast<std::uint64_t>(group_mask) << (group * 4);
  }
  return mask;
#elif defined(GOBLIN_SIMD_LASX)
  const __m256i expected = __lasx_xvreplgr2vr_w(needle);
  std::uint64_t mask = 0;
  for (std::size_t group = 0; group < 2; ++group) {
    const __m256i values = __lasx_xvld(
        const_cast<std::uint32_t*>(hashes + group * 8), 0);
    const __m256i packed =
        __lasx_xvmskltz_w(__lasx_xvseq_w(values, expected));
    const auto group_mask =
        (static_cast<unsigned>(__lasx_xvpickve2gr_wu(packed, 0)) & 0x0fU) |
        ((static_cast<unsigned>(__lasx_xvpickve2gr_wu(packed, 4)) & 0x0fU)
         << 4);
    mask |= static_cast<std::uint64_t>(group_mask) << (group * 8);
  }
  return mask;
#elif defined(GOBLIN_SIMD_LSX)
  const __m128i expected = __lsx_vreplgr2vr_w(needle);
  std::uint64_t mask = 0;
  for (std::size_t group = 0; group < 4; ++group) {
    const __m128i values =
        __lsx_vld(const_cast<std::uint32_t*>(hashes + group * 4), 0);
    const __m128i packed = __lsx_vmskltz_w(__lsx_vseq_w(values, expected));
    const auto group_mask =
        static_cast<unsigned>(__lsx_vpickve2gr_wu(packed, 0)) & 0x0fU;
    mask |= static_cast<std::uint64_t>(group_mask) << (group * 4);
  }
  return mask;
#elif defined(GOBLIN_SIMD_NEON)
  static constexpr std::uint32_t kLaneBits[4] = {1, 2, 4, 8};
  const uint32x4_t expected = vdupq_n_u32(needle);
  const uint32x4_t lane_bits = vld1q_u32(kLaneBits);
  std::uint64_t mask = 0;
  for (std::size_t group = 0; group < 4; ++group) {
    const uint32x4_t values = vld1q_u32(hashes + group * 4);
    const uint32x4_t equal = vshrq_n_u32(vceqq_u32(values, expected), 31);
    const auto group_mask = vaddvq_u32(vmulq_u32(equal, lane_bits));
    mask |= static_cast<std::uint64_t>(group_mask) << (group * 4);
  }
  return mask;
#else
  std::uint64_t mask = 0;
  for (std::size_t i = 0; i < 16; ++i) {
    mask |= static_cast<std::uint64_t>(hashes[i] == needle) << i;
  }
  return mask;
#endif
}

// Compact-hash fingerprint scan. Unlike the Swiss-table probe above, this is a
// dense sequential directory, so use the widest byte vector provided by the
// selected build ISA. The return mask always maps bit i to group[i].
#if defined(GOBLIN_SIMD_AVX512)
inline constexpr std::size_t kFingerprintGroupWidth = 64;
#elif defined(GOBLIN_SIMD_AVX2) || defined(GOBLIN_SIMD_LASX)
inline constexpr std::size_t kFingerprintGroupWidth = 32;
#else
inline constexpr std::size_t kFingerprintGroupWidth = 16;
#endif

[[nodiscard]] inline std::uint64_t match_fingerprint_group(
    const std::uint8_t* group, std::uint8_t needle) noexcept {
#if defined(GOBLIN_SIMD_AVX512)
  const __m512i fingerprints =
      _mm512_loadu_si512(reinterpret_cast<const void*>(group));
  return static_cast<std::uint64_t>(_mm512_cmpeq_epi8_mask(
      fingerprints, _mm512_set1_epi8(static_cast<char>(needle))));
#elif defined(GOBLIN_SIMD_AVX2)
  const __m256i fingerprints =
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(group));
  const __m256i equal = _mm256_cmpeq_epi8(
      fingerprints, _mm256_set1_epi8(static_cast<char>(needle)));
  return static_cast<std::uint64_t>(
      static_cast<std::uint32_t>(_mm256_movemask_epi8(equal)));
#elif defined(GOBLIN_SIMD_LASX)
  const __m256i fingerprints =
      __lasx_xvld(const_cast<std::uint8_t*>(group), 0);
  const __m256i equal = __lasx_xvseq_b(
      fingerprints, __lasx_xvreplgr2vr_b(needle));
  const __m256i packed = __lasx_xvmskltz_b(equal);
  const auto low =
      static_cast<std::uint32_t>(__lasx_xvpickve2gr_wu(packed, 0)) & 0xFFFFU;
  const auto high =
      static_cast<std::uint32_t>(__lasx_xvpickve2gr_wu(packed, 4)) & 0xFFFFU;
  return static_cast<std::uint64_t>(low | (high << 16));
#elif defined(GOBLIN_SIMD_LSX)
  const __m128i fingerprints =
      __lsx_vld(const_cast<std::uint8_t*>(group), 0);
  const __m128i equal =
      __lsx_vseq_b(fingerprints, __lsx_vreplgr2vr_b(needle));
  const __m128i packed = __lsx_vmskltz_b(equal);
  return static_cast<std::uint64_t>(
      __lsx_vpickve2gr_hu(packed, 0) & 0xFFFFU);
#else
  return match_control_group_16(group, needle);
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
#if defined(GOBLIN_SIMD_AVX2)
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
#if defined(GOBLIN_SIMD_SSE2)
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
#elif defined(GOBLIN_SIMD_NEON)
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

#if defined(GOBLIN_SIMD_AVX2)
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
#if defined(GOBLIN_SIMD_SSE2)
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
#elif defined(GOBLIN_SIMD_NEON)
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

#if defined(GOBLIN_SIMD_AVX2)
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
#if defined(GOBLIN_SIMD_SSE2)
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
#elif defined(GOBLIN_SIMD_NEON)
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

// ---- Bitset helpers for 0/1 knapsack / subset-sum (tail-donor packing) ----
// Reachable sums live in a little-endian bitset of uint64_t words. The hot step
// is `bits |= bits << weight`; the OR of large word arrays is SIMD-friendly.

// dst[i] |= src[i] for `n` uint64 words. Widest OR available at compile time
// (AVX-512/AVX2/SSE2 on x86, NEON on AArch64, LASX/LSX on LoongArch).
inline void or_u64_words(std::uint64_t* dst, const std::uint64_t* src,
                         std::size_t n) noexcept {
  std::size_t i = 0;
#if defined(GOBLIN_SIMD_AVX512)
  while (i + 8 <= n) {
    const __m512i a =
        _mm512_loadu_si512(reinterpret_cast<const void*>(dst + i));
    const __m512i b =
        _mm512_loadu_si512(reinterpret_cast<const void*>(src + i));
    _mm512_storeu_si512(reinterpret_cast<void*>(dst + i), _mm512_or_si512(a, b));
    i += 8;
  }
#endif
#if defined(GOBLIN_SIMD_AVX2)
  while (i + 4 <= n) {
    const __m256i a =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
    const __m256i b =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),
                        _mm256_or_si256(a, b));
    i += 4;
  }
#endif
#if defined(GOBLIN_SIMD_SSE2)
  while (i + 2 <= n) {
    const __m128i a =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
    const __m128i b =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), _mm_or_si128(a, b));
    i += 2;
  }
#elif defined(GOBLIN_SIMD_NEON)
  while (i + 2 <= n) {
    const uint64x2_t a = vld1q_u64(dst + i);
    const uint64x2_t b = vld1q_u64(src + i);
    vst1q_u64(dst + i, vorrq_u64(a, b));
    i += 2;
  }
#elif defined(GOBLIN_SIMD_LASX)
  // LASX: 256-bit = 4×u64 (same width as AVX2).
  while (i + 4 <= n) {
    const __m256i a =
        __lasx_xvld(const_cast<std::uint64_t*>(dst + i), 0);
    const __m256i b =
        __lasx_xvld(const_cast<std::uint64_t*>(src + i), 0);
    __lasx_xvst(__lasx_xvor_v(a, b), dst + i, 0);
    i += 4;
  }
  // Remainder: LSX 128-bit = 2×u64 (LASX builds always have LSX).
  while (i + 2 <= n) {
    const __m128i a =
        __lsx_vld(const_cast<std::uint64_t*>(dst + i), 0);
    const __m128i b =
        __lsx_vld(const_cast<std::uint64_t*>(src + i), 0);
    __lsx_vst(__lsx_vor_v(a, b), dst + i, 0);
    i += 2;
  }
#elif defined(GOBLIN_SIMD_LSX)
  // LSX only (no LASX): 128-bit = 2×u64.
  while (i + 2 <= n) {
    const __m128i a =
        __lsx_vld(const_cast<std::uint64_t*>(dst + i), 0);
    const __m128i b =
        __lsx_vld(const_cast<std::uint64_t*>(src + i), 0);
    __lsx_vst(__lsx_vor_v(a, b), dst + i, 0);
    i += 2;
  }
#endif
  for (; i < n; ++i) {
    dst[i] |= src[i];
  }
}

}  // namespace goblin::core::simd::GOBLIN_SIMD_BACKEND
