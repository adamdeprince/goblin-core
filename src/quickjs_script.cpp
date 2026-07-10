#include "goblin/core/quickjs_script.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/detail/script_shared.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// quickjs.h is extern "C"-guarded, so it can be included directly.
#include "quickjs.h"

namespace goblin::core {

using namespace script_shared;

namespace {

[[nodiscard]] QuickJsEngine* engine_of(JSContext* ctx) {
  return static_cast<QuickJsEngine*>(JS_GetContextOpaque(ctx));
}

// A JS string value as a std::string (UTF-8); empty when it is not a string or
// conversion fails.
[[nodiscard]] std::string js_to_string(JSContext* ctx, JSValueConst val) {
  std::size_t len = 0;
  const char* chars = JS_ToCStringLen(ctx, &len, val);
  if (chars == nullptr) {
    return {};
  }
  std::string result(chars, len);
  JS_FreeCString(ctx, chars);
  return result;
}

// Format a JS number as a redis.call argument: whole values as integers, else a
// round-trippable float.
[[nodiscard]] std::string format_call_number(double value) {
  char buf[32];
  if (std::isfinite(value) && value == std::floor(value) &&
      std::abs(value) < 9.2e18) {
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  } else {
    std::snprintf(buf, sizeof(buf), "%.17g", value);
  }
  return std::string(buf);
}

// Wrap the script body in an immediately-invoked function so `return` yields the
// reply and the body's top-level declarations stay script-local (the shared
// context keeps no state between scripts). KEYS / ARGV / redis are globals it
// closes over.
[[nodiscard]] std::string wrap_body(std::string_view body) {
  std::string wrapped = "(function(){\n";
  wrapped.append(body);
  wrapped += "\n})()";
  return wrapped;
}

[[nodiscard]] std::string_view resp_error_message(std::string_view reply) {
  std::string_view body = reply.substr(1);
  const auto crlf = body.find('\r');
  if (crlf != std::string_view::npos) {
    body = body.substr(0, crlf);
  }
  return body;
}

// One RESP reply -> a JS value (recursive); *pp advances past what it consumed.
[[nodiscard]] JSValue resp_to_js(JSContext* ctx, const char** pp,
                                 const char* end) {
  const char* p = *pp;
  if (p >= end) {
    *pp = end;
    return JS_NULL;
  }
  const char type = *p++;
  std::string_view line;
  switch (type) {
    case '+': {  // simple string -> { ok: <text> }
      p = read_line(p, end, &line);
      JSValue obj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, obj, "ok",
                        JS_NewStringLen(ctx, line.data(), line.size()));
      *pp = p;
      return obj;
    }
    case '-': {  // error -> { err: <text> }
      p = read_line(p, end, &line);
      JSValue obj = JS_NewObject(ctx);
      JS_SetPropertyStr(ctx, obj, "err",
                        JS_NewStringLen(ctx, line.data(), line.size()));
      *pp = p;
      return obj;
    }
    case ':': {
      p = read_line(p, end, &line);
      *pp = p;
      return JS_NewInt64(ctx, parse_signed(line));
    }
    case '$': {
      p = read_line(p, end, &line);
      const long long len = parse_signed(line);
      if (len < 0) {
        *pp = p;
        return JS_NULL;
      }
      JSValue s = JS_NewStringLen(ctx, p, static_cast<std::size_t>(len));
      p += len;
      if (p + 2 <= end) {
        p += 2;
      }
      *pp = p;
      return s;
    }
    case '*': {
      p = read_line(p, end, &line);
      const long long count = parse_signed(line);
      if (count < 0) {
        *pp = p;
        return JS_NULL;
      }
      JSValue arr = JS_NewArray(ctx);
      for (long long i = 0; i < count; ++i) {
        JSValue elem = resp_to_js(ctx, &p, end);
        JS_SetPropertyUint32(ctx, arr, static_cast<std::uint32_t>(i), elem);
      }
      *pp = p;
      return arr;
    }
    default:
      *pp = end;
      return JS_NULL;
  }
}

// A JS value -> RESP reply (recursive).
void js_value_to_resp(JSContext* ctx, JSValueConst val, std::string& out) {
  if (JS_IsNull(val) || JS_IsUndefined(val)) {
    resp::append_null_bulk_string(out);
    return;
  }
  if (JS_IsBool(val)) {
    if (JS_ToBool(ctx, val)) {
      resp::append_integer(out, 1);
    } else {
      resp::append_null_bulk_string(out);  // false -> nil (as in Lua)
    }
    return;
  }
  if (JS_IsNumber(val)) {
    double number = 0;
    JS_ToFloat64(ctx, &number, val);
    if (std::isfinite(number) && number == std::floor(number) &&
        std::abs(number) < 9.2e18) {
      resp::append_integer(out, static_cast<long long>(number));
    } else {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g", number);
      resp::append_bulk_string(out, buf);
    }
    return;
  }
  if (JS_IsString(val)) {
    resp::append_bulk_string(out, js_to_string(ctx, val));
    return;
  }
  if (JS_IsArray(val)) {
    JSValue length = JS_GetPropertyStr(ctx, val, "length");
    std::int64_t count = 0;
    JS_ToInt64(ctx, &count, length);
    JS_FreeValue(ctx, length);
    if (count < 0) {
      count = 0;
    }
    resp::append_array_header(out, static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
      JSValue elem =
          JS_GetPropertyUint32(ctx, val, static_cast<std::uint32_t>(i));
      js_value_to_resp(ctx, elem, out);
      JS_FreeValue(ctx, elem);
    }
    return;
  }
  // An object with a string `err` -> error reply, `ok` -> status reply.
  JSValue err = JS_GetPropertyStr(ctx, val, "err");
  if (JS_IsString(err)) {
    resp::append_error(out, sanitize_line(js_to_string(ctx, err)));
    JS_FreeValue(ctx, err);
    return;
  }
  JS_FreeValue(ctx, err);
  JSValue ok = JS_GetPropertyStr(ctx, val, "ok");
  if (JS_IsString(ok)) {
    resp::append_simple_string(out, sanitize_line(js_to_string(ctx, ok)));
    JS_FreeValue(ctx, ok);
    return;
  }
  JS_FreeValue(ctx, ok);
  resp::append_null_bulk_string(out);  // any other object -> nil
}

// The pending exception as an error message: an Error's .message prefixed with
// its class name (e.g. "ReferenceError: x is not defined"), else the value's
// string form. A propagated redis.call error is thrown as a generic Error whose
// .message is the raw Redis message, so the "Error" class name is dropped to
// leave that text ("WRONGTYPE ...") untouched.
[[nodiscard]] std::string exception_message(JSContext* ctx) {
  JSValue exc = JS_GetException(ctx);
  std::string result;
  if (JS_IsObject(exc)) {
    JSValue message = JS_GetPropertyStr(ctx, exc, "message");
    if (JS_IsString(message)) {
      result = js_to_string(ctx, message);
    }
    JS_FreeValue(ctx, message);
    JSValue name = JS_GetPropertyStr(ctx, exc, "name");
    if (JS_IsString(name)) {
      const std::string cls = js_to_string(ctx, name);
      if (!cls.empty() && cls != "Error" && !result.empty()) {
        result = cls + ": " + result;
      }
    }
    JS_FreeValue(ctx, name);
  }
  if (result.empty()) {
    const char* chars = JS_ToCString(ctx, exc);
    if (chars != nullptr) {
      result = chars;
      JS_FreeCString(ctx, chars);
    }
  }
  JS_FreeValue(ctx, exc);
  return result.empty() ? "script error" : result;
}

// redis.call / redis.pcall shared body.
[[nodiscard]] JSValue redis_call_impl(JSContext* ctx, int argc,
                                      JSValueConst* argv, bool raise_on_error) {
  if (argc < 1) {
    return JS_ThrowTypeError(
        ctx, "Please specify at least one argument for a redis call");
  }
  std::vector<std::string> storage;
  storage.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    if (JS_IsString(argv[i])) {
      storage.push_back(js_to_string(ctx, argv[i]));
    } else if (JS_IsNumber(argv[i])) {
      double d = 0;
      JS_ToFloat64(ctx, &d, argv[i]);
      storage.push_back(format_call_number(d));
    } else {
      return JS_ThrowTypeError(
          ctx, "redis call arguments must be strings or numbers");
    }
  }
  std::vector<std::string_view> call_args;
  call_args.reserve(storage.size());
  for (const std::string& s : storage) {
    call_args.emplace_back(s);
  }
  if (command_blocked_in_script(call_args[0])) {
    return JS_ThrowTypeError(ctx,
                             "This Redis command is not allowed from script");
  }

