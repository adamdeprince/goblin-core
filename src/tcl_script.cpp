#include "goblin/core/tcl_script.hpp"

#include "goblin/core/command.hpp"
#include "goblin/core/detail/script_shared.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

// Jim Tcl is C and its header has no extern "C" guard. Jim reports errors by
// return code (never longjmp/exception into the host), and wrenAbortFiber-style
// aborts are absent, so the foreign command here uses ordinary C++ locals freely.
extern "C" {
#include "jim.h"
}

namespace goblin::core {

using namespace script_shared;

namespace {

// Sentinel marking an explicit reply built by `redis error/status/integer/...`.
// The control bytes make an accidental collision with real script data
// practically impossible.
constexpr std::string_view kReplyTag = "\x1e\x1fGOBLIN-REPLY\x1f\x1e";

constexpr const char* kEngineKey = "goblin.tcl.engine";

[[nodiscard]] std::string_view jim_view(Jim_Obj* obj) {
  int len = 0;
  const char* s = Jim_GetString(obj, &len);
  return std::string_view(s, static_cast<std::size_t>(len));
}

[[nodiscard]] Jim_Obj* new_str(Jim_Interp* interp, std::string_view s) {
  return Jim_NewStringObj(interp, s.data(), static_cast<int>(s.size()));
}

[[nodiscard]] Jim_Obj* build_list(Jim_Interp* interp,
                                  std::span<const std::string_view> items) {
  Jim_Obj* list = Jim_NewListObj(interp, nullptr, 0);
  for (const std::string_view item : items) {
    Jim_ListAppendElement(interp, list, new_str(interp, item));
  }
  return list;
}

// RESP reply -> Tcl value. Integers become int objects, bulk/status strings
// become strings (nil -> empty string, since Tcl has no null), arrays become Tcl
// lists, and an error line becomes its message string.
[[nodiscard]] Jim_Obj* resp_to_tcl(Jim_Interp* interp, const char** pp,
                                   const char* end) {
  const char* p = *pp;
  if (p >= end) {
    *pp = end;
    return new_str(interp, "");
  }
  const char type = *p++;
  std::string_view line;
  Jim_Obj* obj = nullptr;
  switch (type) {
    case ':':
      p = read_line(p, end, &line);
      obj = Jim_NewIntObj(interp, static_cast<jim_wide>(parse_signed(line)));
      break;
    case '+':
    case '-':
      p = read_line(p, end, &line);
      obj = new_str(interp, line);
      break;
    case '$': {
      p = read_line(p, end, &line);
      const long long len = parse_signed(line);
      if (len < 0) {
        obj = new_str(interp, "");
      } else {
        obj = Jim_NewStringObj(interp, p, static_cast<int>(len));
        p += len;
        if (p + 2 <= end) p += 2;
      }
      break;
    }
    case '*': {
      p = read_line(p, end, &line);
      const long long count = parse_signed(line);
      obj = Jim_NewListObj(interp, nullptr, 0);
      for (long long i = 0; i < count; ++i) {
        Jim_ListAppendElement(interp, obj, resp_to_tcl(interp, &p, end));
      }
      break;
    }
    default:
      obj = new_str(interp, "");
      p = end;
      break;
  }
  *pp = p;
  return obj;
}

// Tcl value -> RESP reply. An explicit reply built by a `redis` reply-builder is
// a 3-element list tagged with kReplyTag; otherwise a canonical integer becomes
// an integer reply and everything else a bulk string.
// Callers must keep `obj` referenced (Jim_IncrRefCount) across this call: it
// inspects values with Jim_GetWide, which on a non-integer sets an error result
// and would otherwise drop the last reference to `obj` (or a nested element)
// mid-conversion. run() and the array recursion below both hold a reference.
void tcl_result_to_resp(Jim_Interp* interp, Jim_Obj* obj, std::string& out) {
  if (Jim_ListLength(interp, obj) == 3 &&
      jim_view(Jim_ListGetIndex(interp, obj, 0)) == kReplyTag) {
    const std::string_view kind = jim_view(Jim_ListGetIndex(interp, obj, 1));
    Jim_Obj* payload = Jim_ListGetIndex(interp, obj, 2);
    if (kind == "error") {
      resp::append_error(out, sanitize_line(jim_view(payload)));
      return;
    }
    if (kind == "status") {
      resp::append_simple_string(out, sanitize_line(jim_view(payload)));
      return;
    }
    if (kind == "integer") {
      jim_wide w = 0;
      Jim_GetWide(interp, payload, &w);
      resp::append_integer(out, static_cast<long long>(w));
      return;
    }
    if (kind == "nil") {
      resp::append_null_bulk_string(out);
      return;
    }
    if (kind == "array") {
      const int n = Jim_ListLength(interp, payload);
      resp::append_array_header(out, static_cast<std::size_t>(n));
      for (int i = 0; i < n; ++i) {
        tcl_result_to_resp(interp, Jim_ListGetIndex(interp, payload, i), out);
      }
      return;
    }
  }

  // Plain value: a canonical integer replies as an integer, else a bulk string.
  const std::string_view sv = jim_view(obj);
  jim_wide w = 0;
  if (Jim_GetWide(interp, obj, &w) == JIM_OK) {
    char buf[24];
    const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(w));
    if (n > 0 && sv == std::string_view(buf, static_cast<std::size_t>(n))) {
      resp::append_integer(out, static_cast<long long>(w));
      return;
    }
  }
  resp::append_bulk_string(out, sv);
}

[[nodiscard]] int reply_tag(Jim_Interp* interp, const char* kind, Jim_Obj* payload) {
  Jim_Obj* items[3] = {new_str(interp, kReplyTag), Jim_NewStringObj(interp, kind, -1),
                       payload};
  Jim_SetResult(interp, Jim_NewListObj(interp, items, 3));
  return JIM_OK;
}

// The Jim trampoline: recover the engine from the interpreter's assoc data.
int tcl_redis_cmd(Jim_Interp* interp, int argc, Jim_Obj* const* argv) {
  auto* engine = static_cast<TclEngine*>(Jim_GetAssocData(interp, kEngineKey));
  return engine->redis_command(interp, argc, argv);
}

void delete_command(Jim_Interp* interp, const char* name) {
  Jim_DeleteCommand(interp, Jim_NewStringObj(interp, name, -1));
}

}  // namespace

