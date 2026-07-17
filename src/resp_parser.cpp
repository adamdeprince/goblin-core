#include "goblin/core/resp_parser.hpp"

#include "goblin/core/parse_int.hpp"
#include "goblin/core/simd_ops.hpp"

#include <algorithm>
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

using goblin::core::parse_i64;

[[nodiscard]] std::optional<std::size_t> find_crlf(std::string_view buffer,
                                                   std::size_t offset) {
  return simd::find_crlf(buffer, offset);
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

  const auto queued = frames_.front();
  frames_.pop_front();

  CommandFrame frame;
  frame.fields.reserve(queued.field_count);
  for (std::size_t i = 0; i < queued.field_count; ++i) {
    const auto field = field_pool_[queued.field_begin + i];
    frame.fields.emplace_back(buffer_.data() + field.offset, field.length);
  }
  popped_until_ = std::max(popped_until_, queued.consumed);
  return frame;
}

bool RespParser::pop_into(std::vector<std::string_view>& out) {
  if (frames_.empty()) {
    compact_consumed_prefix();
    return false;
  }

  const auto queued = frames_.front();
  frames_.pop_front();

  out.clear();
  for (std::size_t i = 0; i < queued.field_count; ++i) {
    const auto field = field_pool_[queued.field_begin + i];
    out.emplace_back(buffer_.data() + field.offset, field.length);
  }
  popped_until_ = std::max(popped_until_, queued.consumed);
  return true;
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
  field_pool_.clear();
  error_.clear();
  parse_offset_ = 0;
  popped_until_ = 0;
}

RespParser::ParseResult RespParser::parse_one(
    std::size_t offset,
    std::vector<FieldRange>& out_fields) const {
  if (offset >= buffer_.size()) {
    return incomplete();
  }

  if (buffer_[offset] == '*') {
    return parse_resp_array(offset, out_fields);
  }

  return parse_inline(offset, out_fields);
}

RespParser::ParseResult RespParser::parse_resp_array(
    std::size_t offset,
    std::vector<FieldRange>& out_fields) const {
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
  out_fields.reserve(out_fields.size() + static_cast<std::size_t>(*element_count));

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

    out_fields.push_back(FieldRange{.offset = cursor, .length = len});
    cursor += len + 2;
  }

  return {
      .state = ParseState::complete,
      .consumed = cursor,
  };
}

RespParser::ParseResult RespParser::parse_inline(
    std::size_t offset,
    std::vector<FieldRange>& out_fields) const {
  const auto line_end = simd::find_crlf(buffer_, offset);
  if (!line_end) {
    if (buffer_.size() - offset > kMaxInlineBytes) {
      return parse_error("ERR Protocol error: inline command is too large");
    }
    return incomplete();
  }

  std::string_view line(buffer_.data() + offset, *line_end - offset);

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
    out_fields.push_back(FieldRange{.offset = offset + token_start,
                                    .length = cursor - token_start});
  }

  return {
      .state = ParseState::complete,
      .consumed = *line_end + 2,
  };
}

void RespParser::parse_available() {
  while (parse_offset_ < buffer_.size() && error_.empty()) {
    const auto field_begin = field_pool_.size();
    auto result = parse_one(parse_offset_, field_pool_);
    switch (result.state) {
      case ParseState::complete:
        parse_offset_ = result.consumed;
        frames_.push_back(QueuedFrame{
            .field_begin = field_begin,
            .field_count = field_pool_.size() - field_begin,
            .consumed = result.consumed,
        });
        break;
      case ParseState::incomplete:
        // Discard any field ranges the partial parse appended; they will be
        // re-parsed once the rest of the command arrives.
        field_pool_.resize(field_begin);
        return;
      case ParseState::error:
        error_ = std::move(result.error);
        buffer_.clear();
        frames_.clear();
        field_pool_.clear();
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
  // No queued frame references the pool once it has fully drained, so the
  // accumulated field ranges (whose offsets are about to be invalidated by the
  // buffer erase) can be reset while retaining capacity for reuse.
  field_pool_.clear();
}

}  // namespace goblin::core