  const std::string& reply = engine_of(ctx)->invoke(call_args);
  if (!reply.empty() && reply.front() == '-') {
    const std::string_view msg = resp_error_message(reply);
    if (raise_on_error) {
      JSValue error = JS_NewError(ctx);
      JS_SetPropertyStr(ctx, error, "message",
                        JS_NewStringLen(ctx, msg.data(), msg.size()));
      return JS_Throw(ctx, error);
    }
    JSValue obj = JS_NewObject(ctx);  // pcall: { err: <msg> }
    JS_SetPropertyStr(ctx, obj, "err",
                      JS_NewStringLen(ctx, msg.data(), msg.size()));
    return obj;
  }
  const char* p = reply.data();
  const char* end = p + reply.size();
  return resp_to_js(ctx, &p, end);
}

JSValue js_redis_call(JSContext* ctx, JSValueConst, int argc,
                      JSValueConst* argv) {
  return redis_call_impl(ctx, argc, argv, true);
}
JSValue js_redis_pcall(JSContext* ctx, JSValueConst, int argc,
                       JSValueConst* argv) {
  return redis_call_impl(ctx, argc, argv, false);
}

// redis.error(msg) / redis.status(msg) build the { err } / { ok } reply markers.
[[nodiscard]] JSValue reply_marker(JSContext* ctx, int argc, JSValueConst* argv,
                                   const char* field) {
  JSValue obj = JS_NewObject(ctx);
  JSValue text = (argc >= 1 && JS_IsString(argv[0])) ? JS_DupValue(ctx, argv[0])
                                                     : JS_NewString(ctx, "");
  JS_SetPropertyStr(ctx, obj, field, text);
  return obj;
}
JSValue js_redis_error(JSContext* ctx, JSValueConst, int argc,
                       JSValueConst* argv) {
  return reply_marker(ctx, argc, argv, "err");
}
JSValue js_redis_status(JSContext* ctx, JSValueConst, int argc,
                        JSValueConst* argv) {
  return reply_marker(ctx, argc, argv, "ok");
}

