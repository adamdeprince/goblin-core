#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/score_format.hpp"
#include "goblin/core/resp_version.hpp"
#include "goblin/core/string_encoding.hpp"

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
inline constexpr auto kSmallPushHeaders =
    make_small_headers('>', std::make_index_sequence<17>{});

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

inline char* write_small_bulk_string(char* dst, std::string_view value) noexcept {
  const auto& header = kSmallBulkHeaders[value.size()];
  std::memcpy(dst, header.bytes.data(), header.size);
  dst += header.size;
  if (!value.empty()) {
    std::memcpy(dst, value.data(), value.size());
    dst += value.size();
  }
  dst[0] = '\r';
  dst[1] = '\n';
  return dst + 2;
}

[[nodiscard]] inline bool can_write_small_bulk_string(
    std::size_t length) noexcept {
  return length < kSmallBulkHeaders.size();
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
  if (detail::can_write_small_bulk_string(value.size())) {
    const auto offset = out.size();
    out.resize(offset + detail::bulk_string_wire_size(value.size()));
    (void)detail::write_small_bulk_string(out.data() + offset, value);
    return;
  }

  out.push_back('$');
  detail::append_decimal(out, value.size());
  out.append("\r\n", 2);
  out.append(value);
  out.append("\r\n", 2);
}

inline void append_bulk_string(std::string& out,
                               const EncodedStringView& value) {
  // Raw / verbatim: logical bytes are a tag-stripped (or untagged) copy of the
  // stored layout. Skip valid()+decode and use the same single-shot small-bulk
  // path as plain string_view when the payload is contiguous.
  if (value.is_raw()) {
    if (!value.encoding_enabled()) {
      if (value.encoded_tail().empty()) {
        append_bulk_string(out, value.encoded_head());
        return;
      }
      detail::append_bulk_framing(out, value.encoded_size());
      out.append(value.encoded_head());
      out.append(value.encoded_tail());
      out.append("\r\n", 2);
      return;
    }
    // Encoded raw: leading 0xff tag, then logical payload across head/tail.
    const auto encoded = value.encoded_size();
    if (encoded == 0) {
      append_bulk_string(out, std::string_view{});
      return;
    }
    const auto logical = encoded - 1;
    const auto& head = value.encoded_head();
    const auto& tail = value.encoded_tail();
    if (tail.empty() && head.size() >= 1) {
      append_bulk_string(out, std::string_view(head.data() + 1, head.size() - 1));
      return;
    }
    // Split payload: head after the tag, then full tail.
    if (detail::can_write_small_bulk_string(logical)) {
      const auto offset = out.size();
      out.resize(offset + detail::bulk_string_wire_size(logical));
      char* dst = out.data() + offset;
      const auto& header = detail::kSmallBulkHeaders[logical];
      std::memcpy(dst, header.bytes.data(), header.size);
      dst += header.size;
      if (head.size() > 1) {
        std::memcpy(dst, head.data() + 1, head.size() - 1);
        dst += head.size() - 1;
      }
      if (!tail.empty()) {
        std::memcpy(dst, tail.data(), tail.size());
        dst += tail.size();
      }
      dst[0] = '\r';
      dst[1] = '\n';
      return;
    }
    detail::append_bulk_framing(out, logical);
    if (head.size() > 1) {
      out.append(head.data() + 1, head.size() - 1);
    }
    out.append(tail);
    out.append("\r\n", 2);
    return;
  }

  // Compact int / UUID / LZ4: one size()+decode pass.
  detail::append_bulk_framing(out, value.size());
  value.append_to(out);
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
  std::string_view text = score_format::try_format_finite_to_buffer(value, buffer);
  std::string fallback;
  if (text.empty()) {
    fallback = score_format::fallback(value);
    text = fallback;
  }

  if (detail::can_write_small_bulk_string(text.size())) {
    const auto offset = out.size();
    out.resize(offset + detail::bulk_string_wire_size(text.size()));
    (void)detail::write_small_bulk_string(out.data() + offset, text);
    return;
  }

  append_bulk_string(out, text);
}

inline void append_bulk_member_and_text(std::string& out,
                                        std::string_view member,
                                        std::string_view text) {
  if (!detail::can_write_small_bulk_string(member.size()) ||
      !detail::can_write_small_bulk_string(text.size())) {
    append_bulk_string(out, member);
    append_bulk_string(out, text);
    return;
  }

  const auto wire_bytes = detail::bulk_string_wire_size(member.size()) +
                          detail::bulk_string_wire_size(text.size());
  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  dst = detail::write_small_bulk_string(dst, member);
  (void)detail::write_small_bulk_string(dst, text);
}

