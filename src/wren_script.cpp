#include "goblin/core/wren_script.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/detail/script_shared.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

// Wren is C and its header has no extern "C" guard, so wrap it. Foreign methods
// report errors via wrenAbortFiber, which does NOT longjmp -- it flags the fiber
// and control returns normally -- so the trampolines below can use ordinary C++
// locals with no unwinding hazard.
extern "C" {
#include "wren.h"
}

namespace goblin::core {

using namespace script_shared;

namespace {

// The preamble module: a Redis class exposing the host bindings. call/pcall take
// a List of arguments (Wren has no varargs); error/status build the {err}/{ok}
// reply markers in Wren.
const char* const kGoblinModule =
    "class Redis {\n"
    "  foreign static call(args)\n"
    "  foreign static pcall(args)\n"
    "  foreign static sha1hex(s)\n"
    "  foreign static log(level, message)\n"
    "  foreign static keys\n"
    "  foreign static argv\n"
    "  foreign static setReply_(value)\n"
    "  static error(msg) {\n"
    "    var m = {}\n"
    "    m[\"err\"] = msg\n"
    "    return m\n"
    "  }\n"
    "  static status(msg) {\n"
    "    var m = {}\n"
    "    m[\"ok\"] = msg\n"
    "    return m\n"
    "  }\n"
    "}\n";

[[nodiscard]] WrenEngine* engine_of(WrenVM* vm) {
  return static_cast<WrenEngine*>(wrenGetUserData(vm));
}

void tramp_call(WrenVM* vm) { engine_of(vm)->foreign_call(vm, true); }
void tramp_pcall(WrenVM* vm) { engine_of(vm)->foreign_call(vm, false); }
void tramp_sha1hex(WrenVM* vm) { engine_of(vm)->foreign_sha1hex(vm); }
void tramp_log(WrenVM* vm) { engine_of(vm)->foreign_log(vm); }
void tramp_set_reply(WrenVM* vm) { engine_of(vm)->foreign_set_reply(vm); }
void tramp_keys(WrenVM* vm) { engine_of(vm)->foreign_keys(vm); }
void tramp_argv(WrenVM* vm) { engine_of(vm)->foreign_argv(vm); }

WrenForeignMethodFn bind_foreign_method(WrenVM*, const char* module,
                                        const char* className, bool isStatic,
                                        const char* signature) {
  if (std::strcmp(module, "goblin") != 0 || std::strcmp(className, "Redis") != 0 ||
      !isStatic) {
    return nullptr;
  }
  if (std::strcmp(signature, "call(_)") == 0) return tramp_call;
  if (std::strcmp(signature, "pcall(_)") == 0) return tramp_pcall;
  if (std::strcmp(signature, "sha1hex(_)") == 0) return tramp_sha1hex;
  if (std::strcmp(signature, "log(_,_)") == 0) return tramp_log;
  if (std::strcmp(signature, "setReply_(_)") == 0) return tramp_set_reply;
  if (std::strcmp(signature, "keys") == 0) return tramp_keys;
  if (std::strcmp(signature, "argv") == 0) return tramp_argv;
  return nullptr;
}

void write_fn(WrenVM* vm, const char* text) { engine_of(vm)->note_write(text); }

void error_fn(WrenVM* vm, WrenErrorType type, const char* module, int line,
              const char* message) {
  if (message == nullptr) return;
  if (type == WREN_ERROR_COMPILE) {
    std::string m = "at line ";
    m += std::to_string(line);
    m += ": ";
    m += message;
    engine_of(vm)->report_error(std::move(m));
  } else if (type == WREN_ERROR_RUNTIME) {
    engine_of(vm)->report_error(message);
  }
  // WREN_ERROR_STACK_TRACE frames are ignored.
  (void)module;
}

// Format a Wren number the way redis.call args expect: whole values as integers,
// otherwise a compact float.
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

// Parse one RESP reply into Wren value(s) in `slot` (recursively, using slot+1..
// for scratch).
void resp_to_wren(WrenVM* vm, const char** pp, const char* end, int slot) {
  wrenEnsureSlots(vm, slot + 3);
  const char* p = *pp;
  if (p >= end) {
    wrenSetSlotNull(vm, slot);
    *pp = end;
    return;
  }
  const char type = *p++;
  std::string_view line;
  switch (type) {
    case '+':
      p = read_line(p, end, &line);
      wrenSetSlotNewMap(vm, slot);
      wrenSetSlotString(vm, slot + 1, "ok");
      wrenSetSlotBytes(vm, slot + 2, line.data(), line.size());
      wrenSetMapValue(vm, slot, slot + 1, slot + 2);
      break;
    case '-':
      p = read_line(p, end, &line);
      wrenSetSlotNewMap(vm, slot);
      wrenSetSlotString(vm, slot + 1, "err");
      wrenSetSlotBytes(vm, slot + 2, line.data(), line.size());
      wrenSetMapValue(vm, slot, slot + 1, slot + 2);
      break;
    case ':':
      p = read_line(p, end, &line);
      wrenSetSlotDouble(vm, slot, static_cast<double>(parse_signed(line)));
      break;
    case '$': {
      p = read_line(p, end, &line);
      const long long len = parse_signed(line);
      if (len < 0) {
        wrenSetSlotNull(vm, slot);
      } else {
        wrenSetSlotBytes(vm, slot, p, static_cast<std::size_t>(len));
        p += len;
        if (p + 2 <= end) p += 2;
      }
      break;
    }
    case '*': {
      p = read_line(p, end, &line);
      const long long count = parse_signed(line);
      if (count < 0) {
        wrenSetSlotNull(vm, slot);
      } else {
        wrenSetSlotNewList(vm, slot);
        for (long long i = 0; i < count; ++i) {
          resp_to_wren(vm, &p, end, slot + 1);
          wrenInsertInList(vm, slot, -1, slot + 1);
        }
      }
      break;
    }
    default:
      wrenSetSlotNull(vm, slot);
      p = end;
      break;
  }
  *pp = p;
}

// Convert the Wren value in `slot` to a RESP reply.
void wren_value_to_resp(WrenVM* vm, int slot, std::string& out) {
  wrenEnsureSlots(vm, slot + 4);
  switch (wrenGetSlotType(vm, slot)) {
    case WREN_TYPE_NULL:
      resp::append_null_bulk_string(out);
      return;
    case WREN_TYPE_BOOL:
      if (wrenGetSlotBool(vm, slot)) {
        resp::append_integer(out, 1);
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    case WREN_TYPE_NUM: {
      const double number = wrenGetSlotDouble(vm, slot);
      resp::append_integer(
          out, std::isfinite(number) ? static_cast<long long>(number) : 0);
      return;
    }
    case WREN_TYPE_STRING: {
      int len = 0;
      const char* b = wrenGetSlotBytes(vm, slot, &len);
      resp::append_bulk_string(out, std::string_view(b, static_cast<std::size_t>(len)));
      return;
    }
    case WREN_TYPE_LIST: {
      const int count = wrenGetListCount(vm, slot);
      resp::append_array_header(out, static_cast<std::size_t>(count));
      for (int i = 0; i < count; ++i) {
        wrenGetListElement(vm, slot, i, slot + 1);
        wren_value_to_resp(vm, slot + 1, out);
      }
      return;
    }
    case WREN_TYPE_MAP: {
      for (const char* field : {"err", "ok"}) {
        wrenSetSlotString(vm, slot + 1, field);
        if (wrenGetMapContainsKey(vm, slot, slot + 1)) {
          wrenGetMapValue(vm, slot, slot + 1, slot + 2);
          if (wrenGetSlotType(vm, slot + 2) == WREN_TYPE_STRING) {
            int len = 0;
            const char* b = wrenGetSlotBytes(vm, slot + 2, &len);
            const std::string_view text(b, static_cast<std::size_t>(len));
            if (field[0] == 'e') {
              resp::append_error(out, sanitize_line(text));
            } else {
              resp::append_simple_string(out, sanitize_line(text));
            }
            return;
          }
        }
      }
      resp::append_null_bulk_string(out);  // any other map -> nil
      return;
    }
    default:
      resp::append_null_bulk_string(out);
      return;
  }
}

// Extract the error line from a RESP error reply ("-<msg>\r\n").
[[nodiscard]] std::string_view resp_error_message(std::string_view reply) {
  std::string_view body = reply.substr(1);
  const auto crlf = body.find('\r');
  if (crlf != std::string_view::npos) body = body.substr(0, crlf);
  return body;
}

}  // namespace

void WrenEngine::foreign_call(WrenVM* vm, bool raise_on_error) {
  if (wrenGetSlotType(vm, 1) != WREN_TYPE_LIST) {
    wrenSetSlotString(vm, 0, "Redis.call/pcall expects a List of arguments");
    wrenAbortFiber(vm, 0);
    return;
  }
  const int n = wrenGetListCount(vm, 1);
  if (n < 1) {
    wrenSetSlotString(vm, 0, "Please specify at least one argument for a redis call");
    wrenAbortFiber(vm, 0);
    return;
  }

  wrenEnsureSlots(vm, 3);  // slot 2 = scratch for each element
  call_arg_storage_.clear();
  call_arg_storage_.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    wrenGetListElement(vm, 1, i, 2);
    const WrenType t = wrenGetSlotType(vm, 2);
    if (t == WREN_TYPE_STRING) {
      int len = 0;
      const char* b = wrenGetSlotBytes(vm, 2, &len);
      call_arg_storage_.emplace_back(b, static_cast<std::size_t>(len));
    } else if (t == WREN_TYPE_NUM) {
      call_arg_storage_.push_back(format_call_number(wrenGetSlotDouble(vm, 2)));
    } else {
      wrenSetSlotString(vm, 0, "redis call arguments must be strings or numbers");
      wrenAbortFiber(vm, 0);
      return;
    }
  }
  call_args_.clear();
  call_args_.reserve(call_arg_storage_.size());
  for (const std::string& s : call_arg_storage_) call_args_.emplace_back(s);

