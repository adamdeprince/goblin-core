#pragma once

// Compile-time SIMD flags for startup logging. Goblin is built for the target
// hardware; hot paths use #ifdef in simd_ops.hpp, not runtime CPU dispatch.

namespace goblin::core::simd {

struct Capabilities {
  bool neon{false};
  bool avx2{false};
  bool avx512bw{false};
  bool avx512vl{false};
  bool lsx{false};
  bool lasx{false};
};

[[nodiscard]] Capabilities detect_capabilities() noexcept;

}  // namespace goblin::core::simd