inline void append_bulk_member_and_finite_double(std::string& out,
                                                 std::string_view member,
                                                 double score) {
  std::array<char, 32> score_buffer;
  std::string score_fallback;
  std::string_view score_text =
      score_format::try_format_finite_to_buffer(score, score_buffer);
  if (score_text.empty()) {
    score_fallback = score_format::fallback(score);
    score_text = score_fallback;
  }

  if (!detail::can_write_small_bulk_string(member.size()) ||
      !detail::can_write_small_bulk_string(score_text.size())) {
    append_bulk_string(out, member);
    append_bulk_string(out, score_text);
    return;
  }

  const auto wire_bytes =
      detail::bulk_string_wire_size(member.size()) +
      detail::bulk_string_wire_size(score_text.size());
  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  dst = detail::write_small_bulk_string(dst, member);
  (void)detail::write_small_bulk_string(dst, score_text);
}

inline constexpr std::size_t kBulkWithscoresBatchLimit = 256;
inline constexpr std::size_t kWithscoresStreamChunk = 16;

inline void append_bulk_withscores_chunk(
    std::string& out,
    std::span<const std::pair<std::string_view, double>> entries) {
  if (entries.empty()) {
    return;
  }

  struct FormattedEntry {
    std::string_view member;
    std::string_view score;
    std::array<char, 32> buffer{};
    std::string fallback;
  };

  std::array<FormattedEntry, kWithscoresStreamChunk> formatted{};
  std::size_t wire_bytes = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto& row = formatted[i];
    row.member = entries[i].first;
    row.score = score_format::try_format_finite_to_buffer(entries[i].second,
                                                          row.buffer);
    if (row.score.empty()) {
      row.fallback = score_format::fallback(entries[i].second);
      row.score = row.fallback;
    }

    if (!detail::can_write_small_bulk_string(row.member.size()) ||
        !detail::can_write_small_bulk_string(row.score.size())) {
      for (const auto& entry : entries) {
        append_bulk_member_and_finite_double(out, entry.first, entry.second);
      }
      return;
    }

    wire_bytes += detail::bulk_string_wire_size(row.member.size()) +
                  detail::bulk_string_wire_size(row.score.size());
  }

  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& row = formatted[i];
    dst = detail::write_small_bulk_string(dst, row.member);
    dst = detail::write_small_bulk_string(dst, row.score);
  }
}

inline void append_bulk_withscores_text_chunk(
    std::string& out,
    std::span<const std::pair<std::string_view, std::string_view>> entries) {
  if (entries.empty()) {
    return;
  }

  std::size_t wire_bytes = 0;
  for (const auto& entry : entries) {
    if (!detail::can_write_small_bulk_string(entry.first.size()) ||
        !detail::can_write_small_bulk_string(entry.second.size())) {
      for (const auto& fallback : entries) {
        append_bulk_member_and_text(out, fallback.first, fallback.second);
      }
      return;
    }
    wire_bytes += detail::bulk_string_wire_size(entry.first.size()) +
                  detail::bulk_string_wire_size(entry.second.size());
  }

  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  for (const auto& entry : entries) {
    dst = detail::write_small_bulk_string(dst, entry.first);
    dst = detail::write_small_bulk_string(dst, entry.second);
  }
}

inline void append_bulk_members_batch(std::string& out,
                                      std::span<const std::string_view> members) {
  if (members.empty()) {
    return;
  }

  if (members.size() > kBulkWithscoresBatchLimit) {
    for (const auto member : members) {
      append_bulk_string(out, member);
    }
    return;
  }

  std::size_t wire_bytes = 0;
  for (const auto member : members) {
    if (!detail::can_write_small_bulk_string(member.size())) {
      for (const auto fallback : members) {
        append_bulk_string(out, fallback);
      }
      return;
    }
    wire_bytes += detail::bulk_string_wire_size(member.size());
  }

  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  for (const auto member : members) {
    dst = detail::write_small_bulk_string(dst, member);
  }
}

