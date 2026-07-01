#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core {

struct CommandFrame {
  std::vector<std::string_view> fields;
};

class RespParser {
 public:
  enum class ParseState {
    complete,
    incomplete,
    error,
  };

  struct FieldRange {
    std::size_t offset{0};
    std::size_t length{0};
  };

  struct QueuedFrame {
    std::vector<FieldRange> fields;
    std::size_t consumed{0};
  };

  struct ParseResult {
    ParseState state{ParseState::incomplete};
    std::size_t consumed{0};
    QueuedFrame frame;
    std::string error;
  };

  void append(std::string_view bytes);

  [[nodiscard]] std::optional<CommandFrame> pop();
  [[nodiscard]] bool has_queued_frames() const noexcept;
  [[nodiscard]] bool has_error() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;

  void clear();

 private:
  [[nodiscard]] ParseResult parse_one(std::size_t offset) const;
  [[nodiscard]] ParseResult parse_resp_array(std::size_t offset) const;
  [[nodiscard]] ParseResult parse_inline(std::size_t offset) const;
  void parse_available();
  void compact_consumed_prefix();

  std::string buffer_;
  std::deque<QueuedFrame> frames_;
  std::string error_;
  std::size_t parse_offset_{0};
  std::size_t popped_until_{0};
};

}  // namespace goblin::core
