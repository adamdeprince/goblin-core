#include "goblin/core/command.hpp"

#include "goblin/core/luau_script.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/script.hpp"
#include "goblin/core/store.hpp"
#include "goblin/core/tcl_script.hpp"
#include "goblin/core/upython_script.hpp"
#include "goblin/core/quickjs_script.hpp"
#include "goblin/core/wren_script.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <span>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

// gperf-generated perfect hash for command dispatch (source: command_hash.gperf).
#include "command_hash.hpp"

namespace goblin::core {
namespace {

// Current process resident set size in bytes, for INFO's used_memory_rss (the
// allocator-honest number the memory benchmarks compare on). 0 if unavailable.
[[nodiscard]] std::size_t current_rss_bytes() noexcept {
#if defined(__linux__)
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) {
    return 0;
  }
  long total = 0;
  long resident = 0;
  const int n = std::fscanf(f, "%ld %ld", &total, &resident);
  std::fclose(f);
  if (n != 2 || resident < 0) {
    return 0;
  }
  return static_cast<std::size_t>(resident) *
         static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
#elif defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return info.resident_size;
#else
  return 0;
#endif
}

// A minimal redis-compatible INFO payload: enough of the Server + Memory
// sections for tooling (redis-cli, benchmark harnesses) that keys off
// used_memory_rss / redis_version. Fields are CRLF-separated per the protocol.
//
// used_memory is what our allocation layers actually hold, summed from every
// arena's O(1) live/dead counters (Store::memory_report -- no scan); used_memory_rss
// is the OS resident set. mem_fragmentation_ratio is honest and *internal*: our
// allocation over its compacted size (used minus the reclaimable dead bytes a
// GOBLIN.OPTIMIZE would free), so 1.00 means nothing to reclaim and higher means
// churn has left dead bytes in the arenas. mem_reclaimable_bytes is that dead total.
[[nodiscard]] std::string build_info_string(const Store& store) {
  const std::size_t rss = current_rss_bytes();
  const MemoryReport mem = store.memory_report();
  const std::size_t used = mem.used_memory;
  const std::size_t dead = mem.reclaimable_bytes;
  const std::size_t compacted = used > dead ? used - dead : 0;
  char frag[16];
  std::snprintf(frag, sizeof(frag), "%.2f",
                compacted > 0 ? static_cast<double>(used) / compacted : 1.0);
  std::string s;
  s += "# Server\r\n";
  s += "redis_version:7.4.0\r\n";
  s += "redis_mode:standalone\r\n";
  s += "list_implementation:" +
       std::string(list_implementation_name(store.list_implementation())) +
       "\r\n";
  s += "# Memory\r\n";
  s += "used_memory:" + std::to_string(used) + "\r\n";
  s += "used_memory_rss:" + std::to_string(rss) + "\r\n";
  s += "used_memory_peak:" + std::to_string(used) + "\r\n";
  s += "mem_reclaimable_bytes:" + std::to_string(dead) + "\r\n";
  s += "maxmemory:0\r\n";
  s += "maxmemory_policy:noeviction\r\n";
  s += "mem_fragmentation_ratio:" + std::string(frag) + "\r\n";
  return s;
}

[[nodiscard]] char ascii_upper_char(char value) noexcept {
  if (value >= 'a' && value <= 'z') {
    return static_cast<char>(value - ('a' - 'A'));
  }
  return value;
}

[[nodiscard]] CommandParseResult parse_error(std::string message) {
  return {.error = std::move(message)};
}

[[nodiscard]] bool equals_ci(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_upper_char(lhs[i]) != ascii_upper_char(rhs[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<long long> parse_i64(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }

  long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }

  return value;
}

[[nodiscard]] std::optional<double> parse_score(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }

  double value = 0.0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end || !std::isfinite(value)) {
    return std::nullopt;
  }

  return value;
}

struct ScoreBound {
  double value{0.0};
  bool exclusive{false};
};

// A ZRANGEBYSCORE-style score bound: a plain number (inclusive), `(number`
// (exclusive), or `-inf` / `+inf` / `inf` for an open side.
[[nodiscard]] std::optional<ScoreBound> parse_score_bound(std::string_view text) {
  bool exclusive = false;
  if (!text.empty() && text.front() == '(') {
    exclusive = true;
    text.remove_prefix(1);
  }
  if (equals_ci(text, "-inf")) {
    return ScoreBound{-std::numeric_limits<double>::infinity(), exclusive};
  }
  if (equals_ci(text, "+inf") || equals_ci(text, "inf")) {
    return ScoreBound{std::numeric_limits<double>::infinity(), exclusive};
  }
  const auto value = parse_score(text);
  if (!value) {
    return std::nullopt;
  }
  return ScoreBound{*value, exclusive};
}

[[nodiscard]] std::string wrong_arity(std::string_view command) {
  std::string message = "ERR wrong number of arguments for '";
  message.append(command);
  message.append("' command");
  return message;
}

[[nodiscard]] std::string syntax_error() {
  return "ERR syntax error";
}

[[nodiscard]] std::string integer_range_error() {
  return "ERR value is not an integer or out of range";
}

[[nodiscard]] bool parse_range_indexes(Command& command) {
  const auto start = parse_i64(command.args[1]);
  const auto stop = parse_i64(command.args[2]);
  if (!start || !stop) {
    return false;
  }

  command.range_indexes_parsed = true;
  command.range_start = *start;
  command.range_stop = *stop;
  return true;
}

[[nodiscard]] std::vector<std::string> memory_stats_fields(const ZSetMemoryStats& stats) {
  std::vector<std::string> fields;
  fields.reserve(36);

  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  auto add_string = [&fields](std::string_view name, std::string_view value) {
    fields.emplace_back(name);
    fields.emplace_back(value);
  };

  add("member_count", stats.member_count);
  add_string("rank_cache_mode", rank_cache_mode_name(stats.rank_cache_mode));
  add_string("score_width", score_width_name(stats.score_width));
  add("member_storage_bytes", stats.member_storage_bytes);
  add("member_storage_allocated_bytes", stats.member_storage_allocated_bytes);
  add("member_ref_capacity", stats.member_ref_capacity);
  add("score_string_cache_bytes", stats.score_string_cache_bytes);
  add("score_string_cache_ref_capacity", stats.score_string_cache_ref_capacity);
  add("score_string_cache_allocated_bytes",
      stats.score_string_cache_allocated_bytes);
  add("member_index_capacity", stats.member_index_capacity);
  add("member_index_member_slot_capacity", stats.member_index_member_slot_capacity);
  add("member_index_tombstones", stats.member_index_tombstones);
  add("member_index_allocated_bytes", stats.member_index_allocated_bytes);
  add("score_entry_count", stats.score_entry_count);
  add("score_block_count", stats.score_block_count);
  add("score_block_capacity_sum", stats.score_block_capacity_sum);
  add("rank_location_cache_allocated_bytes",
      stats.rank_location_cache_allocated_bytes);
  add("score_index_allocated_bytes", stats.score_index_allocated_bytes);
  add("total_allocated_bytes", stats.total_allocated_bytes);

  return fields;
}

[[nodiscard]] std::string memory_stats_response(const ZSetMemoryStats& stats) {
  const auto fields = memory_stats_fields(stats);
  const std::vector<std::string_view> views(fields.begin(), fields.end());
  return resp::array(views);
}

[[nodiscard]] std::vector<std::string> hash_memory_stats_fields(const HashMemoryStats& stats) {
  std::vector<std::string> fields;
  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  add("field_count", stats.field_count);
  add("field_value_live_bytes", stats.field_value_live_bytes);
  add("field_value_dead_bytes", stats.field_value_dead_bytes);
  add("field_value_allocated_bytes", stats.field_value_allocated_bytes);
  add("field_index_allocated_bytes", stats.field_index_allocated_bytes);
  add("field_compaction_active", stats.field_compaction_active);
  add("field_compaction_victim_chunk", stats.field_compaction_victim_chunk);
  add("field_compaction_fields_scanned",
      stats.field_compaction_fields_scanned);
  add("field_compaction_fields_total", stats.field_compaction_fields_total);
  add("field_compaction_candidates_remaining",
      stats.field_compaction_candidates_remaining);
  add("field_compaction_relocated_fields",
      stats.field_compaction_relocated_fields);
  add("field_compaction_relocated_bytes",
      stats.field_compaction_relocated_bytes);
  add("total_allocated_bytes", stats.total_allocated_bytes);

  return fields;
}

[[nodiscard]] std::string hash_memory_stats_response(const HashMemoryStats& stats) {
  const auto fields = hash_memory_stats_fields(stats);
  const std::vector<std::string_view> views(fields.begin(), fields.end());
  return resp::array(views);
}

[[nodiscard]] std::vector<std::string> list_memory_stats_fields(
    const ListMemoryStats& stats) {
  std::vector<std::string> fields;
  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  add("element_count", stats.element_count);
  add("value_live_bytes", stats.value_live_bytes);
  add("value_dead_bytes", stats.value_dead_bytes);
  add("value_allocated_bytes", stats.value_allocated_bytes);
  add("order_capacity", stats.order_capacity);
  add("order_front_slack", stats.order_front_slack);
  add("order_back_slack", stats.order_back_slack);
  add("order_allocated_bytes", stats.order_allocated_bytes);
  add("total_allocated_bytes", stats.total_allocated_bytes);
  return fields;
}