JSValue js_redis_sha1hex(JSContext* ctx, JSValueConst, int argc,
                         JSValueConst* argv) {
  if (argc < 1 || !JS_IsString(argv[0])) {
    return JS_ThrowTypeError(ctx, "redis.sha1hex expects a string");
  }
  char hex[40];
  sha1_hex_into(js_to_string(ctx, argv[0]), hex);
  return JS_NewStringLen(ctx, hex, sizeof(hex));
}

JSValue js_redis_log(JSContext* ctx, JSValueConst, int argc,
                     JSValueConst* argv) {
  int level = 0;
  if (argc >= 1) {
    std::int64_t value = 0;
    JS_ToInt64(ctx, &value, argv[0]);
    level = static_cast<int>(value);
  }
  std::string message;
  if (argc >= 2 && JS_IsString(argv[1])) {
    message = js_to_string(ctx, argv[1]);
  }
  std::fprintf(stderr, "goblin-core quickjs script (level %d): %s\n", level,
               message.c_str());
  return JS_UNDEFINED;
}

}  // namespace

const std::string& QuickJsEngine::invoke(
    std::span<const std::string_view> call_args) {
  call_reply_.clear();
  handle_command_into(store_, call_args, call_reply_, CommandExecutionOptions{});
  return call_reply_;
}

QuickJsEngine::QuickJsEngine(Store& store) : store_(store) {}

QuickJsEngine::~QuickJsEngine() {
  if (context_ != nullptr) {
    JS_FreeContext(context_);
    context_ = nullptr;
  }
  if (runtime_ != nullptr) {
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
  }
}

void QuickJsEngine::ensure_vm() {
  if (context_ != nullptr) {
    return;
  }
  runtime_ = JS_NewRuntime();
  if (runtime_ == nullptr) {
    return;
  }
  // Bound a runaway script's memory and native stack. The standard context has
  // no filesystem/os surface (quickjs-libc is not compiled in), so it is already
  // sandboxed to pure computation plus the redis binding below.
  JS_SetMemoryLimit(runtime_, std::size_t{64} << 20);
  JS_SetMaxStackSize(runtime_, std::size_t{256} << 10);
  context_ = JS_NewContext(runtime_);
  if (context_ == nullptr) {
    JS_FreeRuntime(runtime_);
    runtime_ = nullptr;
    return;
  }
  JS_SetContextOpaque(context_, this);

  JSValue global = JS_GetGlobalObject(context_);
  JSValue redis = JS_NewObject(context_);
  JS_SetPropertyStr(context_, redis, "call",
                    JS_NewCFunction(context_, js_redis_call, "call", 1));
  JS_SetPropertyStr(context_, redis, "pcall",
                    JS_NewCFunction(context_, js_redis_pcall, "pcall", 1));
  JS_SetPropertyStr(context_, redis, "error",
                    JS_NewCFunction(context_, js_redis_error, "error", 1));
  JS_SetPropertyStr(context_, redis, "status",
                    JS_NewCFunction(context_, js_redis_status, "status", 1));
  JS_SetPropertyStr(context_, redis, "sha1hex",
                    JS_NewCFunction(context_, js_redis_sha1hex, "sha1hex", 1));
  JS_SetPropertyStr(context_, redis, "log",
                    JS_NewCFunction(context_, js_redis_log, "log", 2));
  JS_SetPropertyStr(context_, global, "redis", redis);
  JS_FreeValue(context_, global);
}

