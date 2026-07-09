#pragma once

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
  explicit TclEngine(Store& store);
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
  void run(std::string_view body,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  Store& store_;
  Jim_Interp* interp_ = nullptr;
  std::unordered_map<std::string, std::string> scripts_;  // 40-hex SHA1 -> body

  // Reusable scratch for a `redis call` (single-threaded; never aliased).
  std::vector<std::string> call_arg_storage_;
  std::vector<std::string_view> call_args_;
  std::string call_reply_;
};

}  // namespace goblin::core
