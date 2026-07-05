#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace goblin::core {

class Store;

enum class CommandType {
  ping,
  zadd,
  zcard,
  zrange,
  zrank,
  zrevrange,
  zrevrank,
  zrem,
  zscore,
  goblin_memory,
  goblin_optimize,
  goblin_save,
  goblin_load,
  unknown,
};

struct Command {
  CommandType type{CommandType::unknown};
  std::string_view name;
  std::span<const std::string_view> args;
  bool with_scores{false};
  bool range_indexes_parsed{false};
  long long range_start{0};
  long long range_stop{0};
};

struct CommandParseResult {
  std::optional<Command> command;
  std::string error;

  [[nodiscard]] bool ok() const noexcept { return command.has_value(); }
};

struct CommandExecutionOptions {
  std::size_t output_reserve_limit{0};
};

[[nodiscard]] CommandParseResult parse_command(std::span<const std::string_view> fields);

void execute_command_into(Store& store,
                          const Command& command,
                          std::string& out,
                          CommandExecutionOptions options = {});

[[nodiscard]] std::string execute_command(Store& store, const Command& command);

void handle_command_into(Store& store,
                         std::span<const std::string_view> fields,
                         std::string& out,
                         CommandExecutionOptions options = {});

[[nodiscard]] std::string handle_command(Store& store,
                                         std::span<const std::string_view> fields);

}  // namespace goblin::core
