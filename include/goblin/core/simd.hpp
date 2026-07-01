#pragma once

namespace goblin::core::simd {

struct Capabilities {
  bool neon{false};
  bool avx2{false};
  bool avx512bw{false};
  bool avx512vl{false};
};

[[nodiscard]] Capabilities detect_capabilities() noexcept;

}  // namespace goblin::core::simd