int TclEngine::redis_command(Jim_Interp* interp, int argc, Jim_Obj* const* argv) {
  if (argc < 2) {
    Jim_WrongNumArgs(interp, 1, argv, "subcommand ?arg ...?");
    return JIM_ERR;
  }
  const std::string_view sub = jim_view(argv[1]);

  if (sub == "call" || sub == "pcall") {
    const bool raise = (sub == "call");
    if (argc < 3) {
      Jim_WrongNumArgs(interp, 2, argv, "command ?arg ...?");
      return JIM_ERR;
    }
    call_arg_storage_.clear();
    call_arg_storage_.reserve(static_cast<std::size_t>(argc - 2));
    for (int i = 2; i < argc; ++i) {
      call_arg_storage_.emplace_back(jim_view(argv[i]));
    }
    call_args_.clear();
    call_args_.reserve(call_arg_storage_.size());
    for (const std::string& s : call_arg_storage_) call_args_.emplace_back(s);

    if (command_blocked_in_script(call_args_[0])) {
      Jim_SetResultString(interp, "This Redis command is not allowed from script", -1);
      return JIM_ERR;
    }

    call_reply_.clear();
    handle_command_into(
        store_, call_args_, call_reply_,
        CommandExecutionOptions{.nested_dispatch = nested_dispatch_});

    if (!call_reply_.empty() && call_reply_.front() == '-') {
      std::string_view msg = call_reply_;
      msg.remove_prefix(1);
      if (const auto crlf = msg.find('\r'); crlf != std::string_view::npos) {
        msg = msg.substr(0, crlf);
      }
      Jim_SetResultString(interp, msg.data(), static_cast<int>(msg.size()));
      return raise ? JIM_ERR : JIM_OK;  // pcall returns the message as the result
    }

    const char* p = call_reply_.data();
    const char* end = p + call_reply_.size();
    Jim_SetResult(interp, resp_to_tcl(interp, &p, end));
    return JIM_OK;
  }

  if (sub == "error" || sub == "status" || sub == "integer" || sub == "array") {
    if (argc != 3) {
      Jim_WrongNumArgs(interp, 2, argv, "value");
      return JIM_ERR;
    }
    return reply_tag(interp, std::string(sub).c_str(), argv[2]);
  }
  if (sub == "nil") {
    return reply_tag(interp, "nil", new_str(interp, ""));
  }
  if (sub == "sha1hex") {
    if (argc != 3) {
      Jim_WrongNumArgs(interp, 2, argv, "string");
      return JIM_ERR;
    }
    char hex[40];
    sha1_hex_into(jim_view(argv[2]), hex);
    Jim_SetResultString(interp, hex, 40);
    return JIM_OK;
  }
  if (sub == "log") {
    if (argc >= 4) {
      std::fprintf(stderr, "goblin-core tcl script: %.*s\n",
                   static_cast<int>(jim_view(argv[3]).size()), jim_view(argv[3]).data());
    }
    return JIM_OK;
  }

  Jim_SetResultFormatted(interp, "unknown redis subcommand \"%#s\"", argv[1]);
  return JIM_ERR;
}

TclEngine::TclEngine(Store& store, NestedCommandDispatch nested_dispatch)
    : store_(store), nested_dispatch_(nested_dispatch) {}

TclEngine::~TclEngine() {
  if (interp_ != nullptr) {
    for (const auto& [sha, obj] : scripts_) Jim_DecrRefCount(interp_, obj);
    scripts_.clear();
    Jim_FreeInterp(interp_);
    interp_ = nullptr;
  }
}

