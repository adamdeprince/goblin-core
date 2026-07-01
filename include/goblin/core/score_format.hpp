#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace goblin::core::score_format {

namespace detail {

[[nodiscard]] inline bool exact_integer_score(double value,
                                              long long& out) noexcept {
  constexpr double kMaxExactInteger = 9'007'199'254'740'992.0;
  if (value < -kMaxExactInteger || value > kMaxExactInteger) {
    return false;
  }
  if (value == 0.0 && std::signbit(value)) {
    return false;
  }

  const auto integer_value = static_cast<long long>(value);
  if (static_cast<double>(integer_value) != value) {
    return false;
  }

  out = integer_value;
  return true;
}

}  // namespace detail

[[nodiscard]] inline std::string fallback(double value) {
  std::ostringstream stream;
  stream << std::setprecision(17) << value;
  return stream.str();
}

[[nodiscard]] inline std::string_view try_format_finite_to_buffer(
    double value,
    std::array<char, 32>& buffer) noexcept {
  long long integer_value = 0;
  if (detail::exact_integer_score(value, integer_value)) {
    const auto [ptr, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), integer_value);
    if (ec == std::errc{}) {
      return std::string_view(buffer.data(),
                              static_cast<std::size_t>(ptr - buffer.data()));
    }
  }

  const auto [ptr, ec] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (ec == std::errc{}) {
    return std::string_view(buffer.data(),
                            static_cast<std::size_t>(ptr - buffer.data()));
  }

  return {};
}

[[nodiscard]] inline std::string_view format_finite_to_buffer(
    double value,
    std::array<char, 32>& buffer,
    std::string& fallback_text) {
  const auto text = try_format_finite_to_buffer(value, buffer);
  if (!text.empty()) {
    return text;
  }

  fallback_text = fallback(value);
  return fallback_text;
}

[[nodiscard]] inline std::string format(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-inf" : "inf";
  }

  std::array<char, 32> buffer;
  const auto text = try_format_finite_to_buffer(value, buffer);
  if (!text.empty()) {
    return std::string(text);
  }

  return fallback(value);
}

}  // namespace goblin::core::score_format
