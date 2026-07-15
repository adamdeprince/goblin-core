#pragma once

#include "goblin/core/nested_command_dispatch.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Luau's lua_State is a distinct type from PUC-Lua's; forward-declare it so this
// header stays free of the vendored Luau headers (which are C++-linkage and must
// not be included in the same translation unit as PUC-Lua's).
struct lua_State;

namespace goblin::core {

class Store;

// The Luau counterpart to ScriptEngine: implements the LUAU.EVAL / LUAU.EVALSHA /
// LUAU.SCRIPT command family on Roblox's Luau runtime, kept deliberately separate
// from the PUC-Lua engine so the two interpreters (and their script caches) never
// mix. Same integration shape: lazy VM, redis.call re-enters the command
// dispatch pipeline, atomic under the single-threaded server loop.
//
// Isolation differs from the PUC engine: after building the shared global table
// the base state is frozen with luaL_sandbox, and every script runs on its own
// sandboxed thread (luaL_sandboxthread), so one script can never observe or
// corrupt another's globals.
class LuauEngine {
 public:
  explicit LuauEngine(Store& store, NestedCommandDispatch nested_dispatch = {});
  ~LuauEngine();

  LuauEngine(const LuauEngine&) = delete;
  LuauEngine& operator=(const LuauEngine&) = delete;

  // LUAU.EVAL script numkeys [key ...] [arg ...]   (args excludes command name)
  void eval(std::span<const std::string_view> args, std::string& out);
  // LUAU.EVALSHA sha1 numkeys [key ...] [arg ...]
  void eval_sha(std::span<const std::string_view> args, std::string& out);
  // LUAU.SCRIPT LOAD|EXISTS|FLUSH ...
  void script(std::span<const std::string_view> args, std::string& out);

 private:
  friend struct LuauBridge;

  void ensure_vm();
  bool compile_ok(std::string_view body, std::string& out);  // caches on success
  void run(std::string_view body,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  int redis_call_impl(lua_State* L, bool raise_on_error);

  Store& store_;
  NestedCommandDispatch nested_dispatch_;
  lua_State* L_ = nullptr;  // the frozen base state; scripts run on child threads
  std::unordered_map<std::string, std::string> scripts_;  // 40-hex SHA1 -> body

  // Reusable, engine-owned scratch (never aliased: scripts do not nest).
  std::string call_reply_;
  std::vector<std::string_view> call_args_;
};

}  // namespace goblin::core
