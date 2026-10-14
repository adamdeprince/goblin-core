#include "goblin/core/simd.hpp"

#include <array>
#include <atomic>
#include <stdexcept>
#include <string>

#if defined(__linux__) && (defined(__aarch64__) || defined(__loongarch__))
#include <sys/auxv.h>
#include <asm/hwcap.h>
#elif defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#endif

namespace goblin::core::simd {
namespace scalar { const detail::Dispatch& backend_dispatch() noexcept; }
#define DECLARE_BACKEND(name) \
  namespace name { const detail::Dispatch& backend_dispatch() noexcept; }
#ifdef GOBLIN_BUILD_SSE2
DECLARE_BACKEND(sse2)
#endif
#ifdef GOBLIN_BUILD_AVX2
DECLARE_BACKEND(avx2)
#endif
#ifdef GOBLIN_BUILD_AVX512
DECLARE_BACKEND(avx512)
#endif
#ifdef GOBLIN_BUILD_NEON
DECLARE_BACKEND(neon)
#endif
#ifdef GOBLIN_BUILD_LSX
DECLARE_BACKEND(lsx)
#endif
#ifdef GOBLIN_BUILD_LASX
DECLARE_BACKEND(lasx)
#endif
#undef DECLARE_BACKEND

Capabilities detect_capabilities() noexcept {
  Capabilities caps;
#if (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
  // These builtins include OSXSAVE/XGETBV checks for AVX and AVX-512.
  __builtin_cpu_init();
  caps.sse2 = __builtin_cpu_supports("sse2");
  caps.avx2 = __builtin_cpu_supports("avx2");
  caps.avx512f = __builtin_cpu_supports("avx512f");
  caps.avx512bw = __builtin_cpu_supports("avx512bw");
  caps.avx512vl = __builtin_cpu_supports("avx512vl");
  caps.crc32 = __builtin_cpu_supports("sse4.2");
#elif defined(__linux__) && defined(__aarch64__)
  const auto hwcap = getauxval(AT_HWCAP);
  caps.neon = (hwcap & HWCAP_ASIMD) != 0;
  caps.crc32 = (hwcap & HWCAP_CRC32) != 0;
#elif defined(__APPLE__) && defined(__aarch64__)
  caps.neon = true;  // Required by the Apple AArch64 ABI.
  int crc = 0;
  std::size_t size = sizeof(crc);
  caps.crc32 = sysctlbyname("hw.optional.armv8_crc32", &crc, &size,
                           nullptr, 0) == 0 && crc != 0;
#elif defined(__linux__) && defined(__loongarch__)
  const auto hwcap = getauxval(AT_HWCAP);
  caps.lsx = (hwcap & HWCAP_LOONGARCH_LSX) != 0;
  caps.lasx = (hwcap & HWCAP_LOONGARCH_LASX) != 0;
  caps.crc32 = true;  // CRC32C is in the base LoongArch ISA.
#elif defined(__aarch64__)
  caps.neon = true;
#endif
  return caps;
}

namespace {
const detail::Dispatch* lookup(Model model, const Capabilities& caps) noexcept {
  switch (model) {
    case Model::scalar: return &scalar::backend_dispatch();
#ifdef GOBLIN_BUILD_SSE2
    case Model::sse2:
      if (caps.sse2) return &sse2::backend_dispatch();
      break;
#endif
#ifdef GOBLIN_BUILD_AVX2
    case Model::avx2:
      if (caps.avx2 && caps.sse2) return &avx2::backend_dispatch();
      break;
#endif
#ifdef GOBLIN_BUILD_AVX512
    case Model::avx512:
      if (caps.avx2 && caps.avx512f && caps.avx512bw && caps.avx512vl)
        return &avx512::backend_dispatch();
      break;
#endif
#ifdef GOBLIN_BUILD_NEON
    case Model::neon:
      if (caps.neon) return &neon::backend_dispatch();
      break;
#endif
#ifdef GOBLIN_BUILD_LSX
    case Model::lsx:
      if (caps.lsx) return &lsx::backend_dispatch();
      break;
#endif
#ifdef GOBLIN_BUILD_LASX
    case Model::lasx:
      if (caps.lasx && caps.lsx) return &lasx::backend_dispatch();
      break;
#endif
    default: break;
  }
  return nullptr;
}

const detail::Dispatch* best_backend() noexcept {
  const auto caps = detect_capabilities();
  for (const auto model : {Model::avx512, Model::avx2, Model::sse2,
                           Model::lasx, Model::lsx, Model::neon}) {
    if (const auto* table = lookup(model, caps)) return table;
  }
  return &scalar::backend_dispatch();
}

std::atomic<const detail::Dispatch*>& selection() noexcept {
  static std::atomic<const detail::Dispatch*> selected{best_backend()};
  return selected;
}

std::uint32_t software_checksum(std::string_view data) noexcept {
  static constexpr auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char byte : data)
    crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}
}  // namespace

const detail::Dispatch& detail::dispatch() noexcept {
  return *selection().load(std::memory_order_relaxed);
}

bool is_supported(Model model) noexcept {
  return lookup(model, detect_capabilities()) != nullptr;
}

Model selected_model() noexcept { return detail::dispatch().model; }

std::string_view model_name(Model model) noexcept {
  switch (model) {
    case Model::scalar: return "scalar";
    case Model::sse2: return "sse2";
    case Model::avx2: return "avx2";
    case Model::avx512: return "avx512";
    case Model::neon: return "neon";
    case Model::lsx: return "lsx";
    case Model::lasx: return "lasx";
  }
  return "unknown";
}

void configure(std::string_view name) {
  if (name == "auto") {
    selection().store(best_backend(), std::memory_order_relaxed);
    return;
  }
  for (const auto model : {Model::scalar, Model::sse2, Model::avx2,
                           Model::avx512, Model::neon, Model::lsx, Model::lasx}) {
    if (name != model_name(model)) continue;
    const auto* table = lookup(model, detect_capabilities());
    if (!table)
      throw std::invalid_argument("SIMD model '" + std::string(name) +
          "' is not compiled or is incompatible with this CPU/OS");
    selection().store(table, std::memory_order_relaxed);
    return;
  }
  throw std::invalid_argument("unknown SIMD model '" + std::string(name) +
      "'; choose auto, scalar, sse2, avx2, avx512, neon, lsx, or lasx");
}

#ifdef GOBLIN_BUILD_CRC
namespace detail {
std::uint32_t hardware_checksum(std::string_view data) noexcept;
}
#endif
std::uint32_t checksum(std::string_view data) noexcept {
#ifdef GOBLIN_BUILD_CRC
  static const bool hardware = detect_capabilities().crc32;
  if (hardware && selected_model() != Model::scalar)
    return detail::hardware_checksum(data);
#endif
  return software_checksum(data);
}
}  // namespace goblin::core::simd
