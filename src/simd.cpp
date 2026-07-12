#include "goblin/core/simd.hpp"

namespace goblin::core::simd {

Capabilities detect_capabilities() noexcept {
  Capabilities caps;

#if defined(__aarch64__) || defined(__ARM_NEON)
  caps.neon = true;
#endif

#if defined(__AVX512VL__)
  caps.avx512vl = true;
#endif
#if defined(__AVX512BW__)
  caps.avx512bw = true;
#endif
#if defined(__AVX2__)
  caps.avx2 = true;
#endif
#if defined(__loongarch_sx)
  caps.lsx = true;
#endif
#if defined(__loongarch_asx)
  caps.lasx = true;
#endif

  return caps;
}

}  // namespace goblin::core::simd
