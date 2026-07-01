#include "goblin/core/simd.hpp"

namespace goblin::core::simd {

Capabilities detect_capabilities() noexcept {
  Capabilities caps;

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  caps.neon = true;
#endif

#if defined(__x86_64__) || defined(__i386__)
#if defined(__GNUC__) || defined(__clang__)
  caps.avx2 = __builtin_cpu_supports("avx2");
  caps.avx512bw = __builtin_cpu_supports("avx512bw");
  caps.avx512vl = __builtin_cpu_supports("avx512vl");
#endif
#endif

  return caps;
}

}  // namespace goblin::core::simd
