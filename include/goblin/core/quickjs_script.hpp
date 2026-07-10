#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// QuickJS's runtime and context, forward-declared so consumers of this header
// (the server and the command layer) need not pull in quickjs.h. The real
// definitions are included only by quickjs_script.cpp.
struct JSRuntime;
struct JSContext;

namespace goblin::core {

class Store;

// The JavaScript counterpart to the other scripting engines, implementing
// QUICKJS.EVAL / QUICKJS.EVALSHA / QUICKJS.SCRIPT on an embedded QuickJS (the
// quickjs-ng fork). It shares only the key space with the Lua, Luau, Wren, Tcl,
// and MicroPython engines, and keeps its own script cache and VM.
//
// A script body runs inside a function, so `return <value>` produces the reply
// (converted to RESP) and the body's top-level declarations stay script-local
// -- nothing leaks between scripts through the shared context. redis.call(...)
// re-enters the shared command pipeline; a command error throws a JS exception
// (catchable with try/catch), while redis.pcall returns { err } instead. KEYS
// and ARGV are 0-based arrays. The runtime is created lazily on first use.
class QuickJsEngine {
 public:
  explicit QuickJsEngine(Store& store);
  ~QuickJsEngine();

  QuickJsEngine(const QuickJsEngine&) = delete;
  QuickJsEngine& operator=(const QuickJsEngine&) = delete;

  void eval(std::span<const std::string_view> args, std::string& out);
  void eval_sha(std::span<const std::string_view> args, std::string& out);
  void script(std::span<const std::string_view> args, std::string& out);

  // Reached by the redis.* C bindings, which recover the engine from the QuickJS
  // context's opaque pointer.
  [[nodiscard]] std::span<const std::string_view> keys() const noexcept {
    return current_keys_;
  }
  [[nodiscard]] std::span<const std::string_view> argv() const noexcept {
    return current_argv_;
  }
  // Re-enter the command pipeline with `call_args`, returning the RESP reply in a
  // reused internal buffer (valid only until the next invoke()).
  [[nodiscard]] const std::string& invoke(
      std::span<const std::string_view> call_args);

 private:
  void ensure_vm();
  bool run(std::string_view body, std::span<const std::string_view> keys,
           std::span<const std::string_view> argv, std::string& out);
  bool compile_ok(std::string_view body, std::string& out);

  Store& store_;
  JSRuntime* runtime_ = nullptr;
  JSContext* context_ = nullptr;
  std::unordered_map<std::string, std::string> scripts_;
  std::span<const std::string_view> current_keys_;
  std::span<const std::string_view> current_argv_;
  std::string call_reply_;
};

}  // namespace goblin::core
