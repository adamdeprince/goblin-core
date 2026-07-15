#pragma once

#include "goblin/core/nested_command_dispatch.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Jim Tcl's opaque types, forward-declared (matching jim.h's tags) so this header
// stays free of the vendored Jim headers.
typedef struct Jim_Interp Jim_Interp;
typedef struct Jim_Obj Jim_Obj;

namespace goblin::core {

class Store;

// The Tcl counterpart to the other scripting engines, implementing TCL.EVAL /
// TCL.EVALSHA / TCL.SCRIPT on an embedded Jim Tcl interpreter. It shares only the
// key space with the Lua and Wren engines. Same integration shape: lazy VM, one
// Store, atomic under the single-threaded loop.
//
// Tcl is string-centric, so the mapping is: the script result is the reply
// (a canonical integer becomes an integer reply, anything else a bulk string);
// the `redis` command provides `redis call`/`pcall` plus explicit reply builders
// (error/status/integer/array/nil). KEYS and ARGV are exposed as Tcl lists.
class TclEngine {
 public:
  explicit TclEngine(Store& store, NestedCommandDispatch nested_dispatch = {});
  ~TclEngine();

  TclEngine(const TclEngine&) = delete;
  TclEngine& operator=(const TclEngine&) = delete;

  void eval(std::span<const std::string_view> args, std::string& out);
  void eval_sha(std::span<const std::string_view> args, std::string& out);
  void script(std::span<const std::string_view> args, std::string& out);

  // The foreign `redis` command, invoked by the Jim trampoline (which recovers
  // the engine from the interpreter's association data). Public only so the
  // trampoline can reach it; not a stable API.
  int redis_command(Jim_Interp* interp, int argc, Jim_Obj* const* argv);

 private:
  void ensure_vm();
  // Intern the body as a ref-counted Jim script object and cache it under `sha`
  // (or return the existing one). Jim_EvalObj compiles a script object's internal
  // rep on first eval and caches it on the object, so reusing the same object is
  // what lets EVALSHA skip re-parsing. interp_ must already exist.
  Jim_Obj* intern_script(const std::string& sha, std::string_view body);
  // Evaluate an interned (precompiled) script object with KEYS/ARGV bound.
  void run(Jim_Obj* script,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  Store& store_;
  NestedCommandDispatch nested_dispatch_;
  Jim_Interp* interp_ = nullptr;
  // 40-hex SHA1 of the source -> interned (ref-counted) Jim script object, whose
  // compiled internal rep is reused across evals.
  std::unordered_map<std::string, Jim_Obj*> scripts_;

  // Reusable scratch for a `redis call` (single-threaded; never aliased).
  std::vector<std::string> call_arg_storage_;
  std::vector<std::string_view> call_args_;
  std::string call_reply_;
};

}  // namespace goblin::core
