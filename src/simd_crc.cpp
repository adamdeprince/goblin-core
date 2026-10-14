#include <cstdint>
#include <cstring>
#include <string_view>
#if defined(GOBLIN_CRC_X86)
#include <nmmintrin.h>
#elif defined(GOBLIN_CRC_ARM)
#include <arm_acle.h>
#endif

namespace goblin::core::simd::detail {
std::uint32_t hardware_checksum(std::string_view data) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t n = data.size();
  std::uint32_t crc = 0xFFFFFFFFu;

#if defined(GOBLIN_CRC_X86)
  std::uint64_t acc = crc;
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t chunk;
    std::memcpy(&chunk, p, 8);
    acc = _mm_crc32_u64(acc, chunk);
  }
  crc = static_cast<std::uint32_t>(acc);
  for (; n > 0; --n) crc = _mm_crc32_u8(crc, *p++);
#elif defined(GOBLIN_CRC_ARM)
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t chunk;
    std::memcpy(&chunk, p, 8);
    crc = __crc32cd(crc, chunk);
  }
  for (; n > 0; --n) crc = __crc32cb(crc, *p++);
#elif defined(GOBLIN_CRC_LOONGARCH)
  // crcc.w.* computes CRC32C directly. Validated on a LoongArch 3A6000 against the
  // published check value (checksum("123456789") == 0xe3069283) and the x86/ARM paths.
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t chunk;
    std::memcpy(&chunk, p, 8);
    crc = static_cast<std::uint32_t>(__builtin_loongarch_crcc_w_d_w(
        static_cast<long long>(chunk), static_cast<int>(crc)));
  }
  for (; n > 0; --n)
    crc = static_cast<std::uint32_t>(
        __builtin_loongarch_crcc_w_b_w(static_cast<char>(*p++), static_cast<int>(crc)));
#endif
  return crc ^ 0xFFFFFFFFu;
}
}  // namespace goblin::core::simd::detail