[[nodiscard]] std::string list_memory_stats_response(
    const ListMemoryStats& stats) {
  const auto fields = list_memory_stats_fields(stats);
  const std::vector<std::string_view> views(fields.begin(), fields.end());
  return resp::array(views);
}

constexpr std::string_view kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

// Goblin Core caps a single string value at 64 KiB by design; larger blobs
// belong in the object store, not the keyspace.
constexpr std::string_view kValueTooLarge =
    "ERR value is larger than the 64 KiB limit; use https://goblin-store.dev";

[[nodiscard]] bool is_zset_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::zadd:
    case CommandType::zcard:
    case CommandType::zrange:
    case CommandType::zrank:
    case CommandType::zrevrange:
    case CommandType::zrevrank:
    case CommandType::zrem:
    case CommandType::zremrangebyscore:
    case CommandType::zscore:
    case CommandType::goblin_td_leaderboard_rescore:  // reads the zset like ZRANGE
    case CommandType::goblin_zwindow:                 // ZADD/ZREM/ZCARD on the zset
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_hash_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::hset:
    case CommandType::hsetnx:
    case CommandType::hget:
    case CommandType::hmget:
    case CommandType::hdel:
    case CommandType::hgetall:
    case CommandType::hkeys:
    case CommandType::hvals:
    case CommandType::hlen:
    case CommandType::hexists:
    case CommandType::hstrlen:
    case CommandType::hincrby:
    case CommandType::goblin_hcad:    // deletes a field; non-hash is WRONGTYPE
    case CommandType::goblin_hsetgt:  // sets a field; non-hash is WRONGTYPE
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_list_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::llen:
    case CommandType::lindex:
    case CommandType::lrange:
    case CommandType::lset:
    case CommandType::ltrim:
    case CommandType::lrem:
    case CommandType::linsert:
    case CommandType::pma_lpush:
    case CommandType::pma_rpush:
    case CommandType::pma_lpushx:
    case CommandType::pma_rpushx:
    case CommandType::pma_lpop:
    case CommandType::pma_rpop:
    case CommandType::pma_llen:
    case CommandType::pma_lindex:
    case CommandType::pma_lrange:
    case CommandType::pma_lset:
    case CommandType::pma_ltrim:
    case CommandType::pma_lrem:
    case CommandType::pma_linsert:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] CommandType resolve_list_command(
    CommandType type, ListImplementation implementation) noexcept {
  switch (implementation) {
    case ListImplementation::Pma:
      switch (type) {
        case CommandType::lpush:
          return CommandType::pma_lpush;
        case CommandType::rpush:
          return CommandType::pma_rpush;
        case CommandType::lpushx:
          return CommandType::pma_lpushx;
        case CommandType::rpushx:
          return CommandType::pma_rpushx;
        case CommandType::lpop:
          return CommandType::pma_lpop;
        case CommandType::rpop:
          return CommandType::pma_rpop;
        case CommandType::llen:
          return CommandType::pma_llen;
        case CommandType::lindex:
          return CommandType::pma_lindex;
        case CommandType::lrange:
          return CommandType::pma_lrange;
        case CommandType::lset:
          return CommandType::pma_lset;
        case CommandType::ltrim:
          return CommandType::pma_ltrim;
        case CommandType::lrem:
          return CommandType::pma_lrem;
        case CommandType::linsert:
          return CommandType::pma_linsert;
        default:
          return type;
      }
  }
  return CommandType::unknown;
}

// String commands that require the key to already hold a string (or be absent).
// SET / SETNX / MSET clobber or create regardless of type, and MGET / DEL /
// EXISTS / TYPE are type-agnostic, so none of those are gated. GOBLIN.CAD /
// GOBLIN.CAEXPIRE / GOBLIN.CAS are gated too: they read the value like GET, so a
// non-string key is WRONGTYPE.
[[nodiscard]] bool is_typed_string_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::get:
    case CommandType::getset:
    case CommandType::getdel:
    case CommandType::strlen:
    case CommandType::append:
    case CommandType::incr:
    case CommandType::decr:
    case CommandType::incrby:
    case CommandType::decrby:
    case CommandType::incrbyfloat:
    case CommandType::getrange:
    case CommandType::setrange:
    case CommandType::goblin_cad:
    case CommandType::goblin_caexpire:
    case CommandType::goblin_cas:
    case CommandType::goblin_increx:  // INCRs the value; non-string is WRONGTYPE
    case CommandType::goblin_incrbound:  // bounded INCR; non-string is WRONGTYPE
    case CommandType::goblin_decrpos:    // conditional DECR; non-string is WRONGTYPE
      return true;
    default:
      return false;
  }
}

// The type a command's first-key argument must already hold (or be absent) to
// escape WRONGTYPE, or nullopt for type-agnostic / clobbering commands.
[[nodiscard]] std::optional<KeyType> command_requires_type(
    CommandType type) noexcept {
  if (is_zset_command(type)) {
    return KeyType::Zset;
  }
  if (is_hash_command(type)) {
    return KeyType::Hash;
  }
  if (is_list_command(type)) {
    return KeyType::List;
  }
  if (is_typed_string_command(type)) {
    return KeyType::String;
  }
  return std::nullopt;
}

// Commands whose first argument is a key, so lazy expiration purges it on access.
[[nodiscard]] bool command_has_key_arg(CommandType type) noexcept {
  if (command_requires_type(type).has_value()) {
    return true;  // zset / hash / typed-string commands
  }
  switch (type) {
    case CommandType::set:
    case CommandType::setnx:
    case CommandType::mset:
    case CommandType::mget:
    case CommandType::del:
    case CommandType::exists:
    case CommandType::key_type:
    case CommandType::expire:
    case CommandType::pexpire:
    case CommandType::expireat:
    case CommandType::pexpireat:
    case CommandType::ttl:
    case CommandType::pttl:
    case CommandType::persist:
    case CommandType::expiretime:
    case CommandType::pexpiretime:
    case CommandType::goblin_memory:
    case CommandType::goblin_optimize:
      return true;
    default:
      return false;
  }
}

// The absolute expiry ms for the EXPIRE family: base + amount * unit, in a
// 128-bit intermediate so it can't overflow. nullopt when the result is past the
// 48-bit expiry range (an invalid expire time); a negative result clamps to 0
// (already past -> the key is deleted).
[[nodiscard]] std::optional<std::uint64_t> compute_when_ms(std::uint64_t base_ms,
                                                           long long amount,
                                                           long long unit_ms) {
  const __int128 when = static_cast<__int128>(base_ms) +
                        static_cast<__int128>(amount) * unit_ms;
  if (when < 0) {
    return std::uint64_t{0};
  }
  if (when >= (static_cast<__int128>(1) << 48)) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(when);
}

// INCR/DECR/INCRBY/DECRBY: apply the (possibly negative) delta and reply with the
// new value, or the canonical integer error on a non-integer value or overflow.
void append_incr(std::string& out, Store& store, std::string_view key,
                 long long delta) {
  const auto result = store.incr_by(key, delta);
  if (!result) {
    resp::append_error(out, integer_range_error());
    return;
  }
  resp::append_integer(out, *result);
}

[[nodiscard]] std::optional<long long> parse_ll(std::string_view text) {
  long long value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return value;
}

struct WithscoresChunkAppender {
  std::string& out;
  std::array<std::pair<std::string_view, double>, resp::kWithscoresStreamChunk> chunk{};
  std::size_t count{0};

  void operator()(std::string_view member, double score) {
    chunk[count++] = {member, score};
    if (count == chunk.size()) {
      flush();
    }
  }

  void flush() {
    if (count == 0) {
      return;
    }
    resp::append_bulk_withscores_chunk(out, std::span(chunk.data(), count));
    count = 0;
  }
};

struct WithscoresTextChunkAppender {
  std::string& out;
  std::array<std::pair<std::string_view, std::string_view>, resp::kWithscoresStreamChunk>
      chunk{};
  std::size_t count{0};

  void operator()(std::string_view member, std::string_view score_text) {
    chunk[count++] = {member, score_text};
    if (count == chunk.size()) {
      flush();
    }
  }

  void flush() {
    if (count == 0) {
      return;
    }
    resp::append_bulk_withscores_text_chunk(out, std::span(chunk.data(), count));
    count = 0;
  }
};

void append_range_response(Store& store,
                           const Command& command,
                           bool reverse,
                           std::string& out,
                           CommandExecutionOptions options) {
  long long start = command.range_start;
  long long stop = command.range_stop;
  if (!command.range_indexes_parsed) {
    const auto parsed_start = parse_i64(command.args[1]);
    const auto parsed_stop = parse_i64(command.args[2]);
    if (!parsed_start || !parsed_stop) {
      resp::append_error(out, integer_range_error());
      return;
    }
    start = *parsed_start;
    stop = *parsed_stop;
  }

  const auto key = command.args[0];
  const auto reserve_limit = options.output_reserve_limit;
  const auto with_scores = command.with_scores;

  // Reserve and write the array header in the counted callback so bounds are
  // computed once per range (the prior zrange_size + for_each split doubled
  // find_zset/range_bounds work and regressed WITHSCORES).
  auto append_header = [&out, with_scores, reserve_limit](std::size_t entry_count) {
    const auto bulk_count = with_scores ? entry_count * 2 : entry_count;
    resp::reserve_append_capacity(
        out,
        16 + entry_count * (with_scores ? 48 : 32),
        reserve_limit);
    resp::append_array_header(out, bulk_count);
  };

  if (with_scores) {
    if (store.score_string_cache_enabled()) {
      WithscoresTextChunkAppender appender{out};
      if (reverse) {
        store.zrevrange_score_text_values_for_each_counted(
            key, start, stop, append_header, appender);
      } else {
        store.zrange_score_text_values_for_each_counted(
            key, start, stop, append_header, appender);
      }
      appender.flush();
      return;
    }

    WithscoresChunkAppender appender{out};
    if (reverse) {
      store.zrevrange_values_for_each_counted(key, start, stop, append_header, appender);
    } else {
      store.zrange_values_for_each_counted(key, start, stop, append_header, appender);
    }
    appender.flush();
    return;
  }

  auto append_member = [&out](std::string_view member) {
    resp::append_bulk_string(out, member);
  };
  if (reverse) {
    store.zrevrange_members_for_each_counted(
        key, start, stop, append_header, append_member);
  } else {
    store.zrange_members_for_each_counted(
        key, start, stop, append_header, append_member);
  }
}

}  // namespace

