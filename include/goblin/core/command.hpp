#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace goblin::core {

class Store;
class ScriptEngine;
class LuauEngine;
class WrenEngine;
class TclEngine;
class UPythonEngine;

enum class CommandType {
  ping,
  eval,
  evalsha,
  script,
  luau_eval,
  luau_evalsha,
  luau_script,
  wren_eval,
  wren_evalsha,
  wren_script,
  tcl_eval,
  tcl_evalsha,
  tcl_script,
  upython_eval,
  upython_evalsha,
  upython_script,
  zadd,
  zcard,
  zrange,
  zrank,
  zrevrange,
  zrevrank,
  zrem,
  zscore,
  hset,
  hsetnx,
  hget,
  hmget,
  hdel,
  hgetall,
  hkeys,
  hvals,
  hlen,
  hexists,
  hstrlen,
  hincrby,
  set,
  get,
  getset,
  setnx,
  getdel,
  strlen,
  append,
  incr,
  decr,
  incrby,
  decrby,
  incrbyfloat,
  getrange,
  setrange,
  mset,
  mget,
  del,
  exists,
  key_type,
  goblin_memory,
  goblin_optimize,
  goblin_save,
  goblin_load,
  echo,
  info,
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
  // When set, EVAL / EVALSHA / SCRIPT are dispatched to this engine. Left null on
  // the redis.call re-entry path (so a script cannot nest EVAL) and by callers
  // that do not enable scripting; those see the "not available" error instead.
  ScriptEngine* script_engine{nullptr};
  // The Luau counterpart, for LUAU.EVAL / LUAU.EVALSHA / LUAU.SCRIPT. Kept
  // separate so the two interpreters never share a cache or a VM.
  LuauEngine* luau_engine{nullptr};
  // The Wren counterpart, for WREN.EVAL / WREN.EVALSHA / WREN.SCRIPT.
  WrenEngine* wren_engine{nullptr};
  // The Tcl counterpart, for TCL.EVAL / TCL.EVALSHA / TCL.SCRIPT.
  TclEngine* tcl_engine{nullptr};
  // The MicroPython counterpart, for UPYTHON.EVAL / UPYTHON.EVALSHA / UPYTHON.SCRIPT.
  UPythonEngine* upython_engine{nullptr};
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