  if (command_blocked_in_script(call_args_[0])) {
    wrenSetSlotString(vm, 0, "This Redis command is not allowed from script");
    wrenAbortFiber(vm, 0);
    return;
  }

  call_reply_.clear();
  handle_command_into(store_, call_args_, call_reply_, CommandExecutionOptions{});

  if (!call_reply_.empty() && call_reply_.front() == '-') {
    const std::string_view msg = resp_error_message(call_reply_);
    if (raise_on_error) {
      wrenSetSlotBytes(vm, 0, msg.data(), msg.size());
      wrenAbortFiber(vm, 0);
      return;
    }
    // pcall: hand back { "err": msg } like resp_to_wren would.
    wrenEnsureSlots(vm, 3);
    wrenSetSlotNewMap(vm, 0);
    wrenSetSlotString(vm, 1, "err");
    wrenSetSlotBytes(vm, 2, msg.data(), msg.size());
    wrenSetMapValue(vm, 0, 1, 2);
    return;
  }

  const char* p = call_reply_.data();
  const char* end = p + call_reply_.size();
  resp_to_wren(vm, &p, end, 0);
}

void WrenEngine::foreign_sha1hex(WrenVM* vm) {
  if (wrenGetSlotType(vm, 1) != WREN_TYPE_STRING) {
    wrenSetSlotString(vm, 0, "Redis.sha1hex expects a String");
    wrenAbortFiber(vm, 0);
    return;
  }
  int len = 0;
  const char* b = wrenGetSlotBytes(vm, 1, &len);
  char hex[40];
  sha1_hex_into(std::string_view(b, static_cast<std::size_t>(len)), hex);
  wrenSetSlotBytes(vm, 0, hex, 40);
}

void WrenEngine::foreign_log(WrenVM* vm) {
  const int level =
      wrenGetSlotType(vm, 1) == WREN_TYPE_NUM
          ? static_cast<int>(wrenGetSlotDouble(vm, 1))
          : 0;
  std::string message;
  if (wrenGetSlotType(vm, 2) == WREN_TYPE_STRING) {
    int len = 0;
    const char* b = wrenGetSlotBytes(vm, 2, &len);
    message.assign(b, static_cast<std::size_t>(len));
  }
  std::fprintf(stderr, "goblin-core wren script (level %d): %s\n", level,
               message.c_str());
}

void WrenEngine::foreign_set_reply(WrenVM* vm) {
  reply_.clear();
  wren_value_to_resp(vm, 1, reply_);
  reply_set_ = true;
}

void WrenEngine::foreign_keys(WrenVM* vm) {
  wrenEnsureSlots(vm, 2);
  wrenSetSlotNewList(vm, 0);
  for (const std::string_view k : current_keys_) {
    wrenSetSlotBytes(vm, 1, k.data(), k.size());
    wrenInsertInList(vm, 0, -1, 1);
  }
}

void WrenEngine::foreign_argv(WrenVM* vm) {
  wrenEnsureSlots(vm, 2);
  wrenSetSlotNewList(vm, 0);
  for (const std::string_view a : current_argv_) {
    wrenSetSlotBytes(vm, 1, a.data(), a.size());
    wrenInsertInList(vm, 0, -1, 1);
  }
}

void WrenEngine::report_error(std::string message) {
  if (error_.empty()) error_ = std::move(message);
}

void WrenEngine::note_write(std::string_view text) {
  std::cerr << "goblin-core wren print: " << text;
}

WrenEngine::WrenEngine(Store& store) : store_(store) {}

WrenEngine::~WrenEngine() {
  if (vm_ != nullptr) {
    wrenFreeVM(vm_);
    vm_ = nullptr;
  }
}

void WrenEngine::ensure_vm() {
  if (vm_ != nullptr) return;

  WrenConfiguration config;
  wrenInitConfiguration(&config);
  config.bindForeignMethodFn = bind_foreign_method;
  config.writeFn = write_fn;
  config.errorFn = error_fn;
  config.userData = this;
  // Wren's default is 10 MB before the first GC; keep the scripting VM lean to
  // protect the project's memory profile.
  config.initialHeapSize = 1024 * 1024;
  config.minHeapSize = 256 * 1024;

  vm_ = wrenNewVM(&config);
  if (vm_ == nullptr) return;

  // Define the Redis binding module, then import it into "main" once so every
  // per-script interpret can reference it without re-declaring a top-level name.
  wrenInterpret(vm_, "goblin", kGoblinModule);
  wrenInterpret(vm_, "main", "import \"goblin\" for Redis\n");
}

bool WrenEngine::run(std::string_view body,
                     std::span<const std::string_view> keys,
                     std::span<const std::string_view> argv,
                     std::string& out) {
  ensure_vm();
  if (vm_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return false;
  }

  current_keys_ = keys;
  current_argv_ = argv;
  reply_.clear();
  reply_set_ = false;
  error_.clear();

  // Wren has no top-level return, so run the body as a function and capture its
  // result through Redis.setReply_. KEYS/ARGV are 0-based Lists (Wren indexing).
  std::string wrapped = "Redis.setReply_((Fn.new{\nvar KEYS = Redis.keys\nvar ARGV = Redis.argv\n";
  wrapped.append(body);
  wrapped += "\n}).call())\n";

  const WrenInterpretResult result = wrenInterpret(vm_, "main", wrapped.c_str());
  if (result == WREN_RESULT_SUCCESS) {
    if (reply_set_) {
      out.append(reply_);
    } else {
      resp::append_null_bulk_string(out);
    }
    return true;
  }

  const std::string message = error_.empty() ? "script error" : error_;
  if (result == WREN_RESULT_COMPILE_ERROR) {
    resp::append_error(out, "ERR Error compiling script: " + sanitize_line(message));
    return false;
  }
  resp::append_error(out, ensure_error_code(sanitize_line(message)));
  return true;  // compiled, but errored at runtime -- still cacheable
}

bool WrenEngine::compile_ok(std::string_view body, std::string& out) {
  ensure_vm();
  if (vm_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return false;
  }
  error_.clear();

  // Construct the wrapped function without calling it: this compiles the body
  // (syntax check, with KEYS/ARGV in scope) but runs no user code.
  std::string wrapped = "(Fn.new{\nvar KEYS = Redis.keys\nvar ARGV = Redis.argv\n";
  wrapped.append(body);
  wrapped += "\n})\n";

  const WrenInterpretResult result = wrenInterpret(vm_, "main", wrapped.c_str());
  if (result == WREN_RESULT_COMPILE_ERROR) {
    const std::string message = error_.empty() ? "compile error" : error_;
    resp::append_error(out, "ERR Error compiling script: " + sanitize_line(message));
    return false;
  }
  scripts_[sha1_hex(body)] = std::string(body);
  return true;
}

void WrenEngine::eval(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'wren.eval' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string_view body = args[0];
  if (run(body, keys, argv, out)) {
    scripts_.emplace(sha1_hex(body), std::string(body));
  }
}

void WrenEngine::eval_sha(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'wren.evalsha' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string sha = to_lower(args[0]);
  const auto it = scripts_.find(sha);
  if (it == scripts_.end()) {
    resp::append_error(out, "NOSCRIPT No matching script. Please use WREN.EVAL.");
    return;
  }
  const std::string body = it->second;
  (void)run(body, keys, argv, out);
}

void WrenEngine::script(std::span<const std::string_view> args, std::string& out) {
  if (args.empty()) {
    resp::append_error(out, "ERR wrong number of arguments for 'wren.script' command");
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