bool QuickJsEngine::run(std::string_view body,
                        std::span<const std::string_view> keys,
                        std::span<const std::string_view> argv,
                        std::string& out) {
  ensure_vm();
  if (context_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return false;
  }
  current_keys_ = keys;
  current_argv_ = argv;

  // KEYS / ARGV as fresh 0-based global arrays for this run.
  JSValue global = JS_GetGlobalObject(context_);
  JSValue keys_array = JS_NewArray(context_);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    JS_SetPropertyUint32(
        context_, keys_array, static_cast<std::uint32_t>(i),
        JS_NewStringLen(context_, keys[i].data(), keys[i].size()));
  }
  JS_SetPropertyStr(context_, global, "KEYS", keys_array);
  JSValue argv_array = JS_NewArray(context_);
  for (std::size_t i = 0; i < argv.size(); ++i) {
    JS_SetPropertyUint32(
        context_, argv_array, static_cast<std::uint32_t>(i),
        JS_NewStringLen(context_, argv[i].data(), argv[i].size()));
  }
  JS_SetPropertyStr(context_, global, "ARGV", argv_array);
  JS_FreeValue(context_, global);

  const std::string wrapped = wrap_body(body);
  // Compile first so a syntax error is reported as a (non-cacheable) compile
  // error rather than a runtime error, then run the compiled function.
  JSValue compiled =
      JS_Eval(context_, wrapped.data(), wrapped.size(), "<quickjs>",
              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    JS_FreeValue(context_, compiled);
    resp::append_error(out, "ERR Error compiling script: " +
                                sanitize_line(exception_message(context_)));
    return false;
  }
  JSValue result = JS_EvalFunction(context_, compiled);  // consumes `compiled`
  if (JS_IsException(result)) {
    JS_FreeValue(context_, result);
    resp::append_error(
        out, ensure_error_code(sanitize_line(exception_message(context_))));
    return true;  // compiled but threw at runtime -- still cacheable
  }
  js_value_to_resp(context_, result, out);
  JS_FreeValue(context_, result);
  return true;
}

bool QuickJsEngine::compile_ok(std::string_view body, std::string& out) {
  ensure_vm();
  if (context_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return false;
  }
  const std::string wrapped = wrap_body(body);
  JSValue compiled =
      JS_Eval(context_, wrapped.data(), wrapped.size(), "<quickjs>",
              JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(compiled)) {
    JS_FreeValue(context_, compiled);
    resp::append_error(out, "ERR Error compiling script: " +
                                sanitize_line(exception_message(context_)));
    return false;
  }
  JS_FreeValue(context_, compiled);
  scripts_[sha1_hex(body)] = std::string(body);
  return true;
}

void QuickJsEngine::eval(std::span<const std::string_view> args,
                         std::string& out) {
  if (args.size() < 2) {
    resp::append_error(
        out, "ERR wrong number of arguments for 'quickjs.eval' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) {
    return;
  }
  const std::string_view body = args[0];
  if (run(body, keys, argv, out)) {
    scripts_.emplace(sha1_hex(body), std::string(body));
  }
}

void QuickJsEngine::eval_sha(std::span<const std::string_view> args,
                             std::string& out) {
  if (args.size() < 2) {
    resp::append_error(
        out, "ERR wrong number of arguments for 'quickjs.evalsha' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) {
    return;
  }
  const std::string sha = to_lower(args[0]);
  const auto it = scripts_.find(sha);
  if (it == scripts_.end()) {
    resp::append_error(out,
                       "NOSCRIPT No matching script. Please use QUICKJS.EVAL.");
    return;
  }
  const std::string body = it->second;
  (void)run(body, keys, argv, out);
}

void QuickJsEngine::script(std::span<const std::string_view> args,
                           std::string& out) {
  if (args.empty()) {
    resp::append_error(
        out, "ERR wrong number of arguments for 'quickjs.script' command");
    return;
  }
  const std::string_view sub = args[0];
  if (equals_upper(sub, "LOAD")) {
    if (args.size() != 2) {
      resp::append_error(
          out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
      return;
    }
    if (!compile_ok(args[1], out)) {
      return;
    }
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
  resp::append_error(
      out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
}

}  // namespace goblin::core
