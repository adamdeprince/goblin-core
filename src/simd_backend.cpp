#include "goblin/core/simd.hpp"
#include "simd_backend_ops.hpp"

// Keep XXH3's vector selection in the same backend as the table probes.
#define XXH_INLINE_ALL
#if defined(GOBLIN_SIMD_AVX512)
#define XXH_VECTOR XXH_AVX512
#elif defined(GOBLIN_SIMD_AVX2)
#define XXH_VECTOR XXH_AVX2
#elif defined(GOBLIN_SIMD_SSE2)
#define XXH_VECTOR XXH_SSE2
#elif defined(GOBLIN_SIMD_NEON)
#define XXH_VECTOR XXH_NEON
#elif defined(GOBLIN_SIMD_LSX)
#define XXH_VECTOR XXH_LSX
#else
#define XXH_VECTOR XXH_SCALAR
#endif
#include "xxhash.h"

namespace goblin::core::simd::GOBLIN_SIMD_BACKEND {
std::uint64_t hash_bytes(const void* data, std::size_t size) noexcept {
  return XXH3_64bits(data, size);
}
// Constant initialization keeps optional ISA instructions out of startup.
const detail::Dispatch& backend_dispatch() noexcept {
  static constexpr detail::Dispatch table{
      Model::GOBLIN_SIMD_BACKEND, kFingerprintGroupWidth,
      match_control_group_16, match_hash_group_16, match_fingerprint_group,
      bytes_equal_n, find_byte, find_crlf, or_u64_words, hash_bytes};
  return table;
}
}  // namespace goblin::core::simd::GOBLIN_SIMD_BACKEND
