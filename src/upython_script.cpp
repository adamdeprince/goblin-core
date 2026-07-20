#include "goblin/core/upython_script.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/detail/script_shared.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// MicroPython is C; include its API with C linkage. Errors travel by nlr
// (setjmp/longjmp), so the redis module's C functions below keep their working
// state on the engine (reached via the file-scope pointer) rather than in C++
// stack locals, so a longjmp cannot skip C++ cleanup.
extern "C" {
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/objmodule.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
}

namespace goblin::core {

using namespace script_shared;

namespace {

// Set for the duration of a run (single-threaded, non-reentrant): the redis
// module's C functions recover the engine from here.
UPythonEngine* s_engine = nullptr;

[[nodiscard]] mp_obj_t new_str(std::string_view s) {
  return mp_obj_new_str(s.data(), s.size());
}

[[nodiscard]] std::string_view str_view(mp_obj_t o) {
  size_t len = 0;
  const char* d = mp_obj_str_get_data(o, &len);
  return std::string_view(d, len);
}

// Capture the printed form of any object (str(o), or a traceback) into a string.
struct PrintCapture {
  std::string text;
};
void print_capture_cb(void* env, const char* str, size_t len) {
  static_cast<PrintCapture*>(env)->text.append(str, len);
}
[[nodiscard]] std::string py_str(mp_obj_t o) {
  PrintCapture cap;
  mp_print_t pr = {&cap, print_capture_cb};
  mp_obj_print_helper(&pr, o, PRINT_STR);
  return std::move(cap.text);
}

// Append the RESP error line from a `-...\r\n` reply.
[[nodiscard]] std::string_view resp_error_message(std::string_view reply) {
  std::string_view msg = reply.substr(1);
  if (const auto crlf = msg.find('\r'); crlf != std::string_view::npos) {
    msg = msg.substr(0, crlf);
  }
  return msg;
}

// --- RESP reply -> Python value (redis.call return) ------------------------
[[nodiscard]] mp_obj_t resp_to_py(const char** pp, const char* end) {
  const char* p = *pp;
  if (p >= end) {
    *pp = end;
    return mp_const_none;
  }
  const char type = *p++;
  std::string_view line;
  mp_obj_t result = mp_const_none;
  switch (type) {
    case ':':
      p = read_line(p, end, &line);
      result = mp_obj_new_int_from_ll(parse_signed(line));
      break;
    case '+':
    case '-':
      p = read_line(p, end, &line);
      result = new_str(line);
      break;
    case '$': {
      p = read_line(p, end, &line);
      const long long len = parse_signed(line);
      if (len < 0) {
        result = mp_const_none;
      } else {
        result = mp_obj_new_str(p, static_cast<size_t>(len));
        p += len;
        if (p + 2 <= end) p += 2;
      }
      break;
    }
    case '*': {
      p = read_line(p, end, &line);
      const long long count = parse_signed(line);
      if (count < 0) {
        result = mp_const_none;
      } else {
        result = mp_obj_new_list(0, nullptr);
        for (long long i = 0; i < count; ++i) {
          mp_obj_list_append(result, resp_to_py(&p, end));
        }
      }
      break;
    }
    default:
      p = end;
      break;
  }
  *pp = p;
  return result;
}

[[nodiscard]] mp_obj_t dict_get_or_null(mp_obj_t dict, const char* key) {
  mp_map_t* map = mp_obj_dict_get_map(dict);
  mp_map_elem_t* elem =
      mp_map_lookup(map, MP_OBJ_NEW_QSTR(qstr_from_str(key)), MP_MAP_LOOKUP);
  return elem != nullptr ? elem->value : MP_OBJ_NULL;
}

// --- Python value -> RESP reply (the script's `reply`) ---------------------
void py_to_resp(mp_obj_t o, std::string& out) {
  if (o == mp_const_none) {
    resp::append_null_bulk_string(out);
    return;
  }
  if (o == mp_const_true) {
    resp::append_integer(out, 1);
    return;
  }
  if (o == mp_const_false) {
    resp::append_null_bulk_string(out);
    return;
  }
  if (mp_obj_is_int(o)) {
    mp_int_t v = 0;
    if (mp_obj_get_int_maybe(o, &v)) {
      resp::append_integer(out, static_cast<long long>(v));
    } else {
      resp::append_bulk_string(out, py_str(o));  // big int beyond mp_int_t
    }
    return;
  }
  if (mp_obj_is_float(o)) {
    const double d = mp_obj_get_float(o);
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
    resp::append_bulk_string(out, std::string_view(buf, n > 0 ? n : 0));
    return;
  }
  if (mp_obj_is_str_or_bytes(o)) {
    resp::append_bulk_string(out, str_view(o));
    return;
  }
  if (mp_obj_is_type(o, &mp_type_list) || mp_obj_is_type(o, &mp_type_tuple)) {
    size_t n = 0;
    mp_obj_t* items = nullptr;
    mp_obj_get_array(o, &n, &items);
    resp::append_array_header(out, n);
    for (size_t i = 0; i < n; ++i) py_to_resp(items[i], out);
    return;
  }
  if (mp_obj_is_type(o, &mp_type_dict)) {
    if (mp_obj_t err = dict_get_or_null(o, "err");
        err != MP_OBJ_NULL && mp_obj_is_str_or_bytes(err)) {
      resp::append_error(out, sanitize_line(str_view(err)));
      return;
    }
    if (mp_obj_t ok = dict_get_or_null(o, "ok");
        ok != MP_OBJ_NULL && mp_obj_is_str_or_bytes(ok)) {
      resp::append_simple_string(out, sanitize_line(str_view(ok)));
      return;
    }
    resp::append_null_bulk_string(out);
    return;
  }
  resp::append_null_bulk_string(out);  // functions/other -> nil
}

// --- the `redis` module's C functions --------------------------------------
mp_obj_t upy_redis_call_impl(size_t n_args, const mp_obj_t* args, bool raise) {
  UPythonEngine* e = s_engine;
  e->call_begin();
  for (size_t i = 0; i < n_args; ++i) {
    const mp_obj_t a = args[i];
    if (mp_obj_is_str_or_bytes(a)) {
      e->call_add_arg(str_view(a));
    } else if (mp_obj_is_int(a)) {
      char buf[24];
      const int n = std::snprintf(buf, sizeof(buf), "%lld",
                                  static_cast<long long>(mp_obj_get_int(a)));
      e->call_add_arg(std::string_view(buf, n > 0 ? n : 0));
    } else if (mp_obj_is_float(a)) {
      char buf[32];
      const int n = std::snprintf(buf, sizeof(buf), "%.17g", mp_obj_get_float(a));
      e->call_add_arg(std::string_view(buf, n > 0 ? n : 0));
    } else {
      mp_raise_TypeError(MP_ERROR_TEXT("redis.call arguments must be str, bytes, int or float"));
    }
  }

  const std::string_view reply = e->call_dispatch();
  if (!reply.empty() && reply.front() == '-') {
    const std::string_view msg = resp_error_message(reply);
    if (raise) {
      nlr_raise(mp_obj_new_exception_arg1(&mp_type_Exception,
                                          mp_obj_new_str(msg.data(), msg.size())));
    }
    return new_str(msg);  // pcall: return the message text
  }
  const char* p = reply.data();
  const char* end = p + reply.size();
  return resp_to_py(&p, end);
}

mp_obj_t upy_redis_call(size_t n, const mp_obj_t* a) {
  return upy_redis_call_impl(n, a, true);
}
mp_obj_t upy_redis_pcall(size_t n, const mp_obj_t* a) {
  return upy_redis_call_impl(n, a, false);
}
MP_DEFINE_CONST_FUN_OBJ_VAR(upy_redis_call_obj, 1, upy_redis_call);
MP_DEFINE_CONST_FUN_OBJ_VAR(upy_redis_pcall_obj, 1, upy_redis_pcall);

mp_obj_t upy_redis_reply_dict(const char* kind, mp_obj_t value) {
  mp_obj_t d = mp_obj_new_dict(1);
  mp_obj_dict_store(d, mp_obj_new_str(kind, strlen(kind)), value);
  return d;
}
mp_obj_t upy_redis_error(mp_obj_t s) { return upy_redis_reply_dict("err", s); }
mp_obj_t upy_redis_status(mp_obj_t s) { return upy_redis_reply_dict("ok", s); }
MP_DEFINE_CONST_FUN_OBJ_1(upy_redis_error_obj, upy_redis_error);
MP_DEFINE_CONST_FUN_OBJ_1(upy_redis_status_obj, upy_redis_status);

mp_obj_t upy_redis_sha1hex(mp_obj_t s) {
  char hex[40];
  sha1_hex_into(str_view(s), hex);
  return mp_obj_new_str(hex, 40);
}
MP_DEFINE_CONST_FUN_OBJ_1(upy_redis_sha1hex_obj, upy_redis_sha1hex);

mp_obj_t upy_redis_log(mp_obj_t level, mp_obj_t msg) {
  (void)level;
  const std::string_view m = mp_obj_is_str_or_bytes(msg) ? str_view(msg)
                                                         : std::string_view{};
  std::fprintf(stderr, "goblin-core upython script: %.*s\n",
               static_cast<int>(m.size()), m.data());
  return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(upy_redis_log_obj, upy_redis_log);

void store_attr(mp_obj_t module, const char* name, mp_obj_t fn) {
  mp_obj_dict_store(MP_OBJ_FROM_PTR(mp_obj_module_get_globals(module)),
                    MP_OBJ_NEW_QSTR(qstr_from_str(name)), fn);
}

// The concise message from an exception: the last line of its printed traceback
// (which is "ExcType: message").
[[nodiscard]] std::string exception_message(mp_obj_t exc) {
  PrintCapture cap;
  mp_print_t pr = {&cap, print_capture_cb};
  mp_obj_print_exception(&pr, exc);
  std::string_view all = cap.text;
  while (!all.empty() && (all.back() == '\n' || all.back() == '\r')) {
    all.remove_suffix(1);
  }
  if (const auto nl = all.rfind('\n'); nl != std::string_view::npos) {
    all = all.substr(nl + 1);
  }
  return std::string(all);
}

}  // namespace

// --- engine-owned command dispatch (MicroPython-free) ----------------------
void UPythonEngine::call_begin() {
  call_arg_storage_.clear();
  call_args_.clear();
}

void UPythonEngine::call_add_arg(std::string_view arg) {
  call_arg_storage_.emplace_back(arg);
}

std::string_view UPythonEngine::call_dispatch() {
  call_args_.reserve(call_arg_storage_.size());
  for (const std::string& s : call_arg_storage_) call_args_.emplace_back(s);
  call_reply_.clear();
  if (call_args_.empty() || command_blocked_in_script(call_args_[0])) {
    call_reply_ = "-This Redis command is not allowed from script\r\n";
    return call_reply_;
  }
  handle_command_into(
      store_, call_args_, call_reply_,
      CommandExecutionOptions{
          .replication_context = nested_dispatch_.replication_context,
          .replicate_write = nested_dispatch_.replicate_write,
          .read_only = nested_dispatch_.read_only,
          .nested_dispatch = nested_dispatch_});
  return call_reply_;
}

UPythonEngine::UPythonEngine(Store& store, NestedCommandDispatch nested_dispatch)
    : store_(store), nested_dispatch_(nested_dispatch) {}

UPythonEngine::~UPythonEngine() {
  if (vm_ready_) {
    mp_deinit();
    vm_ready_ = false;
  }
  std::free(gc_heap_);
  gc_heap_ = nullptr;
}

void UPythonEngine::ensure_vm() {
  if (vm_ready_) return;

  constexpr std::size_t kHeapSize = 1024 * 1024;  // 1 MiB GC heap
  gc_heap_ = std::malloc(kHeapSize);
  if (gc_heap_ == nullptr) return;

  volatile char stack_marker = 0;
  mp_stack_set_top((void*)((char*)&stack_marker + 8192));
  mp_stack_set_limit(512 * 1024);
  gc_init(gc_heap_, static_cast<char*>(gc_heap_) + kHeapSize);
  mp_init();
  vm_ready_ = true;

  // Build the `redis` module once. mp_obj_new_module registers it in the
  // loaded-modules dict (a GC root), so the cached pointer stays valid.
  mp_obj_t redis_mod = mp_obj_new_module(qstr_from_str("redis"));
  store_attr(redis_mod, "call", MP_OBJ_FROM_PTR(&upy_redis_call_obj));
  store_attr(redis_mod, "pcall", MP_OBJ_FROM_PTR(&upy_redis_pcall_obj));
  store_attr(redis_mod, "error", MP_OBJ_FROM_PTR(&upy_redis_error_obj));
  store_attr(redis_mod, "status", MP_OBJ_FROM_PTR(&upy_redis_status_obj));
  store_attr(redis_mod, "sha1hex", MP_OBJ_FROM_PTR(&upy_redis_sha1hex_obj));
  store_attr(redis_mod, "log", MP_OBJ_FROM_PTR(&upy_redis_log_obj));
  redis_module_ = MP_OBJ_TO_PTR(redis_mod);

  // The shared globals dict that every compiled script binds to (mp_compile
  // captures the current globals); run() clears and repopulates it per call, so
  // scripts stay isolated. A list roots the compiled functions themselves. Both
  // hang off the redis module, which is a GC root (in the loaded-modules dict),
  // so neither the globals nor the cached functions are collected.
  mp_obj_t run_globals = mp_obj_new_dict(8);
  run_globals_ = MP_OBJ_TO_PTR(run_globals);
  store_attr(redis_mod, "__globals__", run_globals);
  mp_obj_t roots = mp_obj_new_list(0, nullptr);
  compiled_roots_ = MP_OBJ_TO_PTR(roots);
  store_attr(redis_mod, "__scripts__", roots);
}

void* UPythonEngine::compile_body(std::string_view body, std::string& out) {
  ensure_vm();
  if (!vm_ready_) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return nullptr;
  }

