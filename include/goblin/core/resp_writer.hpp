#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/score_format.hpp"

namespace goblin::core::resp {

namespace detail {

struct SmallHeader {
  std::array<char, 6> bytes{};
  std::uint8_t size{0};
};

[[nodiscard]] constexpr SmallHeader make_header(char prefix, std::size_t value) {
  SmallHeader header;
  auto out = header.bytes.begin();
  *out++ = prefix;

  std::array<char, 3> digits{};
  std::size_t digit_count = 0;
  do {
    digits[digit_count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);

  while (digit_count > 0) {
    *out++ = digits[--digit_count];
  }
  *out++ = '\r';
  *out++ = '\n';
  header.size = static_cast<std::uint8_t>(out - header.bytes.begin());
  return header;
}

template <std::size_t... Values>
[[nodiscard]] constexpr auto make_small_headers(
    char prefix,
    std::index_sequence<Values...>) {
  return std::array<SmallHeader, sizeof...(Values)>{
      make_header(prefix, Values)...};
}

inline constexpr auto kSmallBulkHeaders =
    make_small_headers('$', std::make_index_sequence<65>{});
inline constexpr auto kSmallArrayHeaders =
    make_small_headers('*', std::make_index_sequence<257>{});

template <class Int>
inline void append_decimal(std::string& out, Int value) {
  std::array<char, 32> buffer{};
  const auto [ptr, ec] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (ec == std::errc{}) {
    out.append(buffer.data(), ptr);
  }
}

[[nodiscard]] inline std::size_t decimal_digits(std::size_t value) noexcept {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

[[nodiscard]] inline std::size_t bulk_string_wire_size(std::size_t length) noexcept {
  if (length < kSmallBulkHeaders.size()) {
    return kSmallBulkHeaders[length].size + length + 2;
  }
  return 1 + decimal_digits(length) + 2 + length + 2;
}

[[nodiscard]] inline std::size_t array_header_wire_size(std::size_t count) noexcept {
  if (count < kSmallArrayHeaders.size()) {
    return kSmallArrayHeaders[count].size;
  }
  return 1 + decimal_digits(count) + 2;
}

inline void append_bulk_framing(std::string& out, std::size_t length) {
  if (length < kSmallBulkHeaders.size()) {
    const auto& header = kSmallBulkHeaders[length];
    out.append(header.bytes.data(), header.size);
  } else {
    out.push_back('$');
    append_decimal(out, length);
    out.append("\r\n", 2);
  }
}

inline void append_bulk_payload(std::string& out, std::string_view value) {
  append_bulk_framing(out, value.size());
  out.append(value);
  out.append("\r\n", 2);
}

}  // namespace detail

inline void reserve_append_capacity(std::string& out,
                                    std::size_t append_bytes,
                                    std::size_t reserve_limit = 0) {
  if (append_bytes == 0) {
    return;
  }

  const auto size = out.size();
  const auto capacity = out.capacity();
  if (append_bytes <= capacity - size) {
    return;
  }

  const auto max_size = out.max_size();
  if (append_bytes > max_size - size) {
    throw std::length_error("RESP output too large");
  }

  const auto required = size + append_bytes;
  const auto reserve_cap =
      reserve_limit == 0 ? max_size : std::min(reserve_limit, max_size);
  if (required > reserve_cap) {
    if (capacity < reserve_cap) {
      out.reserve(reserve_cap);
    }
    return;
  }

  std::size_t target = capacity == 0 ? append_bytes : capacity;
  while (target < required) {
    if (target > max_size / 2) {
      target = max_size;
      break;
    }
    target *= 2;
  }
  target = std::min(target, reserve_cap);
  out.reserve(target);
}

inline void append_simple_string(std::string& out, std::string_view value) {
  out.push_back('+');
  out.append(value);
  out.append("\r\n");
}

inline void append_error(std::string& out, std::string_view message) {
  out.push_back('-');
  out.append(message);
  out.append("\r\n");
}

inline void append_integer(std::string& out, long long value) {
  out.push_back(':');
  detail::append_decimal(out, value);
  out.append("\r\n");
}

inline void append_bulk_string(std::string& out, std::string_view value) {
  if (value.size() < detail::kSmallBulkHeaders.size()) {
    const auto& header = detail::kSmallBulkHeaders[value.size()];
    out.append(header.bytes.data(), header.size);
  } else {
    out.push_back('$');
    detail::append_decimal(out, value.size());
    out.append("\r\n", 2);
  }
  out.append(value);
  out.append("\r\n", 2);
}

inline void reserve_zrange_array(std::string& out,
                                 std::size_t bulk_count,
                                 std::size_t payload_wire_bytes,
                                 std::size_t reserve_limit = 0) {
  reserve_append_capacity(
      out,
      detail::array_header_wire_size(bulk_count) + payload_wire_bytes,
      reserve_limit);
}

[[nodiscard]] inline std::size_t bulk_finite_double_wire_size(double value) noexcept {
  std::array<char, 32> buffer;
  const auto text = score_format::try_format_finite_to_buffer(value, buffer);
  if (!text.empty()) {
    return detail::bulk_string_wire_size(text.size());
  }
  return detail::bulk_string_wire_size(32);
}

inline void append_bulk_finite_double(std::string& out, double value) {
  std::array<char, 32> buffer;
  const auto text = score_format::try_format_finite_to_buffer(value, buffer);
  if (!text.empty()) {
    append_bulk_string(out, text);
    return;
  }

  append_bulk_string(out, score_format::fallback(value));
}

inline void append_bulk_member_and_text(std::string& out,
                                        std::string_view member,
                                        std::string_view text) {
  append_bulk_string(out, member);
  append_bulk_string(out, text);
}

inline void append_bulk_member_and_finite_double(std::string& out,
                                                 std::string_view member,
                                                 double score) {
  append_bulk_string(out, member);
  append_bulk_finite_double(out, score);
}

inline void append_bulk_double(std::string& out, double value) {
  std::string fallback;
  std::string_view view;

  if (std::isnan(value)) {
    view = "nan";
  } else if (std::isinf(value)) {
    view = value < 0 ? "-inf" : "inf";
  } else {
    append_bulk_finite_double(out, value);
    return;
  }

  append_bulk_string(out, view);
}

inline void append_null_bulk_string(std::string& out) {
  out.append("$-1\r\n");
}

inline void append_array_header(std::string& out, std::size_t count) {
  if (count < detail::kSmallArrayHeaders.size()) {
    const auto& header = detail::kSmallArrayHeaders[count];
    out.append(header.bytes.data(), header.size);
  } else {
    out.push_back('*');
    detail::append_decimal(out, count);
    out.append("\r\n");
  }
}

[[nodiscard]] std::string simple_string(std::string_view value);
[[nodiscard]] std::string error(std::string_view message);
[[nodiscard]] std::string integer(long long value);
[[nodiscard]] std::string bulk_string(std::string_view value);
[[nodiscard]] std::string null_bulk_string();
[[nodiscard]] std::string array(std::span<const std::string_view> values);

}  // namespace goblin::core::resp
