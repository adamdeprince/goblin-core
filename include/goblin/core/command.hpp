#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "goblin/core/resp_version.hpp"
#include "goblin/core/nested_command_dispatch.hpp"

namespace goblin::core {

class Store;
class AuthDatabase;
class ScriptEngine;
class LuauEngine;
class WrenEngine;
class TclEngine;
class UPythonEngine;
class QuickJsEngine;

enum class CommandType {
  ping,
  hello,
  auth,
  command,
  config,
  client,
  select,
  quit,
  multi,
  exec,
  discard,
  watch,
  unwatch,
  time,
  role,
  goblin_firehose,
  subscribe,
  unsubscribe,
  psubscribe,
  punsubscribe,
  publish,
  pubsub,
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
  quickjs_eval,
  quickjs_evalsha,
  quickjs_script,
  zadd,
  zincrby,
  zcard,
  zcount,
  zrange,
  zrangebyscore,
  zrevrangebyscore,
  zrank,
  zrevrange,
  zrevrank,
  zrem,
  zremrangebyscore,
  zremrangebyrank,
  zinterstore,
  zunionstore,
  zmscore,
  zpopmin,
  zpopmax,
  zscan,
  zscore,
  hset,
  hmset,
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
  hincrbyfloat,
  hrandfield,
  hscan,
  sadd,
  srem,
  scard,
  sismember,
  smismember,
  smembers,
  spop,
  srandmember,
  smove,
  sinter,
  sinterstore,
  sintercard,
  sunion,
  sunionstore,
  sdiff,
  sdiffstore,
  sscan,
  arreserve,
  arset,
  arget,
  armset,
  armget,
  arlen,
  arcount,
  ardel,
  arinsert,
  arnext,
  arseek,
  lpush,
  rpush,
  lpushx,
  rpushx,
  lpop,
  rpop,
  lmove,
  rpoplpush,
  blpop,
  brpop,
  blmove,
  lmpop,
  blmpop,
  llen,
  lindex,
  lrange,
  lset,
  ltrim,
  lrem,
  linsert,
  pma_lpush,
  pma_rpush,
  pma_lpushx,
  pma_rpushx,
  pma_lpop,
  pma_rpop,
  pma_lmove,
  pma_rpoplpush,
  pma_blpop,
  pma_brpop,
  pma_blmove,
  pma_lmpop,
  pma_blmpop,
  pma_llen,
  pma_lindex,
  pma_lrange,
  pma_lset,
  pma_ltrim,
  pma_lrem,
  pma_linsert,
  segmented_lpush,
  segmented_rpush,
  segmented_lpushx,
  segmented_rpushx,
  segmented_lpop,
  segmented_rpop,
  segmented_lmove,
  segmented_rpoplpush,
  segmented_blpop,
  segmented_brpop,
  segmented_blmove,
  segmented_lmpop,
  segmented_blmpop,
  segmented_llen,
  segmented_lindex,
  segmented_lrange,
  segmented_lset,
  segmented_ltrim,
  segmented_lrem,
  segmented_linsert,
  set,
  get,
  getex,
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
  msetnx,
  mget,
  del,
  exists,
  dbsize,
  rename,
  renamenx,
  copy,
  randomkey,
  touch,
  scan,
  key_type,
  expire,
  pexpire,
  expireat,
  pexpireat,
  ttl,
  pttl,
  persist,
  expiretime,
  pexpiretime,
  goblin_memory,
  goblin_optimize,
  goblin_save,
  goblin_load,
  goblin_cad,
  goblin_caexpire,
  goblin_cas,
  goblin_td_leaderboard_rescore,
  goblin_increx,
  goblin_zwindow,
  goblin_incrbound,
  goblin_decrpos,
  goblin_hcad,
  goblin_hsetgt,
  goblin_claim,
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
  bool range_by_score{false};
  bool range_reverse{false};
  bool range_has_limit{false};
  long long range_limit_offset{0};
  long long range_limit_count{-1};
  // Parsed list move/pop shape. For LMPOP/BLMPOP, list_key_offset points to the
  // first key in args and list_key_count is the declared numkeys. Blocking
  // commands store a finite non-negative timeout in seconds; zero is indefinite.
  std::size_t list_key_offset{0};
  std::size_t list_key_count{0};
  std::size_t list_count{1};
  double list_timeout_seconds{0.0};
  bool list_pop_front{true};
  bool list_push_front{true};
  // Set once at parse for GOBLIN.RT.* / GOBLIN.EFFICENT.* / GOBLIN.CLASSIC.*
  // so execute can select hash vs array implementation without re-scanning.
  // 0 = default store policy, 1 = non-RT qualified family, 2 = Realtime.
  std::uint8_t hash_implementation_tag{0};
};

struct CommandParseResult {
  std::optional<Command> command;
  std::string error;