  volatile char stack_marker = 0;
  mp_stack_set_top((void*)((char*)&stack_marker + 8192));
  mp_stack_set_limit(512 * 1024);

  void* result = nullptr;
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    // mp_compile binds the module function to the current globals, so point them
    // at the shared run-globals dict that run() repopulates each call.
    mp_obj_dict_t* g = static_cast<mp_obj_dict_t*>(run_globals_);
    mp_globals_set(g);
    mp_locals_set(g);
    mp_lexer_t* lex = mp_lexer_new_from_str_len(qstr_from_str("<upython>"),
                                                body.data(), body.size(), 0);
    qstr source_name = lex->source_name;
    mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
    mp_obj_t fun = mp_compile(&parse_tree, source_name, false);
    result = MP_OBJ_TO_PTR(fun);
    nlr_pop();
  } else {
    resp::append_error(out, "ERR Error compiling script: " +
                                sanitize_line(exception_message(
                                    MP_OBJ_FROM_PTR(nlr.ret_val))));
    return nullptr;
  }
  return result;
}

void UPythonEngine::run(void* module_fun,
                        std::span<const std::string_view> keys,
                        std::span<const std::string_view> argv,
                        std::string& out) {
  volatile char stack_marker = 0;
  mp_stack_set_top((void*)((char*)&stack_marker + 8192));
  mp_stack_set_limit(512 * 1024);

  s_engine = this;
  std::string reply_scratch;
  bool ok = false;

  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    // Reset the shared globals to a fresh namespace (isolation between scripts),
    // then bind KEYS/ARGV/redis/reply. Builtins are still reachable via fallback.
    mp_obj_t gdict = MP_OBJ_FROM_PTR(run_globals_);
    mp_map_clear(mp_obj_dict_get_map(gdict));

    mp_obj_t keys_list = mp_obj_new_list(0, nullptr);
    for (const std::string_view k : keys) mp_obj_list_append(keys_list, new_str(k));
    mp_obj_t argv_list = mp_obj_new_list(0, nullptr);
    for (const std::string_view a : argv) mp_obj_list_append(argv_list, new_str(a));
    mp_obj_dict_store(gdict, MP_OBJ_NEW_QSTR(qstr_from_str("KEYS")), keys_list);
    mp_obj_dict_store(gdict, MP_OBJ_NEW_QSTR(qstr_from_str("ARGV")), argv_list);
    mp_obj_dict_store(gdict, MP_OBJ_NEW_QSTR(qstr_from_str("redis")),
                      MP_OBJ_FROM_PTR(redis_module_));
    mp_obj_dict_store(gdict, MP_OBJ_NEW_QSTR(qstr_from_str("reply")), mp_const_none);

    mp_obj_dict_t* g = static_cast<mp_obj_dict_t*>(run_globals_);
    mp_globals_set(g);
    mp_locals_set(g);
    mp_call_function_0(MP_OBJ_FROM_PTR(module_fun));  // precompiled -- no reparse

    mp_obj_t reply = dict_get_or_null(gdict, "reply");
    py_to_resp(reply != MP_OBJ_NULL ? reply : mp_const_none, reply_scratch);
    ok = true;
    nlr_pop();
  } else {
    // Uncaught exception (script error or an uncaught redis.call error).
    reply_scratch.clear();
    resp::append_error(
        out, ensure_error_code(sanitize_line(
                 exception_message(MP_OBJ_FROM_PTR(nlr.ret_val)))));
  }

  s_engine = nullptr;
  if (ok) out.append(reply_scratch);
}

