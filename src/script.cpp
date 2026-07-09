#include "goblin/core/script.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

// The vendored Lua runtime is C. Include it with C linkage; the helper libraries
// expose their openers with C linkage too. Lua reports errors by longjmp (Lua is
// built as C), so every trampoline below keeps its working state in ScriptEngine
// members -- never in stack locals with non-trivial destructors -- so a longjmp
// out of a Lua API call cannot skip C++ cleanup.
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

int luaopen_cjson(lua_State* L);
int luaopen_cmsgpack(lua_State* L);
int luaopen_struct(lua_State* L);
int luaopen_bit(lua_State* L);
}

namespace goblin::core {

// The two trampolines that need the engine's private members reach them through
// this friend. Its static methods are used directly as lua_CFunction pointers
// (the Itanium ABI makes a C++ free function interchangeable with a C one, which
// is how every C++/Lua binding registers callbacks).
struct LuaBridge {
  [[nodiscard]] static ScriptEngine* engine(lua_State* L) {
    return static_cast<ScriptEngine*>(lua_touserdata(L, lua_upvalueindex(1)));
  }
  static int redis_call(lua_State* L) { return engine(L)->redis_call_impl(L, true); }
  static int redis_pcall(lua_State* L) { return engine(L)->redis_call_impl(L, false); }
  static int eval_runner(lua_State* L) { return engine(L)->eval_runner(L); }
};

namespace {

// ---------------------------------------------------------------------------
// SHA1 (FIPS 180-1 / RFC 3174). Needed for SCRIPT LOAD, EVALSHA lookup, and
// redis.sha1hex. Implemented from the public standard.
// ---------------------------------------------------------------------------
class Sha1 {
 public:
  Sha1() { reset(); }

  void update(const unsigned char* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
      block_[block_len_++] = data[i];
      if (block_len_ == 64) {
        transform();
        length_bits_ += 512;
        block_len_ = 0;
      }
    }
  }

  // Writes 40 lowercase hex characters to `out`.
  void finalize_hex(char* out) {
    const std::uint64_t total_bits =
        length_bits_ + static_cast<std::uint64_t>(block_len_) * 8;
    block_[block_len_++] = 0x80;
    if (block_len_ > 56) {
      while (block_len_ < 64) block_[block_len_++] = 0;
      transform();
      block_len_ = 0;
    }
    while (block_len_ < 56) block_[block_len_++] = 0;
    for (int i = 7; i >= 0; --i) {
      block_[block_len_++] =
          static_cast<unsigned char>((total_bits >> (i * 8)) & 0xff);
    }
    transform();

    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 5; ++i) {
      for (int j = 7; j >= 0; --j) {
        *out++ = kHex[(state_[i] >> (j * 4)) & 0xf];
      }
    }
  }