  [[nodiscard]] bool ok() const noexcept { return command.has_value(); }
};

struct BlockingListDispatch {
  void* context{nullptr};
  // Called only after a valid blocking command finds no immediately available
  // element. Returning true means the live connection was parked and no reply
  // should be emitted yet. Direct/script/transaction callers leave this null and
  // receive the command's ordinary null reply instead.
  bool (*park)(void*, const Command&){nullptr};
};

struct CommandExecutionOptions {
  std::size_t output_reserve_limit{0};
  // RESP protocol state belongs to the connection. HELLO updates this value in
  // place, so later commands in the same pipeline use the newly selected wire.
  // A null pointer means RESP2 and is used by direct command/unit-test callers.
  resp::Version* resp_version{nullptr};
  std::uint64_t connection_id{0};
  // Null means the server has no auth file. Otherwise AUTH and HELLO AUTH verify
  // against this immutable startup snapshot, even on a trusted transport that
  // does not require clients to authenticate.
  const AuthDatabase* auth_database{nullptr};
  bool* authenticated{nullptr};
  std::string* authenticated_username{nullptr};
  std::string* client_name{nullptr};
  std::string* client_library_name{nullptr};
  std::string* client_library_version{nullptr};
  bool* quit_requested{nullptr};
  // Server-owned replication hooks. Direct/unit-test callers leave these null.
  // The write callback runs after a successful logical mutation and receives
  // only the reply bytes produced by that command.
  void* replication_context{nullptr};
  void (*replicate_write)(void*, Store&, const Command&, std::string_view) noexcept{
      nullptr};
  void (*render_role)(void*, std::string&, resp::Version){nullptr};
  BlockingListDispatch blocking_lists{};
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
  // The QuickJS counterpart, for QUICKJS.EVAL / QUICKJS.EVALSHA / QUICKJS.SCRIPT.
  QuickJsEngine* quickjs_engine{nullptr};
  // Services retained when a script re-enters command dispatch. Interpreter
  // pointers are deliberately omitted there, so scripts still cannot nest.
  NestedCommandDispatch nested_dispatch{};
};

[[nodiscard]] CommandParseResult parse_command(std::span<const std::string_view> fields);

// Perfect-hash lookup without arity validation, used by COMMAND INFO.
[[nodiscard]] CommandType lookup_command_type(std::string_view name) noexcept;

// True only for commands that mutate logical keyspace state. Administrative,
// connection, Pub/Sub, scripting, and read-only commands are deliberately
// excluded. Kafka replay uses this as a strict allowlist.
[[nodiscard]] bool command_mutates_store(CommandType type) noexcept;

// The INFO text (server/memory fields), for callers that reply it directly (e.g. the
// SBE dispatch) rather than through a Command.
[[nodiscard]] std::string render_server_info(const Store& store);

// GOBLIN.MEMORY's flat [name, value, ...] fields for a zset/hash key, or nullopt if
// the key is neither. The SBE dispatch shapes these into a map reply.
[[nodiscard]] std::optional<std::vector<std::string>> goblin_memory_fields(const Store& store,
                                                                          std::string_view key);

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
