#pragma once

// Base-10 integer parsing via vendored fast_float (MIT OR Apache-2.0).
// Full-string consume: trailing junk → nullopt / false, matching prior
// std::from_chars callers used on the RESP and encoding hot paths.

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include "fast_float/fast_float.h"

namespace goblin::core {

// Prefer long long to match RESP/command call sites (same width as int64_t).
[[nodiscard]] inline std::optional<long long> parse_i64(
    std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  long long value = 0;
  const auto answer =
      fast_float::from_chars(text.data(), text.data() + text.size(), value);
  if (answer.ec != std::errc{} || answer.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] inline std::optional<std::uint64_t> parse_u64(
    std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto answer =
      fast_float::from_chars(text.data(), text.data() + text.size(), value);
  if (answer.ec != std::errc{} || answer.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

// Canonical compact-int digit string: no leading zeros except a single "0".
[[nodiscard]] inline bool parse_canonical_u64(std::string_view digits,
                                              std::uint64_t& value) noexcept {
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) {
    return false;
  }
  const auto parsed = parse_u64(digits);
  if (!parsed) {
    return false;
  }
  value = *parsed;
  return true;
}

}  // namespace goblin::core
