#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Wren's VM type, forward-declared so this header stays free of the vendored
// Wren headers (which are C and must be included via extern "C").
typedef struct WrenVM WrenVM;

namespace goblin::core {

class Store;

// The Wren counterpart to the Lua engines: implements WREN.EVAL / WREN.EVALSHA /
// WREN.SCRIPT on the Wren language (https://wren.io). Wren is class-based with no
// top-level `return` and no in-language eval, so a script's source is wrapped in
// a function whose result is handed back through a foreign `Redis.setReply_`
// call; `Redis.call(list)` re-enters the shared command pipeline. KEYS/ARGV are
// exposed as 0-based Lists (Wren is 0-indexed). Same integration shape as the
// other engines: lazy VM, one shared Store, atomic under the single-threaded loop.
class WrenEngine {
 public:
  explicit WrenEngine(Store& store);
  ~WrenEngine();

  WrenEngine(const WrenEngine&) = delete;
  WrenEngine& operator=(const WrenEngine&) = delete;

  void eval(std::span<const std::string_view> args, std::string& out);
  void eval_sha(std::span<const std::string_view> args, std::string& out);
  void script(std::span<const std::string_view> args, std::string& out);

  // Foreign-method entry points (called from the C trampolines in wren_script.cpp
  // via the VM's user data). Public so the trampolines can reach them without a
  // friend dance; not part of any stable API.
  void foreign_call(WrenVM* vm, bool raise_on_error);
  void foreign_sha1hex(WrenVM* vm);
  void foreign_log(WrenVM* vm);
  void foreign_set_reply(WrenVM* vm);
  void foreign_keys(WrenVM* vm);
  void foreign_argv(WrenVM* vm);
  void report_error(std::string message);
  void note_write(std::string_view text);

 private:
  void ensure_vm();
  bool compile_ok(std::string_view body, std::string& out);  // caches on success
  // Compiles and runs `body`, writing the RESP reply to `out`. Returns true if
  // the script compiled (whether or not it then errored at runtime), so EVAL can
  // cache exactly the scripts a later EVALSHA could run.
  bool run(std::string_view body,
           std::span<const std::string_view> keys,
           std::span<const std::string_view> argv,
           std::string& out);

  Store& store_;
  WrenVM* vm_ = nullptr;
  std::unordered_map<std::string, std::string> scripts_;  // 40-hex SHA1 -> body

  // Per-run state (single-threaded; scripts never nest).
  std::span<const std::string_view> current_keys_;
  std::span<const std::string_view> current_argv_;
  std::string reply_;        // RESP built from the script's return value
  bool reply_set_ = false;
  std::string error_;        // captured compile/runtime error message

  // Reusable scratch for a redis.call.
  std::vector<std::string> call_arg_storage_;
  std::vector<std::string_view> call_args_;
  std::string call_reply_;
};

}  // namespace goblin::core
