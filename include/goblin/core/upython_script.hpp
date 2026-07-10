#pragma once

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
  explicit UPythonEngine(Store& store);
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
  void run(std::string_view body,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  Store& store_;
  bool vm_ready_ = false;
  void* gc_heap_ = nullptr;
  void* redis_module_ = nullptr;  // mp_obj_t, kept alive by the loaded-modules root
  std::unordered_map<std::string, std::string> scripts_;  // 40-hex SHA1 -> body

  // Reusable scratch for a redis.call (single-threaded; never aliased).
  std::vector<std::string> call_arg_storage_;
  std::vector<std::string_view> call_args_;
  std::string call_reply_;
};

}  // namespace goblin::core