 private:
  void reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xEFCDAB89;
    state_[2] = 0x98BADCFE;
    state_[3] = 0x10325476;
    state_[4] = 0xC3D2E1F0;
    block_len_ = 0;
    length_bits_ = 0;
  }

  static std::uint32_t rol(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
  }

  void transform() {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block_[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block_[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block_[i * 4 + 2]) << 8) |
             (static_cast<std::uint32_t>(block_[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2];
    std::uint32_t d = state_[3], e = state_[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f;
      std::uint32_t k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
  }

  std::uint32_t state_[5];
  std::uint64_t length_bits_;
  unsigned char block_[64];
  std::size_t block_len_;
};

void sha1_hex_into(std::string_view data, char* out40) {
  Sha1 sha;
  sha.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
  sha.finalize_hex(out40);
}

[[nodiscard]] std::string sha1_hex(std::string_view data) {
  std::string result(40, '\0');
  sha1_hex_into(data, result.data());
  return result;
}

[[nodiscard]] char lower_char(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

[[nodiscard]] std::string to_lower(std::string_view text) {
  std::string result(text);
  for (char& c : result) c = lower_char(c);
  return result;
}

[[nodiscard]] bool equals_upper(std::string_view text, std::string_view upper) {
  if (text.size() != upper.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const char u = (c >= 'a' && c <= 'z') ? static_cast<char>(c - ('a' - 'A')) : c;
    if (u != upper[i]) return false;
  }
  return true;
}

// Redis blocks a small set of commands from inside a script. Our command surface
// only stubs the scripting/transaction commands, but rejecting them up front
// gives the documented error and prevents EVAL recursion.
[[nodiscard]] bool command_blocked_in_script(std::string_view name) {
  return equals_upper(name, "EVAL") || equals_upper(name, "EVALSHA") ||
         equals_upper(name, "SCRIPT") || equals_upper(name, "MULTI") ||
         equals_upper(name, "EXEC") || equals_upper(name, "WATCH") ||
         equals_upper(name, "SUBSCRIBE");
}

// Strip CRLF so a would-be RESP error/status line keeps the framing intact.
[[nodiscard]] std::string sanitize_line(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (char c : text) result.push_back((c == '\r' || c == '\n') ? ' ' : c);
  return result;
}

// A bare Lua error ("user_script:3: ...") gets an "ERR " code; a message that
// already opens with an ALL-CAPS token (a propagated "WRONGTYPE ...") is left
// alone. Keeps script error replies shaped like every other RESP error.
[[nodiscard]] std::string ensure_error_code(std::string_view message) {
  if (message.empty()) return "ERR script error";
  std::size_t token_end = 0;
  while (token_end < message.size() && message[token_end] != ' ') ++token_end;
  bool all_upper = token_end > 0;
  for (std::size_t i = 0; i < token_end; ++i) {
    if (message[i] < 'A' || message[i] > 'Z') {
      all_upper = false;
      break;
    }
  }
  if (all_upper) return std::string(message);
  return "ERR " + std::string(message);
}

[[nodiscard]] int abs_index(lua_State* L, int index) {
  if (index > 0 || index <= LUA_REGISTRYINDEX) return index;
  return lua_gettop(L) + index + 1;
}

// ---------------------------------------------------------------------------
// RESP reply -> Lua value (what redis.call / redis.pcall hand back to a script):
//   integer      -> number
//   bulk string  -> string           nil bulk  -> false
//   array        -> table (1-based)  nil array -> false
//   status (+)   -> { ok = "..." }
//   error  (-)   -> { err = "..." }  (redis.call additionally raises it)
// The buffer holds exactly one reply. Only Lua-stack and caller-owned memory is
// touched, so a longjmp here (OOM) unwinds cleanly to the enclosing lua_pcall.
// ---------------------------------------------------------------------------
[[nodiscard]] const char* read_line(const char* p, const char* end,
                                    std::string_view* line) {
  const char* start = p;
  while (p < end && *p != '\r') ++p;
  *line = std::string_view(start, static_cast<std::size_t>(p - start));
  if (p + 1 < end && p[0] == '\r' && p[1] == '\n') return p + 2;
  return end;  // malformed; stop
}

[[nodiscard]] long long parse_signed(std::string_view text) {
  long long value = 0;
  std::from_chars(text.data(), text.data() + text.size(), value);
  return value;
}

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
    case '+': {  // simple string -> { ok = line }
      p = read_line(p, end, &line);
      lua_createtable(L, 0, 1);
      lua_pushlstring(L, line.data(), line.size());
      lua_setfield(L, -2, "ok");
      break;
    }
    case '-': {  // error -> { err = line }
      p = read_line(p, end, &line);
      lua_createtable(L, 0, 1);
      lua_pushlstring(L, line.data(), line.size());
      lua_setfield(L, -2, "err");
      if (is_error != nullptr) *is_error = true;
      break;
    }
    case ':': {  // integer -> number
      p = read_line(p, end, &line);
      lua_pushinteger(L, static_cast<lua_Integer>(parse_signed(line)));
      break;
    }
    case '$': {  // bulk string
      p = read_line(p, end, &line);
      const long long len = parse_signed(line);
      if (len < 0) {
        lua_pushboolean(L, 0);  // nil bulk -> false
      } else {
        const char* payload = p;
        p += len;
        lua_pushlstring(L, payload, static_cast<std::size_t>(len));
        if (p + 2 <= end) p += 2;  // trailing CRLF
      }
      break;
    }
    case '*': {  // array
      p = read_line(p, end, &line);
      const long long count = parse_signed(line);
      if (count < 0) {
        lua_pushboolean(L, 0);  // nil array -> false
      } else {
        lua_createtable(L, static_cast<int>(count > 0 ? count : 0), 0);
        for (long long i = 0; i < count; ++i) {
          resp_reply_to_lua(L, &p, end, nullptr);  // inner errors do not raise
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

// ---------------------------------------------------------------------------
// Lua value -> RESP reply (a script's return value):
//   number -> integer (truncated toward zero)   string -> bulk string
//   nil / false -> nil bulk                      true   -> integer 1
//   table with `err` (string) -> error reply
//   table with `ok`  (string) -> status reply
//   other table -> array, stopping at the first nil element
// Writes straight into `out`; only Lua-stack reads happen (numbers are inspected
// with lua_tonumber, never converted in place), so this is safe to run inside
// the protected eval runner.
// ---------------------------------------------------------------------------
void lua_value_to_resp(lua_State* L, int index, std::string& out) {
  luaL_checkstack(L, 4, "resp return too deep");
  index = abs_index(L, index);
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
      resp::append_null_bulk_string(out);  // function/userdata/thread -> nil
      return;
  }
}

// Bind a 1-based table of strings to a global, bypassing the global-write guard
// (rawset), used for KEYS / ARGV on every run.
void set_global_string_table(lua_State* L, const char* name,
                             std::span<const std::string_view> items) {
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_pushstring(L, name);
  lua_createtable(L, static_cast<int>(items.size()), 0);
  for (std::size_t i = 0; i < items.size(); ++i) {
    lua_pushlstring(L, items[i].data(), items[i].size());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

// --- pure trampolines (no engine state needed) -----------------------------

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
  if (n < 2) return luaL_error(L, "redis.log() requires two arguments or more.");
  if (!lua_isnumber(L, 1)) {
    return luaL_error(L, "First argument must be a number (log level).");
  }
  // Force every message argument to string form first (this may convert numbers
  // in place, which can longjmp) BEFORE building any C++ object, so no C++
  // destructor is ever live across that conversion.
  for (int i = 2; i <= n; ++i) {
    std::size_t len = 0;
    (void)lua_tolstring(L, i, &len);
  }
  const int level = static_cast<int>(lua_tointeger(L, 1));
  std::string message;
  for (int i = 2; i <= n; ++i) {
    std::size_t len = 0;
    const char* s = lua_tolstring(L, i, &len);
    if (s == nullptr) continue;
    if (i > 2) message.push_back(' ');
    message.append(s, len);
  }
  std::cerr << "goblin-core script (level " << level << "): " << message << '\n';
  return 0;
}

int l_redis_setresp(lua_State* L) {
  const lua_Integer resp = luaL_checkinteger(L, 1);
  if (resp != 2 && resp != 3) {
    return luaL_error(L, "RESP version must be 2 or 3.");
  }
  // The server speaks RESP2 on the wire; accepting 3 is a documented no-op.
  return 0;
}

int l_redis_replicate_commands(lua_State* L) {
  // We do not replicate; report effects-replication as enabled (Redis returns
  // true) so scripts that call this keep working.
  lua_pushboolean(L, 1);
  return 1;
}

int l_redis_set_repl(lua_State* L) {
  (void)luaL_checkinteger(L, 1);  // validate; replication is a no-op here
  return 0;
}

int l_redis_noop_false(lua_State* L) {
  lua_pushboolean(L, 0);  // breakpoint()/debug(): no debugger attached
  return 1;
}

int l_deny_global_write(lua_State* L) {
  const char* name = lua_tostring(L, 2);
  return luaL_error(L, "Script attempted to create global variable '%s'",
                    name != nullptr ? name : "?");
}

// Open one library through the 5.1 opener protocol (func + name on the stack).
void open_library(lua_State* L, lua_CFunction opener, const char* name) {
  lua_pushcfunction(L, opener);
  lua_pushstring(L, name);
  lua_call(L, 1, 0);
}

// Open a helper module whose opener returns its table, and bind it as a global.
void open_module(lua_State* L, lua_CFunction opener, const char* name) {
  lua_pushcfunction(L, opener);
  lua_call(L, 0, 1);
  lua_setglobal(L, name);
}

void remove_global(lua_State* L, const char* name) {
  lua_pushnil(L);
  lua_setglobal(L, name);
}

}  // namespace

// ---------------------------------------------------------------------------
// redis.call / redis.pcall
// ---------------------------------------------------------------------------
int ScriptEngine::redis_call_impl(lua_State* L, bool raise_on_error) {
  const int nargs = lua_gettop(L);
  if (nargs < 1) {
    lua_pushstring(L, "Please specify at least one argument for this redis lib call");
    return lua_error(L);
  }

  // Every argument must be a string or a number. lua_tolstring converts a number
  // in place to its string form; the pointer then stays valid while the value
  // remains on the stack (we never pop the arguments), so the string_views we
  // stash below are stable for the whole command.
  call_args_.clear();
  call_args_.reserve(static_cast<std::size_t>(nargs));
  for (int i = 1; i <= nargs; ++i) {
    const int type = lua_type(L, i);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
      lua_pushstring(
          L, "Lua redis lib command arguments must be strings or integers");
      return lua_error(L);
    }
  }
  for (int i = 1; i <= nargs; ++i) {
    std::size_t len = 0;
    const char* s = lua_tolstring(L, i, &len);
    call_args_.emplace_back(s, len);
  }

  if (command_blocked_in_script(call_args_[0])) {
    lua_pushstring(L, "This Redis command is not allowed from script");
    return lua_error(L);
  }

  // Execute through the normal dispatch pipeline with no script engine in the
  // options, so a nested EVAL is impossible. The reply lands in an engine-owned
  // buffer, then we translate it to a Lua value.
  call_reply_.clear();
  handle_command_into(store_, call_args_, call_reply_, CommandExecutionOptions{});

  bool is_error = false;
  const char* p = call_reply_.data();
  const char* end = p + call_reply_.size();
  resp_reply_to_lua(L, &p, end, &is_error);

  if (is_error && raise_on_error) {
    // The { err = ... } table is on top of the stack; make it the error object.
    // No C++ destructors are live in this frame, so the longjmp is safe.
    return lua_error(L);
  }
  return 1;
}

// The protected body of a single EVAL: bind KEYS/ARGV, run the compiled chunk
// (its single argument), and translate the return value to RESP. Any error --
// from the chunk, from an uncaught redis.call, or from OOM during conversion --
// propagates to the lua_pcall in run().
int ScriptEngine::eval_runner(lua_State* L) {
  set_global_string_table(L, "KEYS", run_keys_);
  set_global_string_table(L, "ARGV", run_argv_);
  lua_call(L, 0, 1);  // the chunk sits at the bottom of the stack
  lua_value_to_resp(L, -1, reply_scratch_);
  return 0;
}

ScriptEngine::ScriptEngine(Store& store) : store_(store) {}

ScriptEngine::~ScriptEngine() {
  if (L_ != nullptr) {
    lua_close(L_);
    L_ = nullptr;
  }
}

void ScriptEngine::ensure_vm() {
  if (L_ != nullptr) return;

  L_ = luaL_newstate();
  if (L_ == nullptr) return;  // OOM; callers surface an error when L_ stays null
  lua_State* L = L_;

  // A curated, sandboxed standard library: no io/package/require, and os is
  // opened but stripped of anything that touches the host.
  open_library(L, luaopen_base, "");
  open_library(L, luaopen_table, LUA_TABLIBNAME);
  open_library(L, luaopen_string, LUA_STRLIBNAME);
  open_library(L, luaopen_math, LUA_MATHLIBNAME);
  open_library(L, luaopen_debug, LUA_DBLIBNAME);
  open_library(L, luaopen_os, LUA_OSLIBNAME);

  lua_getglobal(L, "os");
  for (const char* field :
       {"execute", "exit", "getenv", "remove", "rename", "setlocale", "tmpname"}) {
    lua_pushnil(L);
    lua_setfield(L, -2, field);
  }
  lua_pop(L, 1);

  // Drop the base-library escapes: no dynamic loading, no filesystem, no stdout.
  for (const char* name : {"dofile", "loadfile", "load", "loadstring", "print"}) {
    remove_global(L, name);
  }

  // The scripting helper modules, each from its own public upstream.
  open_module(L, luaopen_cjson, "cjson");
  open_module(L, luaopen_cmsgpack, "cmsgpack");
  open_module(L, luaopen_struct, "struct");
  open_module(L, luaopen_bit, "bit");

  // The redis table (aliased as `server`, matching current Redis).
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &LuaBridge::redis_call, 1);
  lua_setfield(L, -2, "call");
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &LuaBridge::redis_pcall, 1);
  lua_setfield(L, -2, "pcall");
  lua_pushcfunction(L, l_redis_error_reply);
  lua_setfield(L, -2, "error_reply");
  lua_pushcfunction(L, l_redis_status_reply);
  lua_setfield(L, -2, "status_reply");
  lua_pushcfunction(L, l_redis_sha1hex);
  lua_setfield(L, -2, "sha1hex");
  lua_pushcfunction(L, l_redis_log);
  lua_setfield(L, -2, "log");
  lua_pushcfunction(L, l_redis_setresp);
  lua_setfield(L, -2, "setresp");
  lua_pushcfunction(L, l_redis_replicate_commands);
  lua_setfield(L, -2, "replicate_commands");
  lua_pushcfunction(L, l_redis_set_repl);
  lua_setfield(L, -2, "set_repl");
  lua_pushcfunction(L, l_redis_noop_false);
  lua_setfield(L, -2, "breakpoint");
  lua_pushcfunction(L, l_redis_noop_false);
  lua_setfield(L, -2, "debug");

  struct Constant {
    const char* name;
    lua_Integer value;
  };
  for (const Constant& c : {Constant{"LOG_DEBUG", 0}, Constant{"LOG_VERBOSE", 1},
                            Constant{"LOG_NOTICE", 2}, Constant{"LOG_WARNING", 3},
                            Constant{"REPL_NONE", 0}, Constant{"REPL_AOF", 1},
                            Constant{"REPL_SLAVE", 2}, Constant{"REPL_REPLICA", 2},
                            Constant{"REPL_ALL", 3}}) {
    lua_pushinteger(L, c.value);
    lua_setfield(L, -2, c.name);
  }

  lua_pushvalue(L, -1);
  lua_setglobal(L, "redis");
  lua_setglobal(L, "server");

  // Deny accidental global creation (matches the EVAL sandbox). Installed last,
  // after every legitimate global is in place; KEYS/ARGV are bound with rawset.
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_newtable(L);
  lua_pushcfunction(L, l_deny_global_write);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
}

void ScriptEngine::run(std::string_view body,
                       std::span<const std::string_view> keys,
                       std::span<const std::string_view> argv,
                       std::string& out) {
  ensure_vm();
  lua_State* L = L_;
  if (L == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return;
  }

  const int base = lua_gettop(L);
  if (luaL_loadbuffer(L, body.data(), body.size(), "@user_script") != 0) {
    std::size_t len = 0;
    const char* msg = lua_tolstring(L, -1, &len);
    std::string reply = "ERR Error compiling script (new function): ";
    reply += sanitize_line(msg != nullptr ? std::string_view(msg, len)
                                          : std::string_view("unknown error"));
    resp::append_error(out, reply);
    lua_settop(L, base);
    return;
  }

  run_keys_ = keys;
  run_argv_ = argv;
  reply_scratch_.clear();

  // Wrap the chunk in the protected runner: [chunk] -> [runner][chunk].
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &LuaBridge::eval_runner, 1);
  lua_insert(L, -2);

  if (lua_pcall(L, 1, 0, 0) != 0) {
    std::string message;
    if (lua_type(L, -1) == LUA_TTABLE) {
      lua_getfield(L, -1, "err");
      std::size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      message = (s != nullptr) ? std::string(s, len) : "script error";
      lua_pop(L, 1);
      resp::append_error(out, sanitize_line(message));
    } else {
      std::size_t len = 0;
      const char* s = lua_tolstring(L, -1, &len);
      message = (s != nullptr) ? std::string(s, len) : "script error";
      resp::append_error(out, ensure_error_code(sanitize_line(message)));
    }
    lua_settop(L, base);
    return;
  }

  out.append(reply_scratch_);
  lua_settop(L, base);
}

namespace {

// Shared EVAL/EVALSHA argument shape: body/sha, numkeys, then keys then args.
// On success fills `keys` and `argv`; on failure writes a RESP error and returns
// false.
[[nodiscard]] bool split_keys_and_args(std::span<const std::string_view> args,
                                       std::span<const std::string_view>* keys,
                                       std::span<const std::string_view>* argv,
                                       std::string& out) {
  long long numkeys = 0;
  const std::string_view numkeys_text = args[1];
  const auto [ptr, ec] = std::from_chars(
      numkeys_text.data(), numkeys_text.data() + numkeys_text.size(), numkeys);
  if (ec != std::errc{} || ptr != numkeys_text.data() + numkeys_text.size()) {
    resp::append_error(out, "ERR value is not an integer or out of range");
    return false;
  }
  if (numkeys < 0) {
    resp::append_error(out, "ERR Number of keys can't be negative");
    return false;
  }
  if (static_cast<std::size_t>(numkeys) > args.size() - 2) {
    resp::append_error(out, "ERR Number of keys can't be greater than number of args");
    return false;
  }
  *keys = args.subspan(2, static_cast<std::size_t>(numkeys));
  *argv = args.subspan(2 + static_cast<std::size_t>(numkeys));
  return true;
}

}  // namespace

void ScriptEngine::eval(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'eval' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string_view body = args[0];
  // EVAL caches the script by SHA1, so a later EVALSHA of the same body hits.
  scripts_.emplace(sha1_hex(body), std::string(body));
  run(body, keys, argv, out);
}

void ScriptEngine::eval_sha(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'evalsha' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string sha = to_lower(args[0]);
  const auto it = scripts_.find(sha);
  if (it == scripts_.end()) {
    resp::append_error(out, "NOSCRIPT No matching script. Please use EVAL.");
    return;
  }
  // Copy the body: run() may reuse engine buffers, and the map could rehash if a
  // nested path inserted (it cannot today, but the copy keeps the view stable).
  const std::string body = it->second;
  run(body, keys, argv, out);
}

void ScriptEngine::script(std::span<const std::string_view> args, std::string& out) {
  if (args.empty()) {
    resp::append_error(out, "ERR wrong number of arguments for 'script' command");
    return;
  }

  const std::string_view sub = args[0];
  if (equals_upper(sub, "LOAD")) {
    if (args.size() != 2) {
      resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
      return;
    }
    ensure_vm();
    if (L_ == nullptr) {
      resp::append_error(out, "ERR could not initialize scripting engine");
      return;
    }
    // Validate that the body compiles before caching, so SCRIPT LOAD rejects
    // syntactically broken scripts exactly like EVAL would.
    const std::string_view body = args[1];
    if (luaL_loadbuffer(L_, body.data(), body.size(), "@user_script") != 0) {
      std::size_t len = 0;
      const char* msg = lua_tolstring(L_, -1, &len);
      std::string reply = "ERR Error compiling script (new function): ";
      reply += sanitize_line(msg != nullptr ? std::string_view(msg, len)
                                            : std::string_view("unknown error"));
      resp::append_error(out, reply);
      lua_pop(L_, 1);
      return;
    }
    lua_pop(L_, 1);  // discard the compiled chunk; we cache the source
    const std::string sha = sha1_hex(body);
    scripts_.emplace(sha, std::string(body));
    resp::append_bulk_string(out, sha);
    return;
  }

  if (equals_upper(sub, "EXISTS")) {
    resp::append_array_header(out, args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
      const std::string sha = to_lower(args[i]);
      resp::append_integer(out, scripts_.count(sha) != 0 ? 1 : 0);
    }
    return;
  }

  if (equals_upper(sub, "FLUSH")) {
    // Optional ASYNC/SYNC modifier is accepted and ignored (we flush inline).
    scripts_.clear();
    resp::append_simple_string(out, "OK");
    return;
  }

  resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
}

}  // namespace goblin::core
