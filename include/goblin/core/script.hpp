#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Lua is an implementation detail: the header forward-declares lua_State so that
// nothing outside src/script.cpp needs the vendored Lua headers on its include
// path, and so consumers of goblin_core never link Lua directly.
struct lua_State;

namespace goblin::core {

class Store;

// Embeds the vendored Lua 5.1 runtime and implements the EVAL / EVALSHA / SCRIPT
// command family from the public Redis scripting specification (no Redis source
// was consulted). One engine is owned by the server for the process lifetime; it
// holds the script cache (SHA1 -> body) and, once the first script runs, a single
// lua_State.
//
// Memory: the lua_State is created lazily on the first script (ensure_vm). A
// server that never receives an EVAL pays nothing beyond an empty hash map, so
// the idle resident-set size -- the project's headline number -- is unchanged.
//
// Concurrency: the server is single-threaded and a script never re-enters EVAL
// (nested EVAL/EVALSHA/SCRIPT are rejected inside redis.call), so a script runs
// atomically and the reusable scratch buffers below are never aliased.
class ScriptEngine {
 public:
  explicit ScriptEngine(Store& store);
  ~ScriptEngine();

  ScriptEngine(const ScriptEngine&) = delete;
  ScriptEngine& operator=(const ScriptEngine&) = delete;

  // EVAL script numkeys [key ...] [arg ...]   (args excludes the command name)
  void eval(std::span<const std::string_view> args, std::string& out);
  // EVALSHA sha1 numkeys [key ...] [arg ...]
  void eval_sha(std::span<const std::string_view> args, std::string& out);
  // SCRIPT LOAD|EXISTS|FLUSH ...
  void script(std::span<const std::string_view> args, std::string& out);

 private:
  // The Lua trampolines in src/script.cpp reach the two members below through
  // this friend (they run as lua_CFunction callbacks).
  friend struct LuaBridge;

  void ensure_vm();
  // Compile `body`, run it with KEYS/ARGV bound, and append the RESP reply (or a
  // RESP error) to `out`. `name` is the chunk name used in error messages.
  void run(std::string_view body,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  // Trampolines and the protected runner reach the engine through a lightuserdata
  // upvalue; these do the actual work with the engine's own buffers.
  int redis_call_impl(lua_State* L, bool raise_on_error);
  int eval_runner(lua_State* L);

  Store& store_;
  lua_State* L_ = nullptr;
  std::unordered_map<std::string, std::string> scripts_;  // 40-hex SHA1 -> body

  // Reusable, engine-owned scratch. These live here rather than on the trampoline
  // stack so a Lua error (longjmp) never unwinds past a live C++ destructor.
  std::string call_reply_;                       // RESP captured from a redis.call
  std::vector<std::string_view> call_args_;       // fields for that redis.call
  std::string reply_scratch_;                     // RESP built from a script's return
  std::span<const std::string_view> run_keys_;    // KEYS for the in-flight script
  std::span<const std::string_view> run_argv_;    // ARGV for the in-flight script
};

}  // namespace goblin::core