inline void append_bulk_withscores_batch(
    std::string& out,
    std::span<const std::pair<std::string_view, double>> entries) {
  if (entries.empty()) {
    return;
  }

  struct FormattedEntry {
    std::string_view member;
    std::string_view score;
    std::array<char, 32> buffer{};
    std::string fallback;
  };

  if (entries.size() > kBulkWithscoresBatchLimit) {
    for (const auto& entry : entries) {
      append_bulk_member_and_finite_double(out, entry.first, entry.second);
    }
    return;
  }

  std::array<FormattedEntry, kBulkWithscoresBatchLimit> formatted{};
  std::size_t wire_bytes = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    auto& row = formatted[i];
    row.member = entries[i].first;
    row.score = score_format::try_format_finite_to_buffer(entries[i].second,
                                                          row.buffer);
    if (row.score.empty()) {
      row.fallback = score_format::fallback(entries[i].second);
      row.score = row.fallback;
    }

    if (!detail::can_write_small_bulk_string(row.member.size()) ||
        !detail::can_write_small_bulk_string(row.score.size())) {
      for (std::size_t j = 0; j < entries.size(); ++j) {
        append_bulk_member_and_finite_double(out, entries[j].first,
                                             entries[j].second);
      }
      return;
    }

    wire_bytes += detail::bulk_string_wire_size(row.member.size()) +
                  detail::bulk_string_wire_size(row.score.size());
  }

  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& row = formatted[i];
    dst = detail::write_small_bulk_string(dst, row.member);
    dst = detail::write_small_bulk_string(dst, row.score);
  }
}

inline void append_bulk_withscores_text_batch(
    std::string& out,
    std::span<const std::pair<std::string_view, std::string_view>> entries) {
  if (entries.empty()) {
    return;
  }

  if (entries.size() > kBulkWithscoresBatchLimit) {
    for (const auto& entry : entries) {
      append_bulk_member_and_text(out, entry.first, entry.second);
    }
    return;
  }

  std::size_t wire_bytes = 0;
  for (const auto& entry : entries) {
    if (!detail::can_write_small_bulk_string(entry.first.size()) ||
        !detail::can_write_small_bulk_string(entry.second.size())) {
      for (const auto& fallback : entries) {
        append_bulk_member_and_text(out, fallback.first, fallback.second);
      }
      return;
    }
    wire_bytes += detail::bulk_string_wire_size(entry.first.size()) +
                  detail::bulk_string_wire_size(entry.second.size());
  }

  const auto offset = out.size();
  out.resize(offset + wire_bytes);
  auto* dst = out.data() + offset;
  for (const auto& entry : entries) {
    dst = detail::write_small_bulk_string(dst, entry.first);
    dst = detail::write_small_bulk_string(dst, entry.second);
  }
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

inline void append_double(std::string& out, std::string_view value) {
  out.push_back(',');
  out.append(value);
  out.append("\r\n", 2);
}

inline void append_double(std::string& out, double value) {
  std::array<char, 32> buffer;
  std::string fallback;
  std::string_view text;
  if (std::isnan(value)) {
    text = "nan";
  } else if (std::isinf(value)) {
    text = value < 0 ? "-inf" : "inf";
  } else {
    text = score_format::try_format_finite_to_buffer(value, buffer);
    if (text.empty()) {
      fallback = score_format::fallback(value);
      text = fallback;
    }
  }
  append_double(out, text);
}

inline void append_null_bulk_string(std::string& out) {
  out.append("$-1\r\n");
}

inline void append_null(std::string& out, Version version) {
  if (version == Version::resp3) {
    out.append("_\r\n");
  } else {
    append_null_bulk_string(out);
  }
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

inline void append_push_header(std::string& out, std::size_t count) {
  if (count < detail::kSmallPushHeaders.size()) {
    const auto& header = detail::kSmallPushHeaders[count];
    out.append(header.bytes.data(), header.size);
  } else {
    out.push_back('>');
    detail::append_decimal(out, count);
    out.append("\r\n", 2);
  }
}

inline void append_map_header(std::string& out, std::size_t count) {
  out.push_back('%');
  detail::append_decimal(out, count);
  out.append("\r\n", 2);
}

[[nodiscard]] std::string simple_string(std::string_view value);
[[nodiscard]] std::string error(std::string_view message);
[[nodiscard]] std::string integer(long long value);
[[nodiscard]] std::string bulk_string(std::string_view value);
[[nodiscard]] std::string null_bulk_string();
[[nodiscard]] std::string array(std::span<const std::string_view> values);

}  // namespace goblin::core::resp
