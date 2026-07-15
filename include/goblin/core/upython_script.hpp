#pragma once

#include "goblin/core/nested_command_dispatch.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace goblin::core {

class Store;

// The MicroPython counterpart to the other scripting engines, implementing
// UPYTHON.EVAL / UPYTHON.EVALSHA / UPYTHON.SCRIPT on an embedded MicroPython
// (the ports/embed build). It shares only the key space with the Lua, Wren, and
// Tcl engines.
//
// Python has no top-level return, so a script produces its reply by assigning to
// the module global `reply` (the Python analogue of Wren's Redis.setReply_).
// `redis.call(...)` re-enters the shared command pipeline. KEYS and ARGV are
// exposed as lists of byte-strings (str, with unicode disabled for binary
// safety). The GC heap is allocated lazily on the first script.
//
// MicroPython keeps a single global VM state, so there is one engine per process;
// the server owns it. Scripts never nest (UPYTHON.* are rejected from redis.call).
class UPythonEngine {
 public:
  explicit UPythonEngine(Store& store,
                         NestedCommandDispatch nested_dispatch = {});
  ~UPythonEngine();

  UPythonEngine(const UPythonEngine&) = delete;
  UPythonEngine& operator=(const UPythonEngine&) = delete;

  void eval(std::span<const std::string_view> args, std::string& out);
  void eval_sha(std::span<const std::string_view> args, std::string& out);
  void script(std::span<const std::string_view> args, std::string& out);

  // Used by the `redis` module's C functions, which recover the engine from a
  // file-scope pointer set for the duration of a run. These are MicroPython-free
  // so this header needs none of its headers.
  void call_begin();
  void call_add_arg(std::string_view arg);
  [[nodiscard]] std::string_view call_dispatch();  // runs it, returns RESP bytes

 private:
  void ensure_vm();
  // Compile `body` to a MicroPython module function (bound to the shared run
  // globals) and return it as an mp_obj_t pointer -- what the cache stores. A
  // SyntaxError writes a RESP error to `out` and returns nullptr.
  void* compile_body(std::string_view body, std::string& out);
  // Call a cached compiled function with KEYS/ARGV bound, appending the RESP
  // reply. No parse/compile on this path -- the EVALSHA / cached-EVAL fast path.
  void run(void* module_fun,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  Store& store_;
  NestedCommandDispatch nested_dispatch_;
  bool vm_ready_ = false;
  void* gc_heap_ = nullptr;
  void* redis_module_ = nullptr;  // mp_obj_t, kept alive by the loaded-modules root
  void* run_globals_ = nullptr;   // shared globals dict, cleared+filled per run
  void* compiled_roots_ = nullptr;  // list rooting the cached compiled functions
  // 40-hex SHA1 of the source -> compiled module function (mp_obj_t pointer, kept
  // alive by compiled_roots_). Caching the compiled function is what lets EVALSHA
  // skip the lex/parse/compile step.
  std::unordered_map<std::string, void*> scripts_;

  // Reusable scratch for a redis.call (single-threaded; never aliased).
  std::vector<std::string> call_arg_storage_;
  std::vector<std::string_view> call_args_;
  std::string call_reply_;
};

}  // namespace goblin::core
