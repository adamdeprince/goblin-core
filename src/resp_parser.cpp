#include "goblin/core/resp_parser.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace goblin::core {
namespace {

constexpr std::size_t kMaxArrayElements = 1024U * 1024U;
constexpr std::size_t kMaxBulkBytes = 128U * 1024U * 1024U;
constexpr std::size_t kMaxInlineBytes = 1024U * 1024U;

[[nodiscard]] std::optional<long long> parse_i64(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }

  long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }

  return value;
}

[[nodiscard]] std::optional<std::size_t> find_crlf(std::string_view buffer,
                                                   std::size_t offset) {
  const auto pos = buffer.find("\r\n", offset);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  return pos;
}

[[nodiscard]] RespParser::ParseResult incomplete() {
  return {.state = RespParser::ParseState::incomplete};
}

[[nodiscard]] RespParser::ParseResult parse_error(std::string message) {
  return {
      .state = RespParser::ParseState::error,
      .error = std::move(message),
  };
}

[[nodiscard]] bool is_inline_separator(char c) noexcept {
  return c == ' ' || c == '\t';
}

}  // namespace

void RespParser::append(std::string_view bytes) {
  if (bytes.empty() || !error_.empty()) {
    return;
  }

  if (frames_.empty()) {
    compact_consumed_prefix();
  }
  buffer_.append(bytes);
  parse_available();
}

std::optional<CommandFrame> RespParser::pop() {
  if (frames_.empty()) {
    compact_consumed_prefix();
    return std::nullopt;
  }

  auto queued = std::move(frames_.front());
  frames_.pop_front();

  CommandFrame frame;
  frame.fields.reserve(queued.fields.size());
  for (const auto field : queued.fields) {
    frame.fields.emplace_back(buffer_.data() + field.offset, field.length);
  }
  popped_until_ = std::max(popped_until_, queued.consumed);
  return frame;
}

bool RespParser::has_queued_frames() const noexcept {
  return !frames_.empty();
}

bool RespParser::has_error() const noexcept {
  return !error_.empty();
}

const std::string& RespParser::error() const noexcept {
  return error_;
}

void RespParser::clear() {
  buffer_.clear();
  frames_.clear();
  error_.clear();
  parse_offset_ = 0;
  popped_until_ = 0;
}

RespParser::ParseResult RespParser::parse_one(std::size_t offset) const {
  if (offset >= buffer_.size()) {
    return incomplete();
  }

  if (buffer_[offset] == '*') {
    return parse_resp_array(offset);
  }

  return parse_inline(offset);
}

RespParser::ParseResult RespParser::parse_resp_array(std::size_t offset) const {
  const std::string_view buffer(buffer_);
  const auto line_end = find_crlf(buffer, offset + 1);
  if (!line_end) {
    return incomplete();
  }

  const auto element_count =
      parse_i64(buffer.substr(offset + 1, *line_end - offset - 1));
  if (!element_count || *element_count < 0) {
    return parse_error("ERR Protocol error: invalid multibulk length");
  }
  if (static_cast<unsigned long long>(*element_count) > kMaxArrayElements) {
    return parse_error("ERR Protocol error: multibulk length is too large");
  }

  std::size_t cursor = *line_end + 2;
  QueuedFrame frame;
  frame.fields.reserve(static_cast<std::size_t>(*element_count));

  for (long long i = 0; i < *element_count; ++i) {
    if (cursor >= buffer.size()) {
      return incomplete();
    }
    if (buffer[cursor] != '$') {
      return parse_error("ERR Protocol error: expected bulk string");
    }

    const auto bulk_line_end = find_crlf(buffer, cursor + 1);
    if (!bulk_line_end) {
      return incomplete();
    }

    const auto bulk_len = parse_i64(buffer.substr(cursor + 1, *bulk_line_end - cursor - 1));
    if (!bulk_len || *bulk_len < 0) {
      return parse_error("ERR Protocol error: invalid bulk length");
    }
    if (static_cast<unsigned long long>(*bulk_len) > kMaxBulkBytes) {
      return parse_error("ERR Protocol error: bulk length is too large");
    }

    cursor = *bulk_line_end + 2;
    const auto len = static_cast<std::size_t>(*bulk_len);
    if (buffer.size() < cursor + len + 2) {
      return incomplete();
    }
    if (buffer[cursor + len] != '\r' || buffer[cursor + len + 1] != '\n') {
      return parse_error("ERR Protocol error: invalid bulk terminator");
    }

    frame.fields.push_back(FieldRange{.offset = cursor, .length = len});
    cursor += len + 2;
  }

  return {
      .state = ParseState::complete,
      .consumed = cursor,
      .frame = std::move(frame),
  };
}

RespParser::ParseResult RespParser::parse_inline(std::size_t offset) const {
  const auto line_end = buffer_.find("\r\n", offset);
  if (line_end == std::string::npos) {
    if (buffer_.size() - offset > kMaxInlineBytes) {
      return parse_error("ERR Protocol error: inline command is too large");
    }
    return incomplete();
  }

  std::string_view line(buffer_.data() + offset, line_end - offset);
  QueuedFrame frame;

  std::size_t cursor = 0;
  while (cursor < line.size()) {
    while (cursor < line.size() && is_inline_separator(line[cursor])) {
      ++cursor;
    }
    if (cursor >= line.size()) {
      break;
    }

    const auto token_start = cursor;
    while (cursor < line.size() && !is_inline_separator(line[cursor])) {
      ++cursor;
    }
    frame.fields.push_back(FieldRange{.offset = offset + token_start,
                                      .length = cursor - token_start});
  }

  return {
      .state = ParseState::complete,
      .consumed = line_end + 2,
      .frame = std::move(frame),
  };
}

void RespParser::parse_available() {
  while (parse_offset_ < buffer_.size() && error_.empty()) {
    auto result = parse_one(parse_offset_);
    switch (result.state) {
      case ParseState::complete:
        parse_offset_ = result.consumed;
        result.frame.consumed = result.consumed;
        frames_.push_back(std::move(result.frame));
        break;
      case ParseState::incomplete:
        return;
      case ParseState::error:
        error_ = std::move(result.error);
        buffer_.clear();
        frames_.clear();
        parse_offset_ = 0;
        popped_until_ = 0;
        return;
    }
  }
}

void RespParser::compact_consumed_prefix() {
  if (popped_until_ == 0 || !frames_.empty()) {
    return;
  }

  buffer_.erase(0, popped_until_);
  parse_offset_ -= std::min(parse_offset_, popped_until_);
  popped_until_ = 0;
}

}  // namespace goblin::core