CommandParseResult parse_command(std::span<const std::string_view> fields) {
  if (fields.empty()) {
    return parse_error("ERR empty command");
  }

  Command command;
  command.name = fields.front();
  command.args = fields.subspan(1);

  // Upper-case the name into a fixed 32-byte buffer, then perfect-hash it in O(1).
  // Names longer than the longest command (GOBLIN.TD_LEADERBOARD_RESCORE, 29)
  // cannot match and short-circuit to unknown without touching the buffer.
  const CommandEntry* entry = nullptr;
  if (command.name.size() <= 31) {
    std::array<char, 32> upper{};
    for (std::size_t k = 0; k < command.name.size(); ++k) {
      upper[k] = ascii_upper_char(command.name[k]);
    }
    entry = CommandDispatch::lookup(upper.data(), command.name.size());
  }
  if (entry == nullptr) {
    command.type = CommandType::unknown;
    return {.command = std::move(command)};
  }

  // Arity/setup bodies are identical to the former equals_ci chain; only the
  // selection changed. equals_ci is still used for in-handler argument checks.
  switch (entry->type) {
    case CommandType::ping:
      if (command.args.size() > 1) {
        return parse_error(wrong_arity("ping"));
      }
      command.type = CommandType::ping;
      return {.command = std::move(command)};
    case CommandType::echo:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("echo"));
      }
      command.type = CommandType::echo;
      return {.command = std::move(command)};
    case CommandType::info:
      if (command.args.size() > 1) {  // optional section arg, ignored
        return parse_error(wrong_arity("info"));
      }
      command.type = CommandType::info;
      return {.command = std::move(command)};
    case CommandType::eval:
      if (command.args.size() < 2) {  // script numkeys [key ...] [arg ...]
        return parse_error(wrong_arity("eval"));
      }
      command.type = CommandType::eval;
      return {.command = std::move(command)};
    case CommandType::evalsha:
      if (command.args.size() < 2) {  // sha1 numkeys [key ...] [arg ...]
        return parse_error(wrong_arity("evalsha"));
      }
      command.type = CommandType::evalsha;
      return {.command = std::move(command)};
    case CommandType::script:
      if (command.args.empty()) {  // LOAD | EXISTS | FLUSH ...
        return parse_error(wrong_arity("script"));
      }
      command.type = CommandType::script;
      return {.command = std::move(command)};
    case CommandType::luau_eval:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("luau.eval"));
      }
      command.type = CommandType::luau_eval;
      return {.command = std::move(command)};
    case CommandType::luau_evalsha:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("luau.evalsha"));
      }
      command.type = CommandType::luau_evalsha;
      return {.command = std::move(command)};
    case CommandType::luau_script:
      if (command.args.empty()) {
        return parse_error(wrong_arity("luau.script"));
      }
      command.type = CommandType::luau_script;
      return {.command = std::move(command)};
    case CommandType::wren_eval:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("wren.eval"));
      }
      command.type = CommandType::wren_eval;
      return {.command = std::move(command)};
    case CommandType::wren_evalsha:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("wren.evalsha"));
      }
      command.type = CommandType::wren_evalsha;
      return {.command = std::move(command)};
    case CommandType::wren_script:
      if (command.args.empty()) {
        return parse_error(wrong_arity("wren.script"));
      }
      command.type = CommandType::wren_script;
      return {.command = std::move(command)};
    case CommandType::tcl_eval:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("tcl.eval"));
      }
      command.type = CommandType::tcl_eval;
      return {.command = std::move(command)};
    case CommandType::tcl_evalsha:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("tcl.evalsha"));
      }
      command.type = CommandType::tcl_evalsha;
      return {.command = std::move(command)};
    case CommandType::tcl_script:
      if (command.args.empty()) {
        return parse_error(wrong_arity("tcl.script"));
      }
      command.type = CommandType::tcl_script;
      return {.command = std::move(command)};
    case CommandType::upython_eval:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("upython.eval"));
      }
      command.type = CommandType::upython_eval;
      return {.command = std::move(command)};
    case CommandType::upython_evalsha:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("upython.evalsha"));
      }
      command.type = CommandType::upython_evalsha;
      return {.command = std::move(command)};
    case CommandType::upython_script:
      if (command.args.empty()) {
        return parse_error(wrong_arity("upython.script"));
      }
      command.type = CommandType::upython_script;
      return {.command = std::move(command)};
    case CommandType::quickjs_eval:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("quickjs.eval"));
      }
      command.type = CommandType::quickjs_eval;
      return {.command = std::move(command)};
    case CommandType::quickjs_evalsha:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("quickjs.evalsha"));
      }
      command.type = CommandType::quickjs_evalsha;
      return {.command = std::move(command)};
    case CommandType::quickjs_script:
      if (command.args.empty()) {
        return parse_error(wrong_arity("quickjs.script"));
      }
      command.type = CommandType::quickjs_script;
      return {.command = std::move(command)};
    case CommandType::zadd:
      if (command.args.size() < 3 || (command.args.size() - 1) % 2 != 0) {
        return parse_error(wrong_arity("zadd"));
      }
      command.type = CommandType::zadd;
      return {.command = std::move(command)};
    case CommandType::zcard:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("zcard"));
      }
      command.type = CommandType::zcard;
      return {.command = std::move(command)};
    case CommandType::zrange: {
      if (command.args.size() != 3 && command.args.size() != 4) {
        return parse_error(wrong_arity("zrange"));
      }
      if (command.args.size() == 4) {
        if (!equals_ci(command.args[3], "WITHSCORES")) {
          return parse_error(syntax_error());
        }
        command.with_scores = true;
      }
      if (!parse_range_indexes(command)) {
        return parse_error(integer_range_error());
      }
      command.type = CommandType::zrange;
      return {.command = std::move(command)};
    }
    case CommandType::zrank:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("zrank"));
      }
      command.type = CommandType::zrank;
      return {.command = std::move(command)};
    case CommandType::zrevrange: {
      if (command.args.size() != 3 && command.args.size() != 4) {
        return parse_error(wrong_arity("zrevrange"));
      }
      if (command.args.size() == 4) {
        if (!equals_ci(command.args[3], "WITHSCORES")) {
          return parse_error(syntax_error());
        }
        command.with_scores = true;
      }
      if (!parse_range_indexes(command)) {
        return parse_error(integer_range_error());
      }
      command.type = CommandType::zrevrange;
      return {.command = std::move(command)};
    }
    case CommandType::zrevrank:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("zrevrank"));
      }
      command.type = CommandType::zrevrank;
      return {.command = std::move(command)};
    case CommandType::zrem:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("zrem"));
      }
      command.type = CommandType::zrem;
      return {.command = std::move(command)};
    case CommandType::zremrangebyscore:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("zremrangebyscore"));
      }
      command.type = CommandType::zremrangebyscore;
      return {.command = std::move(command)};
    case CommandType::zscore:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("zscore"));
      }
      command.type = CommandType::zscore;
      return {.command = std::move(command)};
    case CommandType::hset:
      if (command.args.size() < 3 || (command.args.size() - 1) % 2 != 0) {
        return parse_error(wrong_arity("hset"));
      }
      command.type = CommandType::hset;
      return {.command = std::move(command)};
    case CommandType::hsetnx:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("hsetnx"));
      }
      command.type = CommandType::hsetnx;
      return {.command = std::move(command)};
    case CommandType::hget:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("hget"));
      }
      command.type = CommandType::hget;
      return {.command = std::move(command)};
    case CommandType::hmget:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("hmget"));
      }
      command.type = CommandType::hmget;
      return {.command = std::move(command)};
    case CommandType::hdel:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("hdel"));
      }
      command.type = CommandType::hdel;
      return {.command = std::move(command)};
    case CommandType::hgetall:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("hgetall"));
      }
      command.type = CommandType::hgetall;
      return {.command = std::move(command)};
    case CommandType::hkeys:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("hkeys"));
      }
      command.type = CommandType::hkeys;
      return {.command = std::move(command)};
    case CommandType::hvals:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("hvals"));
      }
      command.type = CommandType::hvals;
      return {.command = std::move(command)};
    case CommandType::hlen:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("hlen"));
      }
      command.type = CommandType::hlen;
      return {.command = std::move(command)};
    case CommandType::hexists:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("hexists"));
      }
      command.type = CommandType::hexists;
      return {.command = std::move(command)};
    case CommandType::hstrlen:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("hstrlen"));
      }
      command.type = CommandType::hstrlen;
      return {.command = std::move(command)};
    case CommandType::hincrby:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("hincrby"));
      }
      command.type = CommandType::hincrby;
      return {.command = std::move(command)};
    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::pma_lpush:
    case CommandType::pma_rpush:
    case CommandType::pma_lpushx:
    case CommandType::pma_rpushx:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::pma_lpop:
    case CommandType::pma_rpop:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::llen:
    case CommandType::pma_llen:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::lindex:
    case CommandType::pma_lindex:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::lrange:
    case CommandType::ltrim:
    case CommandType::pma_lrange:
    case CommandType::pma_ltrim:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity(command.name));
      }
      if (!parse_range_indexes(command)) {
        return parse_error(integer_range_error());
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::lset:
    case CommandType::lrem:
    case CommandType::pma_lset:
    case CommandType::pma_lrem:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::linsert:
    case CommandType::pma_linsert:
      if (command.args.size() != 4) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::set:
      if (command.args.size() < 2) {  // options (NX) validated in the handler
        return parse_error(wrong_arity("set"));
      }
      command.type = CommandType::set;
      return {.command = std::move(command)};
    case CommandType::get:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("get"));
      }
      command.type = CommandType::get;
      return {.command = std::move(command)};
    case CommandType::getset:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("getset"));
      }
      command.type = CommandType::getset;
      return {.command = std::move(command)};
    case CommandType::setnx:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("setnx"));
      }
      command.type = CommandType::setnx;
      return {.command = std::move(command)};
    case CommandType::getdel:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("getdel"));
      }
      command.type = CommandType::getdel;
      return {.command = std::move(command)};
    case CommandType::strlen:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("strlen"));
      }
      command.type = CommandType::strlen;
      return {.command = std::move(command)};
    case CommandType::append:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("append"));
      }
      command.type = CommandType::append;
      return {.command = std::move(command)};
    case CommandType::incr:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("incr"));
      }
      command.type = CommandType::incr;
      return {.command = std::move(command)};
    case CommandType::decr:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("decr"));
      }
      command.type = CommandType::decr;
      return {.command = std::move(command)};
    case CommandType::incrby:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("incrby"));
      }
      command.type = CommandType::incrby;
      return {.command = std::move(command)};
    case CommandType::decrby:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("decrby"));
      }
      command.type = CommandType::decrby;
      return {.command = std::move(command)};
    case CommandType::incrbyfloat:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("incrbyfloat"));
      }
      command.type = CommandType::incrbyfloat;
      return {.command = std::move(command)};
    case CommandType::getrange:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("getrange"));
      }
      command.type = CommandType::getrange;
      return {.command = std::move(command)};
    case CommandType::setrange:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("setrange"));
      }
      command.type = CommandType::setrange;
      return {.command = std::move(command)};
    case CommandType::mset:
      if (command.args.size() < 2 || command.args.size() % 2 != 0) {
        return parse_error(wrong_arity("mset"));
      }
      command.type = CommandType::mset;
      return {.command = std::move(command)};
    case CommandType::mget:
      if (command.args.empty()) {
        return parse_error(wrong_arity("mget"));
      }
      command.type = CommandType::mget;
      return {.command = std::move(command)};
    case CommandType::del:
      if (command.args.empty()) {
        return parse_error(wrong_arity("del"));
      }
      command.type = CommandType::del;
      return {.command = std::move(command)};
    case CommandType::exists:
      if (command.args.empty()) {
        return parse_error(wrong_arity("exists"));
      }
      command.type = CommandType::exists;
      return {.command = std::move(command)};
    case CommandType::key_type:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("type"));
      }
      command.type = CommandType::key_type;
      return {.command = std::move(command)};
    case CommandType::expire:
    case CommandType::pexpire:
    case CommandType::expireat:
    case CommandType::pexpireat:
      if (command.args.size() < 2) {  // key, amount, then optional NX/XX/GT/LT
        return parse_error(wrong_arity("expire"));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::ttl:
    case CommandType::pttl:
    case CommandType::persist:
    case CommandType::expiretime:
    case CommandType::pexpiretime:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("ttl"));
      }
      command.type = entry->type;
      return {.command = std::move(command)};
    case CommandType::goblin_memory:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("goblin.memory"));
      }
      command.type = CommandType::goblin_memory;
      return {.command = std::move(command)};
    case CommandType::goblin_optimize:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity("goblin.optimize"));
      }
      command.type = CommandType::goblin_optimize;
      return {.command = std::move(command)};
    case CommandType::goblin_save:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity("goblin.save"));
      }
      command.type = CommandType::goblin_save;
      return {.command = std::move(command)};
    case CommandType::goblin_load:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("goblin.load"));
      }
      command.type = CommandType::goblin_load;
      return {.command = std::move(command)};
    case CommandType::goblin_cad:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("goblin.cad"));
      }
      command.type = CommandType::goblin_cad;
      return {.command = std::move(command)};
    case CommandType::goblin_caexpire:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("goblin.caexpire"));
      }
      command.type = CommandType::goblin_caexpire;
      return {.command = std::move(command)};
    case CommandType::goblin_cas:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("goblin.cas"));
      }
      command.type = CommandType::goblin_cas;
      return {.command = std::move(command)};
    case CommandType::goblin_td_leaderboard_rescore:
      if (command.args.size() != 5) {
        return parse_error(wrong_arity("goblin.td_leaderboard_rescore"));
      }
      command.type = CommandType::goblin_td_leaderboard_rescore;
      return {.command = std::move(command)};
    case CommandType::goblin_increx:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("goblin.increx"));
      }
      command.type = CommandType::goblin_increx;
      return {.command = std::move(command)};
    case CommandType::goblin_zwindow:
      if (command.args.size() != 5) {
        return parse_error(wrong_arity("goblin.zwindow"));
      }
      command.type = CommandType::goblin_zwindow;
      return {.command = std::move(command)};
    case CommandType::goblin_incrbound:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("goblin.incrbound"));
      }
      command.type = CommandType::goblin_incrbound;
      return {.command = std::move(command)};
    case CommandType::goblin_decrpos:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("goblin.decrpos"));
      }
      command.type = CommandType::goblin_decrpos;
      return {.command = std::move(command)};
    case CommandType::goblin_hcad:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("goblin.hcad"));
      }
      command.type = CommandType::goblin_hcad;
      return {.command = std::move(command)};
    case CommandType::goblin_hsetgt:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("goblin.hsetgt"));
      }
      command.type = CommandType::goblin_hsetgt;
      return {.command = std::move(command)};
    case CommandType::goblin_claim:
      if (command.args.size() != 4) {
        return parse_error(wrong_arity("goblin.claim"));
      }
      command.type = CommandType::goblin_claim;
      return {.command = std::move(command)};
    case CommandType::unknown:
      break;  // never returned by the perfect hash; keeps the switch exhaustive
  }
  command.type = CommandType::unknown;
  return {.command = std::move(command)};
}