void UPythonEngine::eval(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'upython.eval' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string_view body = args[0];
  const std::string sha = sha1_hex(body);
  // Compile once and cache; a repeat EVAL of the same source then takes the same
  // no-recompile path as EVALSHA (looked up by SHA1 before compiling).
  if (const auto it = scripts_.find(sha); it != scripts_.end()) {
    run(it->second, keys, argv, out);
    return;
  }
  void* fun = compile_body(body, out);
  if (fun == nullptr) return;
  scripts_[sha] = fun;
  mp_obj_list_append(MP_OBJ_FROM_PTR(compiled_roots_), MP_OBJ_FROM_PTR(fun));
  run(fun, keys, argv, out);
}

void UPythonEngine::eval_sha(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'upython.evalsha' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string sha = to_lower(args[0]);
  const auto it = scripts_.find(sha);
  if (it == scripts_.end()) {
    resp::append_error(out, "NOSCRIPT No matching script. Please use UPYTHON.EVAL.");
    return;
  }
  run(it->second, keys, argv, out);  // cached compiled function -- no reparse
}

void UPythonEngine::script(std::span<const std::string_view> args, std::string& out) {
  if (args.empty()) {
    resp::append_error(out, "ERR wrong number of arguments for 'upython.script' command");
    return;
  }

  const std::string_view sub = args[0];
  if (equals_upper(sub, "LOAD")) {
    if (args.size() != 2) {
      resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
      return;
    }
    // Compile (validates syntax, catching a SyntaxError) and cache the compiled
    // function, so a later EVALSHA skips the lex/parse/compile step.
    const std::string_view body = args[1];
    const std::string sha = sha1_hex(body);
    if (scripts_.find(sha) == scripts_.end()) {
      void* fun = compile_body(body, out);
      if (fun == nullptr) return;  // error already written
      scripts_[sha] = fun;
      mp_obj_list_append(MP_OBJ_FROM_PTR(compiled_roots_), MP_OBJ_FROM_PTR(fun));
    }
    resp::append_bulk_string(out, sha);
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
    // Drop the rooting list so the compiled functions become collectable.
    if (compiled_roots_ != nullptr) {
      mp_obj_t fresh = mp_obj_new_list(0, nullptr);
      compiled_roots_ = MP_OBJ_TO_PTR(fresh);
      store_attr(MP_OBJ_FROM_PTR(redis_module_), "__scripts__", fresh);
    }
    resp::append_simple_string(out, "OK");
    return;
  }

  resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
}

}  // namespace goblin::core
