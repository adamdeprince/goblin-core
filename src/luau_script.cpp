#include "goblin/core/luau_script.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/detail/script_shared.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// Luau's headers are C++ (C++ linkage), so unlike the PUC-Lua engine they are NOT
// wrapped in extern "C". Because the symbols are mangled they never collide with
// goblin_lua's C-linkage Lua symbols even though both derive from Lua. Luau raises
// errors via C++ exceptions caught by lua_pcall, so a trampoline error unwinds
// cleanly; the engine-owned scratch below is kept for reuse and reentrancy safety.
#include "luacode.h"
#include "lua.h"
#include "lualib.h"

namespace goblin::core {

using namespace script_shared;

// Trampolines that need the engine reach it through a lightuserdata upvalue.
struct LuauBridge {
  [[nodiscard]] static LuauEngine* engine(lua_State* L) {
    return static_cast<LuauEngine*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
  }
  static int redis_call(lua_State* L) { return engine(L)->redis_call_impl(L, true); }
  static int redis_pcall(lua_State* L) { return engine(L)->redis_call_impl(L, false); }
};

namespace {

void raise_string(lua_State* L, const char* message) {
  lua_pushlstring(L, message, std::strlen(message));
  lua_error(L);
}

// RESP reply -> Luau value. Same rules as the PUC engine, except a RESP integer
// is pushed as a Lua number (Luau's lua_pushinteger is 32-bit, which would clip a
// 64-bit reply). The buffer holds exactly one reply.
void resp_reply_to_lua(lua_State* L, const char** pp, const char* end,
                       bool* is_error) {
  luaL_checkstack(L, 6, "resp reply too deep");
  const char* p = *pp;
  if (p >= end) {
    lua_pushboolean(L, 0);
    *pp = end;
    return;
  }

  const char type = *p++;
  std::string_view line;
  switch (type) {
    case '+':
      p = read_line(p, end, &line);
      lua_createtable(L, 0, 1);
      lua_pushlstring(L, line.data(), line.size());
      lua_setfield(L, -2, "ok");
      break;
    case '-':
      p = read_line(p, end, &line);
      lua_createtable(L, 0, 1);
      lua_pushlstring(L, line.data(), line.size());
      lua_setfield(L, -2, "err");
      if (is_error != nullptr) *is_error = true;
      break;
    case ':':
      p = read_line(p, end, &line);
      lua_pushnumber(L, static_cast<double>(parse_signed(line)));
      break;
    case '$': {
      p = read_line(p, end, &line);
      const long long len = parse_signed(line);
      if (len < 0) {
        lua_pushboolean(L, 0);
      } else {
        const char* payload = p;
        p += len;
        lua_pushlstring(L, payload, static_cast<std::size_t>(len));
        if (p + 2 <= end) p += 2;
      }
      break;
    }
    case '*': {
      p = read_line(p, end, &line);
      const long long count = parse_signed(line);
      if (count < 0) {
        lua_pushboolean(L, 0);
      } else {
        lua_createtable(L, static_cast<int>(count > 0 ? count : 0), 0);
        for (long long i = 0; i < count; ++i) {
          resp_reply_to_lua(L, &p, end, nullptr);
          lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
      }
      break;
    }
    default:
      lua_pushboolean(L, 0);
      p = end;
      break;
  }
  *pp = p;
}

// Luau value -> RESP reply (a script's return value); same rules as the PUC engine.
void lua_value_to_resp(lua_State* L, int index, std::string& out) {
  luaL_checkstack(L, 4, "resp return too deep");
  index = lua_absindex(L, index);
  switch (lua_type(L, index)) {
    case LUA_TNIL:
      resp::append_null_bulk_string(out);
      return;
    case LUA_TBOOLEAN:
      if (lua_toboolean(L, index)) {
        resp::append_integer(out, 1);
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    case LUA_TNUMBER: {
      const double number = lua_tonumber(L, index);
      const long long truncated =
          std::isfinite(number) ? static_cast<long long>(number) : 0;
      resp::append_integer(out, truncated);
      return;
    }
    case LUA_TSTRING: {
      std::size_t len = 0;
      const char* s = lua_tolstring(L, index, &len);
      resp::append_bulk_string(out, std::string_view(s, len));
      return;
    }
    case LUA_TTABLE: {
      lua_getfield(L, index, "err");
      if (lua_type(L, -1) == LUA_TSTRING) {
        std::size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        resp::append_error(out, sanitize_line(std::string_view(s, len)));
        lua_pop(L, 1);
        return;
      }
      lua_pop(L, 1);

      lua_getfield(L, index, "ok");
      if (lua_type(L, -1) == LUA_TSTRING) {
        std::size_t len = 0;
        const char* s = lua_tolstring(L, -1, &len);
        resp::append_simple_string(out, sanitize_line(std::string_view(s, len)));
        lua_pop(L, 1);
        return;
      }
      lua_pop(L, 1);

      long long count = 0;
      for (;; ++count) {
        lua_rawgeti(L, index, static_cast<int>(count + 1));
        const bool nil = lua_type(L, -1) == LUA_TNIL;
        lua_pop(L, 1);
        if (nil) break;
      }
      resp::append_array_header(out, static_cast<std::size_t>(count));
      for (long long i = 0; i < count; ++i) {
        lua_rawgeti(L, index, static_cast<int>(i + 1));
        lua_value_to_resp(L, -1, out);
        lua_pop(L, 1);
      }
      return;
    }
    default:
      resp::append_null_bulk_string(out);
      return;
  }
}

void set_global_string_table(lua_State* L, const char* name,
                             std::span<const std::string_view> items) {
  lua_createtable(L, static_cast<int>(items.size()), 0);
  for (std::size_t i = 0; i < items.size(); ++i) {
    lua_pushlstring(L, items[i].data(), items[i].size());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_setglobal(L, name);
}

int l_redis_error_reply(lua_State* L) {
  std::size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  lua_createtable(L, 0, 1);
  lua_pushlstring(L, s, len);
  lua_setfield(L, -2, "err");
  return 1;
}

int l_redis_status_reply(lua_State* L) {
  std::size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  lua_createtable(L, 0, 1);
  lua_pushlstring(L, s, len);
  lua_setfield(L, -2, "ok");
  return 1;
}

int l_redis_sha1hex(lua_State* L) {
  std::size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  char hex[40];
  sha1_hex_into(std::string_view(s, len), hex);
  lua_pushlstring(L, hex, 40);
  return 1;
}

int l_redis_log(lua_State* L) {
  const int n = lua_gettop(L);
  if (n < 2) luaL_error(L, "redis.log() requires two arguments or more.");  // noreturn
  if (lua_type(L, 1) != LUA_TNUMBER) {
    luaL_error(L, "First argument must be a number (log level).");  // noreturn
  }
  for (int i = 2; i <= n; ++i) {
    std::size_t len = 0;
    (void)lua_tolstring(L, i, &len);  // force to string first (never over a C++ local)
  }
  const int level = static_cast<int>(lua_tonumber(L, 1));
  std::string message;
  for (int i = 2; i <= n; ++i) {
    std::size_t len = 0;
    const char* s = lua_tolstring(L, i, &len);
    if (s == nullptr) continue;
    if (i > 2) message.push_back(' ');
    message.append(s, len);
  }
  std::fprintf(stderr, "goblin-core luau script (level %d): %s\n", level,
               message.c_str());
  return 0;
}

int l_redis_setresp(lua_State* L) {
  const int resp = luaL_checkinteger(L, 1);
  if (resp != 2 && resp != 3) luaL_error(L, "RESP version must be 2 or 3.");  // noreturn
  return 0;
}

int l_redis_replicate_commands(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}

int l_redis_set_repl(lua_State* L) {
  (void)luaL_checkinteger(L, 1);
  return 0;
}

int l_redis_noop_false(lua_State* L) {
  lua_pushboolean(L, 0);
  return 1;
}

void register_function(lua_State* L, int table, const char* name, lua_CFunction fn) {
  lua_pushcfunction(L, fn, name);
  lua_setfield(L, table, name);
}

}  // namespace

int LuauEngine::redis_call_impl(lua_State* L, bool raise_on_error) {
  const int nargs = lua_gettop(L);
  if (nargs < 1) {
    raise_string(L, "Please specify at least one argument for this redis lib call");
    return 0;  // unreachable; lua_error does not return
  }

  for (int i = 1; i <= nargs; ++i) {
    const int type = lua_type(L, i);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
      raise_string(L, "Lua redis lib command arguments must be strings or integers");
      return 0;
    }
  }
  call_args_.clear();
  call_args_.reserve(static_cast<std::size_t>(nargs));
  for (int i = 1; i <= nargs; ++i) {
    std::size_t len = 0;
    const char* s = lua_tolstring(L, i, &len);
    call_args_.emplace_back(s, len);
  }

  if (command_blocked_in_script(call_args_[0])) {
    raise_string(L, "This Redis command is not allowed from script");
    return 0;
  }

  // No engines in the options: a script cannot nest EVAL / LUAU.EVAL.
  call_reply_.clear();
  handle_command_into(
      store_, call_args_, call_reply_,
      CommandExecutionOptions{
          .replication_context = nested_dispatch_.replication_context,
          .replicate_write = nested_dispatch_.replicate_write,
          .read_only = nested_dispatch_.read_only,
          .nested_dispatch = nested_dispatch_});

  bool is_error = false;
  const char* p = call_reply_.data();
  const char* end = p + call_reply_.size();
  resp_reply_to_lua(L, &p, end, &is_error);

  if (is_error && raise_on_error) {
    lua_error(L);  // the { err = ... } table on top becomes the error object
  }
  return 1;
}

LuauEngine::LuauEngine(Store& store, NestedCommandDispatch nested_dispatch)
    : store_(store), nested_dispatch_(nested_dispatch) {}

LuauEngine::~LuauEngine() {
  if (L_ != nullptr) {
    lua_close(L_);
    L_ = nullptr;
  }
}

void LuauEngine::ensure_vm() {
  if (L_ != nullptr) return;

  L_ = luaL_newstate();
  if (L_ == nullptr) return;
  lua_State* L = L_;

  // Luau's standard library is already sandbox-safe (no io/package/require; os is
  // limited to time/clock/date). It adds bit32, buffer, utf8, and vector natively.
  luaL_openlibs(L);

  lua_newtable(L);
  const int redis = lua_gettop(L);
  lua_pushlightuserdatatagged(L, this, 0);
  lua_pushcclosure(L, &LuauBridge::redis_call, "redis.call", 1);
  lua_setfield(L, redis, "call");
  lua_pushlightuserdatatagged(L, this, 0);
  lua_pushcclosure(L, &LuauBridge::redis_pcall, "redis.pcall", 1);
  lua_setfield(L, redis, "pcall");
  register_function(L, redis, "error_reply", l_redis_error_reply);
  register_function(L, redis, "status_reply", l_redis_status_reply);
  register_function(L, redis, "sha1hex", l_redis_sha1hex);
  register_function(L, redis, "log", l_redis_log);
  register_function(L, redis, "setresp", l_redis_setresp);
  register_function(L, redis, "replicate_commands", l_redis_replicate_commands);
  register_function(L, redis, "set_repl", l_redis_set_repl);
  register_function(L, redis, "breakpoint", l_redis_noop_false);
  register_function(L, redis, "debug", l_redis_noop_false);

  struct Constant {
    const char* name;
    int value;
  };
  for (const Constant& c : {Constant{"LOG_DEBUG", 0}, Constant{"LOG_VERBOSE", 1},
                            Constant{"LOG_NOTICE", 2}, Constant{"LOG_WARNING", 3},
                            Constant{"REPL_NONE", 0}, Constant{"REPL_AOF", 1},
                            Constant{"REPL_SLAVE", 2}, Constant{"REPL_REPLICA", 2},
                            Constant{"REPL_ALL", 3}}) {
    lua_pushinteger(L, c.value);
    lua_setfield(L, redis, c.name);
  }

  lua_pushvalue(L, redis);
  lua_setglobal(L, "redis");
  lua_setglobal(L, "server");  // Redis 7.4 alias

  // Freeze the base globals; per-script threads get their own writable globals.
  luaL_sandbox(L);
}

bool LuauEngine::compile_ok(std::string_view body, std::string& out) {
  ensure_vm();
  if (L_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return false;
  }

  std::size_t bytecode_size = 0;
  char* bytecode = luau_compile(body.data(), body.size(), nullptr, &bytecode_size);
  if (bytecode == nullptr) {
    resp::append_error(out, "ERR could not compile script");
    return false;
  }
  std::string owned(bytecode, bytecode_size);
  std::free(bytecode);

  // luau_load rejects an error blob (compilation failure is encoded in bytecode),
  // so this both validates syntax and reports the message.
  if (luau_load(L_, "@user_script", owned.data(), owned.size(), 0) != 0) {
    std::size_t len = 0;
    const char* msg = lua_tolstring(L_, -1, &len);
    std::string reply = "ERR Error compiling script (new function): ";
    reply += sanitize_line(msg != nullptr ? std::string_view(msg, len)
                                          : std::string_view("unknown error"));
    resp::append_error(out, reply);
    lua_pop(L_, 1);
    return false;
  }
  lua_pop(L_, 1);  // discard the validated function; we cache the bytecode

  scripts_[sha1_hex(body)] = std::move(owned);
  return true;
}

void LuauEngine::run(std::string_view bytecode,
                     std::span<const std::string_view> keys,
                     std::span<const std::string_view> argv,
                     std::string& out) {
  lua_State* GL = L_;
  lua_State* T = lua_newthread(GL);  // fresh thread, pushed onto GL
  luaL_sandboxthread(T);
  // Dynamic KEYS/ARGV globals: turn off the safe-env import fast path so global
  // reads resolve against the live table rather than a load-time snapshot.
  lua_setsafeenv(T, LUA_GLOBALSINDEX, false);
  set_global_string_table(T, "KEYS", keys);
  set_global_string_table(T, "ARGV", argv);

  if (luau_load(T, "@user_script", bytecode.data(), bytecode.size(), 0) != 0) {
    std::size_t len = 0;
    const char* s = lua_tolstring(T, -1, &len);
    resp::append_error(out, ensure_error_code(sanitize_line(
                                s != nullptr ? std::string_view(s, len)
                                             : std::string_view("compile error"))));
    lua_pop(GL, 1);
    return;
  }

  if (lua_pcall(T, 0, 1, 0) != 0) {
    if (lua_type(T, -1) == LUA_TTABLE) {
      lua_getfield(T, -1, "err");
      std::size_t len = 0;
      const char* s = lua_tolstring(T, -1, &len);
      std::string message = (s != nullptr) ? std::string(s, len) : "script error";
      lua_pop(T, 1);
      resp::append_error(out, sanitize_line(message));
    } else {
      std::size_t len = 0;
      const char* s = lua_tolstring(T, -1, &len);
      std::string message = (s != nullptr) ? std::string(s, len) : "script error";
      resp::append_error(out, ensure_error_code(sanitize_line(message)));
    }
    lua_pop(GL, 1);
    return;
  }

  lua_value_to_resp(T, -1, out);
  lua_pop(GL, 1);  // pop the thread; GC reclaims it
}

void LuauEngine::eval(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'luau.eval' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string_view body = args[0];
  if (!compile_ok(body, out)) return;  // caches bytecode under SHA1(body)
  run(scripts_[sha1_hex(body)], keys, argv, out);
}

void LuauEngine::eval_sha(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'luau.evalsha' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  ensure_vm();
  if (L_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return;
  }

  const std::string sha = to_lower(args[0]);
  const auto it = scripts_.find(sha);
  if (it == scripts_.end()) {
    resp::append_error(out, "NOSCRIPT No matching script. Please use LUAU.EVAL.");
    return;
  }
  const std::string bytecode = it->second;  // copy: run may mutate the cache map
  run(bytecode, keys, argv, out);
}

void LuauEngine::script(std::span<const std::string_view> args, std::string& out) {
  if (args.empty()) {
    resp::append_error(out, "ERR wrong number of arguments for 'luau.script' command");
    return;
  }

  const std::string_view sub = args[0];
  if (equals_upper(sub, "LOAD")) {
    if (args.size() != 2) {
      resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
      return;
    }
    if (!compile_ok(args[1], out)) return;
    resp::append_bulk_string(out, sha1_hex(args[1]));
    return;
  }

  if (equals_upper(sub, "EXISTS")) {
    resp::append_array_header(out, args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
      resp::append_integer(out, scripts_.count(to_lower(args[i])) != 0 ? 1 : 0);
    }
    return;
  }

  if (equals_upper(sub, "FLUSH")) {
    scripts_.clear();
    resp::append_simple_string(out, "OK");
    return;
  }

  resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
}

}  // namespace goblin::core