// Public accessor for the INFO text so the SBE dispatch can reply it without
// duplicating build_info_string (which stays an internal helper here).
std::string render_server_info(const Store& store) { return build_info_string(store); }

// GOBLIN.MEMORY's flat [name, value, ...] fields for a zset or hash key (nullopt if
// the key is neither), so the SBE dispatch can shape them into a map reply and the
// RESP path into an array -- one field list, two encodings.
std::optional<std::vector<std::string>> goblin_memory_fields(const Store& store,
                                                             std::string_view key) {
  if (const auto z = store.zset_memory_stats(key)) return memory_stats_fields(*z);
  if (const auto h = store.hash_memory_stats(key)) return hash_memory_stats_fields(*h);
  if (const auto l = store.list_memory_stats(key)) return list_memory_stats_fields(*l);
  return std::nullopt;
}

void execute_command_into(Store& store,
                          const Command& command,
                          std::string& out,
                          CommandExecutionOptions options) {
  const auto type =
      resolve_list_command(command.type, store.list_implementation());
  // Lazy expiration: a key whose TTL has already passed is deleted on first
  // access, before the type gate or the command sees it. Only the first-key
  // argument is handled here (multi-key readers purge their extra keys); gated on
  // a non-empty TTL set so a server with no expirations pays nothing.
  if (!command.args.empty() && command_has_key_arg(type) &&
      !store.ttl_empty()) {
    (void)store.purge_if_expired(command.args[0], store.now_ms());
  }

  // WRONGTYPE: a key holds at most one type (one unified namespace). A command
  // that operates on a specific type is rejected when the key already holds a
  // different one. SET / SETNX / MSET (clobber or create), MGET / DEL / EXISTS /
  // TYPE (type-agnostic), scripts, and the introspection GOBLIN.* commands are
  // exempt; GOBLIN.CAD / GOBLIN.CAEXPIRE / GOBLIN.CAS are not (they read the
  // value like GET).
  if (!command.args.empty()) {
    if (const auto required = command_requires_type(type)) {
      if (const auto actual = store.key_type(command.args[0]);
          actual.has_value() && *actual != *required) {
        resp::append_error(out, kWrongType);
        return;
      }
    }
  }

  switch (type) {
    case CommandType::ping:
      if (command.args.empty()) {
        resp::append_simple_string(out, "PONG");
      } else {
        resp::append_bulk_string(out, command.args.front());
      }
      return;
    case CommandType::echo:
      resp::append_bulk_string(out, command.args.front());
      return;
    case CommandType::info:
      resp::append_bulk_string(out, build_info_string(store));
      return;

    case CommandType::eval:
      if (options.script_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.script_engine->eval(command.args, out);
      }
      return;

    case CommandType::evalsha:
      if (options.script_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.script_engine->eval_sha(command.args, out);
      }
      return;

    case CommandType::script:
      if (options.script_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.script_engine->script(command.args, out);
      }
      return;

    case CommandType::luau_eval:
      if (options.luau_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.luau_engine->eval(command.args, out);
      }
      return;

    case CommandType::luau_evalsha:
      if (options.luau_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.luau_engine->eval_sha(command.args, out);
      }
      return;

    case CommandType::luau_script:
      if (options.luau_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.luau_engine->script(command.args, out);
      }
      return;

    case CommandType::wren_eval:
      if (options.wren_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.wren_engine->eval(command.args, out);
      }
      return;

    case CommandType::wren_evalsha:
      if (options.wren_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.wren_engine->eval_sha(command.args, out);
      }
      return;

    case CommandType::wren_script:
      if (options.wren_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.wren_engine->script(command.args, out);
      }
      return;

    case CommandType::tcl_eval:
      if (options.tcl_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.tcl_engine->eval(command.args, out);
      }
      return;

    case CommandType::tcl_evalsha:
      if (options.tcl_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.tcl_engine->eval_sha(command.args, out);
      }
      return;

    case CommandType::tcl_script:
      if (options.tcl_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.tcl_engine->script(command.args, out);
      }
      return;

    case CommandType::upython_eval:
      if (options.upython_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.upython_engine->eval(command.args, out);
      }
      return;

    case CommandType::upython_evalsha:
      if (options.upython_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.upython_engine->eval_sha(command.args, out);
      }
      return;

    case CommandType::upython_script:
      if (options.upython_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.upython_engine->script(command.args, out);
      }
      return;

    case CommandType::quickjs_eval:
      if (options.quickjs_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.quickjs_engine->eval(command.args, out);
      }
      return;

    case CommandType::quickjs_evalsha:
      if (options.quickjs_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.quickjs_engine->eval_sha(command.args, out);
      }
      return;

    case CommandType::quickjs_script:
      if (options.quickjs_engine == nullptr) {
        resp::append_error(out, "ERR This Redis command is not available");
      } else {
        options.quickjs_engine->script(command.args, out);
      }
      return;

    case CommandType::zadd: {
      const auto& key = command.args[0];
      long long added = 0;
      for (std::size_t i = 1; i < command.args.size(); i += 2) {
        const auto score = parse_score(command.args[i]);
        if (!score) {
          resp::append_error(out, "ERR value is not a valid float");
          return;
        }
        added += store.zadd(key, *score, command.args[i + 1]);
      }
      resp::append_integer(out, added);
      return;
    }

    case CommandType::zcard:
      resp::append_integer(out, store.zcard(command.args[0]));
      return;

    case CommandType::zrange:
      append_range_response(store, command, false, out, options);
      return;

    case CommandType::zrank: {
      const auto rank = store.zrank(command.args[0], command.args[1]);
      if (!rank) {
        resp::append_null_bulk_string(out);
      } else {
        resp::append_integer(out, static_cast<long long>(*rank));
      }
      return;
    }

    case CommandType::zrevrange:
      append_range_response(store, command, true, out, options);
      return;

    case CommandType::zrevrank: {
      const auto rank = store.zrevrank(command.args[0], command.args[1]);
      if (!rank) {
        resp::append_null_bulk_string(out);
      } else {
        resp::append_integer(out, static_cast<long long>(*rank));
      }
      return;
    }

    case CommandType::zrem:
      resp::append_integer(out, store.zrem(
          command.args[0], std::span<const std::string_view>(command.args.data() + 1,
                                                             command.args.size() - 1)));
      return;

    case CommandType::zremrangebyscore: {
      const auto lo = parse_score_bound(command.args[1]);
      const auto hi = parse_score_bound(command.args[2]);
      if (!lo || !hi) {
        resp::append_error(out, "ERR min or max is not a float");
        return;
      }
      resp::append_integer(
          out, store.zremrangebyscore(command.args[0], lo->value, lo->exclusive,
                                      hi->value, hi->exclusive));
      return;
    }

    case CommandType::zscore: {
      const auto score = store.zscore(command.args[0], command.args[1]);
      if (!score) {
        resp::append_null_bulk_string(out);
      } else {
        resp::append_bulk_finite_double(out, *score);
      }
      return;
    }

    case CommandType::hset: {
      const auto& key = command.args[0];
      // Common case: HSET k f v -- skip the multi-field vector.
      if (command.args.size() == 3) {
        resp::append_integer(
            out, store.hset(key, command.args[1], command.args[2]));
        return;
      }
      // Multi-field: one keyspace lookup, reserve, then set each pair.
      static thread_local std::vector<std::pair<std::string_view, std::string_view>>
          pairs;
      pairs.clear();
      pairs.reserve((command.args.size() - 1) / 2);
      for (std::size_t i = 1; i + 1 < command.args.size(); i += 2) {
        pairs.emplace_back(command.args[i], command.args[i + 1]);
      }
      resp::append_integer(out, store.hset_many(key, pairs));
      return;
    }

    case CommandType::hsetnx:
      resp::append_integer(
          out, store.hsetnx(command.args[0], command.args[1], command.args[2]));
      return;

    case CommandType::hget: {
      const auto value = store.hget(command.args[0], command.args[1]);
      if (!value) {
        resp::append_null_bulk_string(out);
      } else {
        resp::append_bulk_string(out, *value);
      }
      return;
    }

    case CommandType::hmget: {
      resp::append_array_header(out, command.args.size() - 1);
      for (std::size_t i = 1; i < command.args.size(); ++i) {
        const auto value = store.hget(command.args[0], command.args[i]);
        if (!value) {
          resp::append_null_bulk_string(out);
        } else {
          resp::append_bulk_string(out, *value);
        }
      }
      return;
    }

    case CommandType::hdel: {
      const auto fields = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      resp::append_integer(
          out, static_cast<long long>(store.hdel_many(command.args[0], fields)));
      return;
    }

    case CommandType::hgetall: {
      resp::append_array_header(out, store.hlen(command.args[0]) * 2);
      store.hash_for_each(command.args[0],
                          [&out](std::string_view field, std::string_view value) {
                            resp::append_bulk_string(out, field);
                            resp::append_bulk_string(out, value);
                          });
      return;
    }

    case CommandType::hkeys: {
      resp::append_array_header(out, store.hlen(command.args[0]));
      store.hash_for_each(command.args[0],
                          [&out](std::string_view field, std::string_view) {
                            resp::append_bulk_string(out, field);
                          });
      return;
    }

    case CommandType::hvals: {
      resp::append_array_header(out, store.hlen(command.args[0]));
      store.hash_for_each(command.args[0],
                          [&out](std::string_view, std::string_view value) {
                            resp::append_bulk_string(out, value);
                          });
      return;
    }

    case CommandType::hlen:
      resp::append_integer(out, static_cast<long long>(store.hlen(command.args[0])));
      return;

    case CommandType::hexists:
      resp::append_integer(
          out, store.hexists(command.args[0], command.args[1]) ? 1 : 0);
      return;

    case CommandType::hstrlen: {
      const auto len = store.hstrlen(command.args[0], command.args[1]);
      resp::append_integer(out, static_cast<long long>(len.value_or(0)));
      return;
    }

    case CommandType::hincrby: {
      const auto delta = parse_ll(command.args[2]);
      if (!delta) {
        resp::append_error(out, "ERR value is not an integer or out of range");
        return;
      }
      const auto result = store.hincrby(command.args[0], command.args[1], *delta);
      if (!result) {
        resp::append_error(
            out, "ERR hash value is not an integer or out of range");
      } else {
        resp::append_integer(out, *result);
      }
      return;
    }

    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::pma_lpush:
    case CommandType::pma_rpush:
    case CommandType::pma_lpushx:
    case CommandType::pma_rpushx: {
      for (std::size_t index = 1; index < command.args.size(); ++index) {
        if (command.args[index].size() > Store::max_value_bytes()) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      const auto values = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      const bool front = type == CommandType::pma_lpush ||
                         type == CommandType::pma_lpushx;
      const bool only_if_exists = type == CommandType::pma_lpushx ||
                                  type == CommandType::pma_rpushx;
      const auto length = front
                              ? store.lpush(command.args[0], values,
                                            only_if_exists)
                              : store.rpush(command.args[0], values,
                                            only_if_exists);
      resp::append_integer(out, length);
      return;
    }

    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::pma_lpop:
    case CommandType::pma_rpop: {
      const bool front = type == CommandType::pma_lpop;
      if (command.args.size() == 1) {
        const auto value = front ? store.lpop(command.args[0])
                                 : store.rpop(command.args[0]);
        if (value) {
          resp::append_bulk_string(out, *value);
        } else {
          resp::append_null_bulk_string(out);
        }
        return;
      }
      const auto count = parse_ll(command.args[1]);
      if (!count) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (*count < 0) {
        resp::append_error(out, "ERR value is out of range, must be positive");
        return;
      }
      if (!store.exists(command.args[0])) {
        resp::append_null_bulk_string(out);
        return;
      }
      const auto values = front
                              ? store.lpop(command.args[0],
                                           static_cast<std::size_t>(*count))
                              : store.rpop(command.args[0],
                                           static_cast<std::size_t>(*count));
      resp::append_array_header(out, values.size());
      for (const auto& value : values) {
        resp::append_bulk_string(out, value);
      }
      return;
    }

    case CommandType::llen:
    case CommandType::pma_llen:
      resp::append_integer(out,
                           static_cast<long long>(store.llen(command.args[0])));
      return;

    case CommandType::lindex:
    case CommandType::pma_lindex: {
      const auto index = parse_ll(command.args[1]);
      if (!index) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto value = store.lindex(command.args[0], *index);
      if (value) {
        resp::append_bulk_string(out, *value);
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    }

    case CommandType::lrange:
    case CommandType::pma_lrange: {
      const auto values = store.lrange(command.args[0], command.range_start,
                                       command.range_stop);
      resp::append_array_header(out, values.size());
      for (const auto value : values) {
        resp::append_bulk_string(out, value);
      }
      return;
    }

    case CommandType::lset:
    case CommandType::pma_lset: {
      const auto index = parse_ll(command.args[1]);
      if (!index) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (command.args[2].size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      switch (store.lset(command.args[0], *index, command.args[2])) {
        case Store::ListSetResult::Stored:
          resp::append_simple_string(out, "OK");
          return;
        case Store::ListSetResult::MissingKey:
          resp::append_error(out, "ERR no such key");
          return;
        case Store::ListSetResult::OutOfRange:
          resp::append_error(out, "ERR index out of range");
          return;
      }
      return;
    }

    case CommandType::ltrim:
    case CommandType::pma_ltrim:
      store.ltrim(command.args[0], command.range_start, command.range_stop);
      resp::append_simple_string(out, "OK");
      return;

    case CommandType::lrem:
    case CommandType::pma_lrem: {
      const auto count = parse_ll(command.args[1]);
      if (!count) {
        resp::append_error(out, integer_range_error());
        return;
      }
      resp::append_integer(
          out, static_cast<long long>(
                   store.lrem(command.args[0], *count, command.args[2])));
      return;
    }

    case CommandType::linsert:
    case CommandType::pma_linsert: {
      bool before = false;
      if (equals_ci(command.args[1], "BEFORE")) {
        before = true;
      } else if (!equals_ci(command.args[1], "AFTER")) {
        resp::append_error(out, syntax_error());
        return;
      }
      if (command.args[3].size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(
          out, store.linsert(command.args[0], before, command.args[2],
                             command.args[3]));
      return;
    }

    case CommandType::set: {
      // SET key value [NX | XX] [GET]
      //     [EX s | PX ms | EXAT ts | PXAT ms | KEEPTTL].
      const auto& key = command.args[0];
      const auto& value = command.args[1];
      if (value.size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      bool nx = false;
      bool xx = false;
      bool keepttl = false;
      bool want_get = false;
      std::optional<std::uint64_t> expire_when;
      const auto now = store.now_ms();
      for (std::size_t i = 2; i < command.args.size(); ++i) {
        const auto& opt = command.args[i];
        if (equals_ci(opt, "NX")) {
          nx = true;
        } else if (equals_ci(opt, "XX")) {
          xx = true;
        } else if (equals_ci(opt, "GET")) {
          want_get = true;
        } else if (equals_ci(opt, "KEEPTTL")) {
          keepttl = true;
        } else if (equals_ci(opt, "EX") || equals_ci(opt, "PX") ||
                   equals_ci(opt, "EXAT") || equals_ci(opt, "PXAT")) {
          if (i + 1 >= command.args.size()) {
            resp::append_error(out, syntax_error());
            return;
          }
          const auto amount = parse_ll(command.args[++i]);
          if (!amount) {
            resp::append_error(out, integer_range_error());
            return;
          }
          if (equals_ci(opt, "EX")) {
            expire_when = compute_when_ms(now, *amount, 1000);
          } else if (equals_ci(opt, "PX")) {
            expire_when = compute_when_ms(now, *amount, 1);
          } else if (equals_ci(opt, "EXAT")) {
            expire_when = compute_when_ms(0, *amount, 1000);
          } else {  // PXAT
            expire_when = compute_when_ms(0, *amount, 1);
          }
          if (!expire_when) {
            resp::append_error(out, "ERR invalid expire time in 'set' command");
            return;
          }
        } else {
          resp::append_error(out, syntax_error());
          return;
        }
      }
      if ((nx && xx) || (keepttl && expire_when)) {
        resp::append_error(out, syntax_error());
        return;
      }

      const bool exists = store.exists(key);
      // SET ... GET reports the old value, so a non-string key is WRONGTYPE and
      // the whole command is aborted (nothing is set).
      if (want_get && exists && !store.key_is_string(key)) {
        resp::append_error(out, kWrongType);
        return;
      }
      const bool condition_met = !(nx && exists) && !(xx && !exists);

      std::optional<std::string> old_value;  // captured before the overwrite
      if (want_get) {
        if (const auto current = store.get(key)) {
          old_value.emplace();
          old_value->reserve(current->size());
          old_value->append(current->head);
          old_value->append(current->tail);
        }
      }
      if (condition_met) {
        if (keepttl) {
          store.set_keep_ttl(key, value);
        } else {
          store.set(key, value);  // clears any existing TTL
        }
        if (expire_when) {
          (void)store.expire_at_ms(key, *expire_when, now);
        }
      }

      if (want_get) {
        if (old_value) {
          resp::append_bulk_string(out, *old_value);
        } else {
          resp::append_null_bulk_string(out);
        }
      } else if (condition_met) {
        resp::append_simple_string(out, "OK");
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    }
    case CommandType::get: {
      const auto value = store.get(command.args[0]);
      if (!value) {
        resp::append_null_bulk_string(out);
      } else if (value->tail.empty()) {  // fully inline -> zero-copy
        resp::append_bulk_string(out, value->head);
      } else {
        std::string joined;
        joined.reserve(value->size());
        joined.append(value->head);
        joined.append(value->tail);
        resp::append_bulk_string(out, joined);
      }
      return;
    }
    case CommandType::getset: {
      const auto& value = command.args[1];
      if (value.size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      const auto previous = store.get_set(command.args[0], value);
      if (previous) {
        resp::append_bulk_string(out, *previous);
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    }
    case CommandType::setnx: {
      const auto& value = command.args[1];
      if (value.size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(out, store.set_nx(command.args[0], value) ? 1 : 0);
      return;
    }
    case CommandType::getdel: {
      const auto previous = store.get_del(command.args[0]);
      if (previous) {
        resp::append_bulk_string(out, *previous);
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    }
    case CommandType::strlen: {
      const auto len = store.strlen(command.args[0]);
      resp::append_integer(out, len ? static_cast<long long>(*len) : 0);
      return;
    }
    case CommandType::append: {
      const auto& value = command.args[1];
      const auto current = store.strlen(command.args[0]).value_or(0);
      if (current + value.size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(
          out, static_cast<long long>(store.append(command.args[0], value)));
      return;
    }
    case CommandType::incr:
      append_incr(out, store, command.args[0], 1);
      return;
    case CommandType::decr:
      append_incr(out, store, command.args[0], -1);
      return;
    case CommandType::incrby: {
      const auto delta = parse_ll(command.args[1]);
      if (!delta) {
        resp::append_error(out, integer_range_error());
        return;
      }
      append_incr(out, store, command.args[0], *delta);
      return;
    }
    case CommandType::decrby: {
      const auto delta = parse_ll(command.args[1]);
      if (!delta || *delta == std::numeric_limits<long long>::min()) {
        // Negating LLONG_MIN overflows, so DECRBY rejects it as out of range.
        resp::append_error(out, integer_range_error());
        return;
      }
      append_incr(out, store, command.args[0], -*delta);
      return;
    }
    case CommandType::incrbyfloat: {
      const auto delta = parse_score(command.args[1]);
      if (!delta) {
        resp::append_error(out, "ERR value is not a valid float");
        return;
      }
      const auto result = store.incr_by_float(command.args[0], *delta);
      if (!result) {
        resp::append_error(out, "ERR value is not a valid float");
        return;
      }
      resp::append_bulk_string(out, *result);
      return;
    }
    case CommandType::getrange: {
      const auto start = parse_ll(command.args[1]);
      const auto end = parse_ll(command.args[2]);
      if (!start || !end) {
        resp::append_error(out, integer_range_error());
        return;
      }
      resp::append_bulk_string(out,
                               store.getrange(command.args[0], *start, *end));
      return;
    }
    case CommandType::setrange: {
      const auto offset = parse_ll(command.args[1]);
      if (!offset) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (*offset < 0) {
        resp::append_error(out, "ERR offset is out of range");
        return;
      }
      const auto len = store.setrange(
          command.args[0], static_cast<std::size_t>(*offset), command.args[2]);
      if (!len) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(out, static_cast<long long>(*len));
      return;
    }
    case CommandType::mset: {
      for (std::size_t i = 0; i + 1 < command.args.size(); i += 2) {
        if (command.args[i + 1].size() > Store::max_value_bytes()) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      for (std::size_t i = 0; i + 1 < command.args.size(); i += 2) {
        store.set(command.args[i], command.args[i + 1]);
      }
      resp::append_simple_string(out, "OK");
      return;
    }
    case CommandType::mget: {
      const bool has_ttl = !store.ttl_empty();
      const auto now = has_ttl ? store.now_ms() : std::uint64_t{0};
      resp::append_array_header(out, command.args.size());
      for (const auto& key : command.args) {
        if (has_ttl) {
          (void)store.purge_if_expired(key, now);
        }
        const auto value = store.get(key);  // nil for a missing or non-string key
        if (!value) {
          resp::append_null_bulk_string(out);
        } else if (value->tail.empty()) {
          resp::append_bulk_string(out, value->head);
        } else {
          std::string joined;
          joined.reserve(value->size());
          joined.append(value->head);
          joined.append(value->tail);
          resp::append_bulk_string(out, joined);
        }
      }
      return;
    }
    case CommandType::del: {
      long long removed = 0;
      for (const auto& key : command.args) {
        removed += store.del(key) ? 1 : 0;
      }
      resp::append_integer(out, removed);
      return;
    }
    case CommandType::exists: {
      const bool has_ttl = !store.ttl_empty();
      const auto now = has_ttl ? store.now_ms() : std::uint64_t{0};
      long long count = 0;
      for (const auto& key : command.args) {
        if (has_ttl) {
          (void)store.purge_if_expired(key, now);
        }
        count += store.exists(key) ? 1 : 0;
      }
      resp::append_integer(out, count);
      return;
    }
    case CommandType::key_type: {
      const auto type = store.key_type(command.args[0]);
      std::string_view name = "none";
      if (type) {
        switch (*type) {
          case KeyType::String:
            name = "string";
            break;
          case KeyType::Zset:
            name = "zset";
            break;
          case KeyType::Hash:
            name = "hash";
            break;
          case KeyType::List:
            name = "list";
            break;
        }
      }
      resp::append_simple_string(out, name);
      return;
    }
    case CommandType::expire:
    case CommandType::pexpire:
    case CommandType::expireat:
    case CommandType::pexpireat: {
      const auto amount = parse_ll(command.args[1]);
      if (!amount) {
        resp::append_error(out, integer_range_error());
        return;
      }
      unsigned flags = 0;
      for (std::size_t i = 2; i < command.args.size(); ++i) {
        const auto& opt = command.args[i];
        if (equals_ci(opt, "NX")) {
          flags |= ExpireFlag::kNx;
        } else if (equals_ci(opt, "XX")) {
          flags |= ExpireFlag::kXx;
        } else if (equals_ci(opt, "GT")) {
          flags |= ExpireFlag::kGt;
        } else if (equals_ci(opt, "LT")) {
          flags |= ExpireFlag::kLt;
        } else {
          resp::append_error(out,
                             "ERR Unsupported option " + std::string(opt));
          return;
        }
      }
      if ((flags & ExpireFlag::kNx) &&
          (flags & (ExpireFlag::kXx | ExpireFlag::kGt | ExpireFlag::kLt))) {
        resp::append_error(
            out,
            "ERR NX and XX, GT or LT options at the same time are not compatible");
        return;
      }
      if ((flags & ExpireFlag::kGt) && (flags & ExpireFlag::kLt)) {
        resp::append_error(
            out, "ERR GT and LT options at the same time are not compatible");
        return;
      }
      const auto now = store.now_ms();
      std::optional<std::uint64_t> when;
      switch (type) {
        case CommandType::expire:
          when = compute_when_ms(now, *amount, 1000);
          break;
        case CommandType::pexpire:
          when = compute_when_ms(now, *amount, 1);
          break;
        case CommandType::expireat:
          when = compute_when_ms(0, *amount, 1000);
          break;
        default:  // pexpireat
          when = compute_when_ms(0, *amount, 1);
          break;
      }
      if (!when) {
        resp::append_error(out, "ERR invalid expire time");
        return;
      }
      resp::append_integer(
          out, store.expire_at_ms(command.args[0], *when, now, flags) ? 1 : 0);
      return;
    }
    case CommandType::ttl:
    case CommandType::pttl: {
      const auto ms = store.pttl_ms(command.args[0], store.now_ms());
      if (type == CommandType::pttl || ms < 0) {
        resp::append_integer(out, ms);
      } else {
        resp::append_integer(out, (ms + 500) / 1000);  // TTL rounds to seconds
      }
      return;
    }
    case CommandType::persist:
      resp::append_integer(out, store.persist(command.args[0]) ? 1 : 0);
      return;
    case CommandType::expiretime:
    case CommandType::pexpiretime: {
      const auto ms = store.expiretime_ms(command.args[0]);
      if (type == CommandType::pexpiretime || ms < 0) {
        resp::append_integer(out, ms);
      } else {
        resp::append_integer(out, ms / 1000);  // EXPIRETIME in seconds
      }
      return;
    }

    case CommandType::goblin_memory: {
      if (const auto zstats = store.zset_memory_stats(command.args[0]); zstats) {
        out.append(memory_stats_response(*zstats));
      } else if (const auto hstats = store.hash_memory_stats(command.args[0]);
                 hstats) {
        out.append(hash_memory_stats_response(*hstats));
      } else if (const auto lstats = store.list_memory_stats(command.args[0]);
                 lstats) {
        out.append(list_memory_stats_response(*lstats));
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    }

    case CommandType::goblin_optimize: {
      double density = kDefaultMemberIndexDensity;
      if (command.args.size() == 2) {
        const auto parsed = parse_score(command.args[1]);
        if (!parsed || *parsed <= 0.0 || *parsed > 1.0) {
          resp::append_error(out, "ERR packing density must be in (0, 1]");
          return;
        }
        density = *parsed;
      }
      const auto reclaimed = store.optimize(command.args[0], density);
      if (!reclaimed) {
        resp::append_null_bulk_string(out);
      } else {
        resp::append_integer(out, static_cast<long long>(*reclaimed));
      }
      return;
    }

    case CommandType::goblin_cad: {
      // Compare-and-delete: the Redlock unlock idiom (GET; if it equals the
      // expected token, DEL) as one native atomic op, no interpreter. Replies
      // with the number of keys deleted -- 1 on a match, 0 otherwise -- so it is
      // a drop-in for the script's `return redis.call("del", KEYS[1])` / `0`.
      const bool deleted =
          store.compare_and_delete(command.args[0], command.args[1]);
      resp::append_integer(out, deleted ? 1 : 0);
      return;
    }

    case CommandType::goblin_caexpire: {
      // Compare-and-expire (renew): the Redlock/Redisson watchdog idiom (GET; if
      // it equals the expected token, PEXPIRE by `ms`) as one native atomic op.
      // Replies with the PEXPIRE result -- 1 when the TTL was (re)set, 0 when the
      // token did not match -- so it is a drop-in for the script's
      // `return redis.call("pexpire", KEYS[1], ARGV[2])` / `0`.
      const auto ms = parse_ll(command.args[2]);
      if (!ms) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto now = store.now_ms();
      const auto when = compute_when_ms(now, *ms, 1);
      if (!when) {
        resp::append_error(out, "ERR invalid expire time");
        return;
      }
      const bool renewed =
          store.compare_and_expire(command.args[0], command.args[1], *when, now);
      resp::append_integer(out, renewed ? 1 : 0);
      return;
    }

    case CommandType::goblin_cas: {
      // Compare-and-set: the check-and-swap idiom (GET; if it equals the expected
      // token, SET the new value with KEEPTTL) as one native atomic op. KEEPTTL
      // is the point -- a bare SET would clear the key's expiry, a bug shipped
      // constantly; GOBLIN.CAS preserves it. Replies +OK on a swap (what SET
      // returns) and 0 when the token did not match -- a drop-in for the script's
      // `return redis.call("set", KEYS[1], ARGV[2], "KEEPTTL")` / `0`.
      const auto& new_value = command.args[2];
      if (new_value.size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      const bool swapped =
          store.compare_and_set(command.args[0], command.args[1], new_value);
      if (swapped) {
        resp::append_simple_string(out, "OK");
      } else {
        resp::append_integer(out, 0);
      }
      return;
    }

    case CommandType::goblin_td_leaderboard_rescore: {
      // Time-decay leaderboard rescore: read a zset whose score is each member's
      // last-activity timestamp, recompute a recency weight, and return the top k
      // by that weight -- the whole-zset rescore idiom scripted for real
      // leaderboards, done natively (one pass, no interpreter, no ZRANGE copy).
      //   LINEAR: 1 / (1 + age/hl)   (hyperbolic falloff, no transcendental)
      //   EXP:    0.5 ^ (age/hl)     (true half-life decay)
      //   STEP:   1 inside the [now-hl, now] window, else 0
      const auto now = parse_score(command.args[1]);
      const auto hl = parse_score(command.args[2]);
      if (!now || !hl) {
        resp::append_error(out, "ERR value is not a valid float");
        return;
      }
      const auto k_parsed = parse_ll(command.args[3]);
      if (!k_parsed) {
        resp::append_error(out, integer_range_error());
        return;
      }
      enum class Mode { kLinear, kExp, kStep } mode;
      if (equals_ci(command.args[4], "LINEAR")) {
        mode = Mode::kLinear;
      } else if (equals_ci(command.args[4], "EXP")) {
        mode = Mode::kExp;
      } else if (equals_ci(command.args[4], "STEP")) {
        mode = Mode::kStep;
      } else {
        resp::append_error(out, "ERR mode must be LINEAR, EXP or STEP");
        return;
      }

      const double now_v = *now;
      const double inv = 1.0 / *hl;         // mirrors the script's `local inv`
      const double cutoff = now_v - *hl;    // STEP window edge
      const std::size_t k =
          *k_parsed > 0 ? static_cast<std::size_t>(*k_parsed) : 0;

      // Bounded top-k kept sorted descending, by the same insertion sort as the
      // script (so ties break by iteration = ZRANGE order, which matters for STEP
      // where many weights are equal).
      struct Entry {
        std::string name;
        double score;
      };
      std::vector<Entry> best;
      best.reserve(std::min<std::size_t>(k, 4096));
      const auto push = [&best, k](std::string_view name, double s) {
        std::size_t j;
        if (best.size() < k) {
          best.push_back({std::string(name), s});
          j = best.size() - 1;
        } else if (k > 0 && s > best[k - 1].score) {
          best[k - 1] = {std::string(name), s};
          j = k - 1;
        } else {
          return;
        }
        while (j > 0 && best[j].score > best[j - 1].score) {
          std::swap(best[j], best[j - 1]);
          --j;
        }
      };

      if (k > 0) {
        store.for_each_zset_entry(
            command.args[0], [&](std::string_view member, double score) {
              const double age = now_v - score;
              double decayed;
              switch (mode) {
                case Mode::kLinear:
                  decayed = 1.0 / (1.0 + age * inv);
                  break;
                case Mode::kExp:
                  decayed = std::pow(0.5, age * inv);
                  break;
                default:  // kStep
                  decayed = score >= cutoff ? 1.0 : 0.0;
                  break;
              }
              push(member, decayed);
            });
      }

      resp::append_array_header(out, best.size() * 2);
      char buf[32];
      for (const auto& e : best) {
        resp::append_bulk_string(out, e.name);
        const int len = std::snprintf(buf, sizeof(buf), "%.14g", e.score);
        resp::append_bulk_string(out,
                                 std::string_view(buf, static_cast<std::size_t>(len)));
      }
      return;
    }

    case CommandType::goblin_increx: {
      // INCR with expiry-on-first-write: increment the key, and if the result is
      // 1 (the key was just created) set its TTL to `seconds`. Returns the new
      // counter -- the atomic native form of the fixed-window rate-limit idiom
      // Redis's own docs script in Lua (INCR; if == 1 then EXPIRE). The window
      // resets for free: an expired key is lazily purged before the INCR, so the
      // next call recreates it at 1 and re-arms the TTL.
      const auto seconds = parse_ll(command.args[1]);
      if (!seconds) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto now = store.now_ms();
      const auto when = compute_when_ms(now, *seconds, 1000);
      if (!when) {
        resp::append_error(out, "ERR invalid expire time");
        return;
      }
      const auto value = store.incr_expire(command.args[0], *when, now);
      if (!value) {
        resp::append_error(out, integer_range_error());  // non-integer or overflow
        return;
      }
      resp::append_integer(out, *value);
      return;
    }

    case CommandType::goblin_zwindow: {
      // Sliding-window limiter: evict entries older than `window`, and if the
      // window has room (< `limit`) record this request at `now` and re-arm the
      // key's TTL to `window` (which reaps an idle window). Returns 1 if admitted,
      // else 0. The native form of the zremrangebyscore + zcard + zadd + expire
      // idiom -- and the trailing EXPIRE was the piece TTLs made possible.
      const auto now = parse_score(command.args[1]);
      const auto window = parse_score(command.args[2]);
      const auto limit = parse_ll(command.args[3]);
      if (!now || !window) {
        resp::append_error(out, "ERR value is not a valid float");
        return;
      }
      if (!limit) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (*window > 9.0e15 || *window < -9.0e15) {
        resp::append_error(out, "ERR invalid expire time");
        return;
      }
      const std::uint64_t clock = store.now_ms();
      const auto when = compute_when_ms(clock, static_cast<long long>(*window), 1000);
      if (!when) {
        resp::append_error(out, "ERR invalid expire time");
        return;
      }
      const double cutoff = *now - *window;
      const bool admitted = store.zwindow(command.args[0], *now, cutoff, *limit,
                                          command.args[4], *when, clock);
      resp::append_integer(out, admitted ? 1 : 0);
      return;
    }

    case CommandType::goblin_incrbound: {
      // Bounded increment: apply `delta` only if the result stays <= `max`, else
      // reply -1. The native form of the get + compare + incrby quota idiom, done
      // as one integer parse, one bound check, and a conditional store -- no
      // interpreter and no round trip.
      const auto delta = parse_ll(command.args[1]);
      const auto max = parse_ll(command.args[2]);
      if (!delta || !max) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto result = store.incr_bound(command.args[0], *delta, *max);
      if (!result) {  // stored value not an integer, or the admitted result overflows
        resp::append_error(out, integer_range_error());
        return;
      }
      resp::append_integer(out, *result);  // new value on admit, or -1 on reject
      return;
    }

    case CommandType::goblin_decrpos: {
      // Decrement-if-positive: reserve one unit only when the counter is > 0, else
      // reply -1 without creating or touching the key. The native form of the
      // get + test + decr stock-reservation idiom.
      const auto result = store.decr_positive(command.args[0]);
      if (!result) {  // stored value is not an integer
        resp::append_error(out, integer_range_error());
        return;
      }
      resp::append_integer(out, *result);  // new value on success, or -1 on reject
      return;
    }

    case CommandType::goblin_hcad: {
      // Compare-and-delete on a hash field: the field-level form of GOBLIN.CAD
      // (HGET; if it equals `expected`, HDEL) as one native atomic op. Replies 1
      // on a match/delete, 0 otherwise.
      const bool deleted = store.hash_compare_and_delete(
          command.args[0], command.args[1], command.args[2]);
      resp::append_integer(out, deleted ? 1 : 0);
      return;
    }

    case CommandType::goblin_hsetgt: {
      // Set-if-greater on a hash field: the ZADD GT that hashes lack (HGET; if the
      // new value beats the current, HSET). Watermarks, monotonic versions,
      // last-write-wins by timestamp. Replies 1 if it updated, 0 if not greater.
      const auto value = parse_score(command.args[2]);
      if (!value) {
        resp::append_error(out, "ERR value is not a valid float");
        return;
      }
      const auto result = store.hash_set_if_greater(command.args[0], command.args[1],
                                                    *value, command.args[2]);
      if (!result) {  // the current field value is present but not a number
        resp::append_error(out, "ERR hash value is not a float");
        return;
      }
      resp::append_integer(out, *result ? 1 : 0);
      return;
    }

    case CommandType::goblin_claim: {
      // Idempotency guard: SET claim_key = token NX EX seconds; if that won the
      // slot reply "CLAIMED", otherwise the request was already handled -- reply
      // the stored result under result_key (GET). One native atomic op, no
      // interpreter: the SET-NX-EX and the GET are direct store calls.
      //   args: claim_key(0) result_key(1) token(2) seconds(3)
      const auto seconds = parse_ll(command.args[3]);
      if (!seconds) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (*seconds <= 0) {
        resp::append_error(out, "ERR invalid expire time in 'goblin.claim' command");
        return;
      }
      if (command.args[2].size() > Store::max_value_bytes()) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      const auto now = store.now_ms();
      const auto when = compute_when_ms(now, *seconds, 1000);
      if (!when) {  // seconds so large the expiry leaves the representable range
        resp::append_error(out, "ERR invalid expire time in 'goblin.claim' command");
        return;
      }
      const auto outcome =
          store.claim(command.args[0], command.args[1], command.args[2], *when, now);
      if (outcome.claimed) {
        resp::append_bulk_string(out, "CLAIMED");
      } else if (outcome.result_wrongtype) {
        resp::append_error(out, kWrongType);
      } else if (outcome.result) {
        resp::append_bulk_string(out, *outcome.result);
      } else {
        resp::append_null_bulk_string(out);
      }
      return;
    }

    case CommandType::goblin_save: {
      bool with_accelerator = true;
      if (command.args.size() == 2) {
        if (equals_ci(command.args[1], "noaccel")) {
          with_accelerator = false;
        } else if (!equals_ci(command.args[1], "accel")) {
          resp::append_error(out, "ERR syntax error, expected ACCEL or NOACCEL");
          return;
        }
      }
      switch (store.start_background_save(std::string(command.args[0]),
                                          with_accelerator)) {
        case Store::SaveStart::Started:
          resp::append_simple_string(out, "Background saving started");
          break;
        case Store::SaveStart::AlreadyRunning:
          resp::append_error(out, "ERR background save already in progress");
          break;
        case Store::SaveStart::ForkFailed:
          resp::append_error(out, "ERR cannot fork for background save");
          break;
      }
      return;
    }

    case CommandType::goblin_load: {
      std::ifstream file(std::string(command.args[0]), std::ios::binary);
      if (!file) {
        resp::append_error(out, "ERR cannot open snapshot file for reading");
        return;
      }
      try {
        const auto stats = store.load(file);
        resp::append_integer(out, static_cast<long long>(stats.keys));
      } catch (const std::exception& error) {
        resp::append_error(out, "ERR snapshot load failed: " + std::string(error.what()));
      }
      return;
    }

    case CommandType::unknown:
      resp::append_error(out, "ERR unknown command '" + std::string(command.name) + "'");
      return;
  }

  resp::append_error(out, "ERR internal command dispatch error");
}

std::string execute_command(Store& store, const Command& command) {
  std::string out;
  execute_command_into(store, command, out);
  return out;
}

void handle_command_into(Store& store,
                         std::span<const std::string_view> fields,
                         std::string& out,
                         CommandExecutionOptions options) {
  auto parsed = parse_command(fields);
  if (!parsed.ok()) {
    resp::append_error(out, parsed.error);
    return;
  }

  execute_command_into(store, *parsed.command, out, options);
}

std::string handle_command(Store& store, std::span<const std::string_view> fields) {
  std::string out;
  handle_command_into(store, fields, out);
  return out;
}

}  // namespace goblin::core