void TclEngine::ensure_vm() {
  if (interp_ != nullptr) return;

  interp_ = Jim_CreateInterp();
  if (interp_ == nullptr) return;
  Jim_RegisterCoreCommands(interp_);
  Jim_InitStaticExtensions(interp_);  // stdlib + tclcompat only (see third_party)

  Jim_SetAssocData(interp_, kEngineKey, nullptr, this);
  Jim_CreateCommand(interp_, "redis", tcl_redis_cmd, this, nullptr);

  // Sandbox: drop the commands that could touch the host or the process. No I/O,
  // exec, socket, event-loop, or child-interp extensions are compiled in, so the
  // only survivors that matter are these.
  for (const char* name : {"exit", "source", "popen", "puts"}) {
    delete_command(interp_, name);
  }
  Jim_Eval(interp_, "catch {unset ::env}");  // no environment-variable access
}

Jim_Obj* TclEngine::intern_script(const std::string& sha, std::string_view body) {
  if (const auto it = scripts_.find(sha); it != scripts_.end()) {
    return it->second;
  }
  // Jim_EvalObj is binary-safe (unlike Jim_Eval, which takes a C string). Holding
  // a reference keeps the object -- and the compiled script rep Jim caches on it
  // after the first eval -- alive for later EVALSHA calls.
  Jim_Obj* script =
      Jim_NewStringObj(interp_, body.data(), static_cast<int>(body.size()));
  Jim_IncrRefCount(script);
  scripts_.emplace(sha, script);
  return script;
}

void TclEngine::run(Jim_Obj* script,
                    std::span<const std::string_view> keys,
                    std::span<const std::string_view> argv,
                    std::string& out) {
  Jim_SetVariableStr(interp_, "KEYS", build_list(interp_, keys));
  Jim_SetVariableStr(interp_, "ARGV", build_list(interp_, argv));

  const int rc = Jim_EvalObj(interp_, script);  // reuses the cached compiled rep

  if (rc == JIM_OK || rc == JIM_RETURN) {
    // Hold a reference to the result: converting it calls Jim_GetWide, which on a
    // non-integer replaces the interpreter result and would otherwise free this
    // object (and its nested elements) part-way through the walk.
    Jim_Obj* result = Jim_GetResult(interp_);
    Jim_IncrRefCount(result);
    tcl_result_to_resp(interp_, result, out);
    Jim_DecrRefCount(interp_, result);
    return;
  }
  resp::append_error(out, ensure_error_code(sanitize_line(jim_view(Jim_GetResult(interp_)))));
}

void TclEngine::eval(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'tcl.eval' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  ensure_vm();
  if (interp_ == nullptr) {
    resp::append_error(out, "ERR could not initialize scripting engine");
    return;
  }
  // Tcl has no separate compile phase (parse errors surface at run time), so a
  // submitted script is always cached, matching EVAL's "cache then run". The
  // interned object reuses its compiled rep on a later EVALSHA / repeat EVAL.
  Jim_Obj* script = intern_script(sha1_hex(args[0]), args[0]);
  run(script, keys, argv, out);
}

void TclEngine::eval_sha(std::span<const std::string_view> args, std::string& out) {
  if (args.size() < 2) {
    resp::append_error(out, "ERR wrong number of arguments for 'tcl.evalsha' command");
    return;
  }
  std::span<const std::string_view> keys;
  std::span<const std::string_view> argv;
  if (!split_keys_and_args(args, &keys, &argv, out)) return;

  const std::string sha = to_lower(args[0]);
  const auto it = scripts_.find(sha);
  if (it == scripts_.end()) {
    resp::append_error(out, "NOSCRIPT No matching script. Please use TCL.EVAL.");
    return;
  }
  // A cached entry implies the interpreter exists (it was created when interned).
  run(it->second, keys, argv, out);
}

void TclEngine::script(std::span<const std::string_view> args, std::string& out) {
  if (args.empty()) {
    resp::append_error(out, "ERR wrong number of arguments for 'tcl.script' command");
    return;
  }

  const std::string_view sub = args[0];
  if (equals_upper(sub, "LOAD")) {
    if (args.size() != 2) {
      resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
      return;
    }
    ensure_vm();
    if (interp_ == nullptr) {
      resp::append_error(out, "ERR could not initialize scripting engine");
      return;
    }
    // Tcl cannot be validated without running it, but `info complete` catches the
    // common unbalanced-brace/quote error without executing anything.
    const std::string_view body = args[1];
    Jim_SetVariableStr(interp_, "__goblin_load_src",
                       Jim_NewStringObj(interp_, body.data(), static_cast<int>(body.size())));
    Jim_Eval(interp_, "info complete $__goblin_load_src");
    jim_wide complete = 0;
    Jim_GetWide(interp_, Jim_GetResult(interp_), &complete);
    if (complete != 1) {
      resp::append_error(out, "ERR Error compiling script: script is not complete "
                              "(unbalanced braces, brackets, or quotes)");
      return;
    }
    const std::string sha = sha1_hex(body);
    (void)intern_script(sha, body);
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
    for (const auto& [sha, obj] : scripts_) Jim_DecrRefCount(interp_, obj);
    scripts_.clear();
    resp::append_simple_string(out, "OK");
    return;
  }

  resp::append_error(out, "ERR Unknown SCRIPT subcommand or wrong number of arguments");
}

}  // namespace goblin::core
