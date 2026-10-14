#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace goblin::core::simd {

enum class Model { scalar, sse2, avx2, avx512, neon, lsx, lasx };

struct Capabilities {
  bool sse2{false};
  bool neon{false};
  bool avx2{false};
  bool avx512f{false};
  bool avx512bw{false};
  bool avx512vl{false};
  bool lsx{false};
  bool lasx{false};
  bool crc32{false};
};

// Reports usable CPU/OS features, independently of the compiled backends.
[[nodiscard]] Capabilities detect_capabilities() noexcept;
[[nodiscard]] bool is_supported(Model model) noexcept;
[[nodiscard]] Model selected_model() noexcept;
[[nodiscard]] std::string_view model_name(Model model) noexcept;

// Call before starting workers. "auto" selects the best compiled, usable ISA.
// Explicit choices must be compiled and CPU/OS compatible; throws
// std::invalid_argument otherwise, preserving the previous selection.
void configure(std::string_view model = "auto");
[[nodiscard]] std::uint32_t checksum(std::string_view data) noexcept;

namespace detail {
struct Dispatch {
  Model model;
  std::size_t fingerprint_width;
  std::uint64_t (*match_control)(const std::uint8_t*, std::uint8_t) noexcept;
  std::uint64_t (*match_hash)(const std::uint32_t*, std::uint32_t) noexcept;
  std::uint64_t (*match_fingerprint)(const std::uint8_t*, std::uint8_t) noexcept;
  bool (*bytes_equal)(const void*, const void*, std::size_t) noexcept;
  std::optional<std::size_t> (*find_byte)(std::string_view, char,
                                        std::size_t) noexcept;
  std::optional<std::size_t> (*find_crlf)(std::string_view, std::size_t) noexcept;
  void (*or_words)(std::uint64_t*, const std::uint64_t*, std::size_t) noexcept;
  std::uint64_t (*hash_bytes)(const void*, std::size_t) noexcept;
};
[[nodiscard]] const Dispatch& dispatch() noexcept;
}  // namespace detail
}  // namespace goblin::core::simd
