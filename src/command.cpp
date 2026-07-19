#include "goblin/core/command.hpp"

#include "goblin/core/auth.hpp"
#include "goblin/core/exasock.hpp"
#include "goblin/core/luau_script.hpp"
#include "goblin/core/parse_int.hpp"
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
// used_memory is what our allocation layers actually hold, including retained
// blob-pool capacity and full-hash heap state; used_memory_rss is the OS resident
// set. mem_fragmentation_ratio remains the arena-internal ratio: allocation over
// its compacted size (used minus reclaimable dead arena bytes).
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
  s += "hash_implementation:" +
       std::string(hash_implementation_name(store.hash_implementation())) +
       "\r\n";
  s += "array_implementation:" +
       std::string(array_implementation_name(store.array_implementation())) +
       "\r\n";
  s += "keyspace_index:" +
       std::string(store.real_time() ? "linear" : "swiss") + "\r\n";
#if defined(GOBLIN_HAS_RDMA)
  s += "rdma_support:1\r\n";
#else
  s += "rdma_support:0\r\n";
#endif
#if defined(GOBLIN_HAS_EXASOCK)
  s += "exasock_support:1\r\n";
  {
    // Runtime: true only under the `exasock` LD_PRELOAD wrapper.
    const bool loaded = goblin::core::exasock::loaded();
    s += "exasock_loaded:";
    s += loaded ? "1\r\n" : "0\r\n";
    if (loaded) {
      s += "exasock_version:";
      s += goblin::core::exasock::version_text();
      s += "\r\n";
    }
  }
#else
  s += "exasock_support:0\r\n";
  s += "exasock_loaded:0\r\n";
#endif
  s += "# Memory\r\n";
  s += "used_memory:" + std::to_string(used) + "\r\n";
  s += "used_memory_rss:" + std::to_string(rss) + "\r\n";
  s += "used_memory_peak:" + std::to_string(used) + "\r\n";
  s += "mem_reclaimable_bytes:" + std::to_string(dead) + "\r\n";
  s += "hash_heap_allocated_bytes:" +
       std::to_string(mem.hash_heap_allocated_bytes) + "\r\n";
  s += "realtime_hash_index_pool_bytes:" +
       std::to_string(mem.realtime_hash_index_pool_bytes) + "\r\n";
  s += "realtime_keyspace_index_pool_bytes:" +
       std::to_string(mem.realtime_keyspace_index_pool_bytes) + "\r\n";
  s += "blob_pool_requested_bytes:" +
       std::to_string(mem.blob_pool_requested_bytes) + "\r\n";
  s += "blob_pool_capacity_bytes:" +
       std::to_string(mem.blob_pool_capacity_bytes) + "\r\n";
  s += "blob_pool_fragmentation_bytes:" +
       std::to_string(mem.blob_pool_fragmentation_bytes) + "\r\n";
  s += "blob_pool_live_allocations:" +
       std::to_string(mem.blob_pool_live_allocations) + "\r\n";
  s += "blob_pool_upstream_allocations:" +
       std::to_string(mem.blob_pool_upstream_allocations) + "\r\n";
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

[[nodiscard]] bool starts_with_ci(std::string_view value,
                                  std::string_view prefix) {
  return value.size() >= prefix.size() &&
         equals_ci(value.substr(0, prefix.size()), prefix);
}

[[nodiscard]] std::optional<HashImplementation> qualified_hash_implementation(
    std::string_view command_name) {
  if (starts_with_ci(command_name, "GOBLIN.RT.")) {
    return HashImplementation::Realtime;
  }
  if (starts_with_ci(command_name, "GOBLIN.EFFICENT.")) {
    return HashImplementation::Efficient;
  }
  return std::nullopt;
}

using goblin::core::parse_i64;

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

[[nodiscard]] std::optional<double> parse_zset_score(std::string_view text) {
  if (equals_ci(text, "-inf")) {
    return -std::numeric_limits<double>::infinity();
  }
  if (equals_ci(text, "+inf") || equals_ci(text, "inf")) {
    return std::numeric_limits<double>::infinity();
  }
  return parse_score(text);
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

[[nodiscard]] resp::Version response_version(
    const CommandExecutionOptions& options) noexcept {
  return options.resp_version != nullptr ? *options.resp_version
                                         : resp::Version::resp2;
}

void append_null_array(std::string& out, resp::Version version) {
  if (version == resp::Version::resp3) {
    resp::append_null(out, version);
  } else {
    out.append("*-1\r\n");
  }
}

void append_hello_response(std::string& out, resp::Version version,
                           std::uint64_t connection_id) {
  constexpr std::size_t kFieldCount = 7;
  if (version == resp::Version::resp3) {
    resp::append_map_header(out, kFieldCount);
  } else {
    resp::append_array_header(out, kFieldCount * 2);
  }

  const auto append_string = [&out](std::string_view key,
                                    std::string_view value) {
    resp::append_bulk_string(out, key);
    resp::append_bulk_string(out, value);
  };
  append_string("server", "goblin-core");
  append_string("version", GOBLIN_CORE_VERSION);
  resp::append_bulk_string(out, "proto");
  resp::append_integer(out, static_cast<unsigned>(version));
  resp::append_bulk_string(out, "id");
  resp::append_integer(out, static_cast<long long>(connection_id));
  append_string("mode", "standalone");
  append_string("role", "master");
  resp::append_bulk_string(out, "modules");
  resp::append_array_header(out, 0);
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

[[nodiscard]] std::string parse_zrange_options(Command& command,
                                               bool force_by_score,
                                               bool force_reverse,
                                               bool legacy_command) {
  command.range_by_score = force_by_score;
  command.range_reverse = force_reverse;
  bool seen_by_score = force_by_score;
  bool seen_reverse = force_reverse;
  bool seen_with_scores = false;

  for (std::size_t i = 3; i < command.args.size(); ++i) {
    const auto option = command.args[i];
    if (equals_ci(option, "BYSCORE")) {
      if (legacy_command || seen_by_score || force_by_score) {
        return syntax_error();
      }
      seen_by_score = true;
      command.range_by_score = true;
      continue;
    }
    if (equals_ci(option, "REV")) {
      if (legacy_command || seen_reverse || force_reverse) {
        return syntax_error();
      }
      seen_reverse = true;
      command.range_reverse = true;
      continue;
    }
    if (equals_ci(option, "WITHSCORES")) {
      if (seen_with_scores) {
        return syntax_error();
      }
      seen_with_scores = true;
      command.with_scores = true;
      continue;
    }
    if (equals_ci(option, "LIMIT")) {
      if (command.range_has_limit || i + 2 >= command.args.size()) {
        return syntax_error();
      }
      const auto offset = parse_i64(command.args[i + 1]);
      const auto count = parse_i64(command.args[i + 2]);
      if (!offset || !count || *offset < 0) {
        return integer_range_error();
      }
      command.range_has_limit = true;
      command.range_limit_offset = *offset;
      command.range_limit_count = *count;
      i += 2;
      continue;
    }
    return syntax_error();
  }

  if (command.range_has_limit && !command.range_by_score) {
    return syntax_error();
  }
  if (!command.range_by_score && !parse_range_indexes(command)) {
    return integer_range_error();
  }
  return {};
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

[[nodiscard]] std::string fields_response(
    const std::vector<std::string>& fields, resp::Version version) {
  std::string out;
  if (version == resp::Version::resp3) {
    resp::append_map_header(out, fields.size() / 2);
  } else {
    resp::append_array_header(out, fields.size());
  }
  for (const auto& field : fields) {
    resp::append_bulk_string(out, field);
  }
  return out;
}

[[nodiscard]] std::string memory_stats_response(const ZSetMemoryStats& stats,
                                                resp::Version version) {
  const auto fields = memory_stats_fields(stats);
  return fields_response(fields, version);
}

[[nodiscard]] std::vector<std::string> hash_memory_stats_fields(const HashMemoryStats& stats) {
  std::vector<std::string> fields;
  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  fields.emplace_back("implementation");
  fields.emplace_back(hash_implementation_name(stats.implementation));
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
  add("field_compaction_selection_nanoseconds",
      stats.field_compaction_selection_nanoseconds);
  add("field_compaction_densify_nanoseconds",
      stats.field_compaction_densify_nanoseconds);
  add("field_compaction_donor_nanoseconds",
      stats.field_compaction_donor_nanoseconds);
  add("field_compaction_tail_settle_nanoseconds",
      stats.field_compaction_tail_settle_nanoseconds);
  add("hash_heap_allocated_bytes", stats.hash_heap_allocated_bytes);
  add("keyspace_accounted_bytes", stats.keyspace_accounted_bytes);
  add("total_allocated_bytes", stats.total_allocated_bytes);

  return fields;
}

[[nodiscard]] std::vector<std::string> set_memory_stats_fields(
    const SetMemoryStats& stats) {
  std::vector<std::string> fields;
  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  add("member_count", stats.member_count);
  add("member_live_bytes", stats.member_live_bytes);
  add("member_dead_bytes", stats.member_dead_bytes);
  add("member_allocated_bytes", stats.member_allocated_bytes);
  add("member_index_capacity", stats.member_index_capacity);
  add("member_index_tombstones", stats.member_index_tombstones);
  add("member_index_allocated_bytes", stats.member_index_allocated_bytes);
  add("total_allocated_bytes", stats.total_allocated_bytes);
  return fields;
}

[[nodiscard]] std::string set_memory_stats_response(const SetMemoryStats& stats,
                                                    resp::Version version) {
  return fields_response(set_memory_stats_fields(stats), version);
}

[[nodiscard]] std::string hash_memory_stats_response(const HashMemoryStats& stats,
                                                     resp::Version version) {
  const auto fields = hash_memory_stats_fields(stats);
  return fields_response(fields, version);
}

[[nodiscard]] std::vector<std::string> list_memory_stats_fields(
    const ListMemoryStats& stats) {
  std::vector<std::string> fields;
  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  fields.emplace_back("implementation");
  fields.emplace_back(list_implementation_name(stats.implementation));
  add("element_count", stats.element_count);
  add("object_allocated_bytes", stats.object_allocated_bytes);
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
    const ListMemoryStats& stats, resp::Version version) {
  const auto fields = list_memory_stats_fields(stats);
  return fields_response(fields, version);
}

[[nodiscard]] std::vector<std::string> array_memory_stats_fields(
    const ArrayMemoryStats& stats) {
  std::vector<std::string> fields;
  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };
  fields.emplace_back("implementation");
  fields.emplace_back(array_implementation_name(stats.implementation));
  add("element_count", stats.element_count);
  add("logical_length", stats.logical_length);
  add("slice_count", stats.slice_count);
  add("sparse_slices", stats.sparse_slices);
  add("dense_slices", stats.dense_slices);
  add("directory_depth", stats.directory_depth);
  add("directory_nodes", stats.directory_nodes);
  add("value_live_bytes", stats.value_live_bytes);
  add("value_dead_bytes", stats.value_dead_bytes);
  add("value_allocated_bytes", stats.value_allocated_bytes);
  add("leaf_table_bytes", stats.leaf_table_bytes);
  add("realtime_reserved", stats.realtime_reserved ? 1 : 0);
  add("reserved_max_index", stats.reserved_max_index);
  add("reserved_value_capacity", stats.reserved_value_capacity);
  add("reserved_value_bytes", stats.reserved_value_bytes);
  add("total_allocated_bytes", stats.total_allocated_bytes);
  return fields;
}

[[nodiscard]] std::string array_memory_stats_response(
    const ArrayMemoryStats& stats, resp::Version version) {
  return fields_response(array_memory_stats_fields(stats), version);
}

constexpr std::string_view kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

// Every stored field/value reference retains a 16-bit encoded length. LZ4 can
// admit a larger logical value when that encoded representation still fits.
constexpr std::string_view kValueTooLarge =
    "ERR value does not fit the 65,535-byte encoded limit; use "
    "https://goblin-store.dev";

[[nodiscard]] constexpr std::string_view key_type_name(KeyType type) noexcept {
  switch (type) {
    case KeyType::String:
      return "string";
    case KeyType::Zset:
      return "zset";
    case KeyType::Hash:
      return "hash";
    case KeyType::List:
      return "list";
    case KeyType::Set:
      return "set";
    case KeyType::Array:
      return "array";
  }
  return "none";
}

[[nodiscard]] bool is_zset_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::zadd:
    case CommandType::zincrby:
    case CommandType::zcard:
    case CommandType::zcount:
    case CommandType::zrange:
    case CommandType::zrangebyscore:
    case CommandType::zrevrangebyscore:
    case CommandType::zrank:
    case CommandType::zrevrange:
    case CommandType::zrevrank:
    case CommandType::zrem:
    case CommandType::zremrangebyscore:
    case CommandType::zmscore:
    case CommandType::zpopmin:
    case CommandType::zpopmax:
    case CommandType::zscan:
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
    case CommandType::hscan:
    case CommandType::goblin_hcad:    // deletes a field; non-hash is WRONGTYPE
    case CommandType::goblin_hsetgt:  // sets a field; non-hash is WRONGTYPE
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_set_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::sadd:
    case CommandType::srem:
    case CommandType::scard:
    case CommandType::sismember:
    case CommandType::smismember:
    case CommandType::smembers:
    case CommandType::spop:
    case CommandType::srandmember:
    case CommandType::smove:
    case CommandType::sinter:
    case CommandType::sunion:
    case CommandType::sdiff:
    case CommandType::sscan:
      // *STORE first-arg is a destination that may clobber any type; SINTERCARD
      // first-arg is numkeys. Both are type-checked in their handlers.
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_array_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::arreserve:
    case CommandType::arset:
    case CommandType::arget:
    case CommandType::armset:
    case CommandType::armget:
    case CommandType::arlen:
    case CommandType::arcount:
    case CommandType::ardel:
    case CommandType::arinsert:
    case CommandType::arnext:
    case CommandType::arseek:
      return true;
    default:
      return false;
  }
}

// Redis-style glob for incremental scans' MATCH option (* ? [abc]).
[[nodiscard]] bool scan_glob_match(std::string_view pattern,
                                   std::string_view value) noexcept {
  std::size_t pi = 0;
  std::size_t vi = 0;
  std::size_t star_pi = std::string_view::npos;
  std::size_t star_vi = 0;
  while (vi < value.size()) {
    if (pi < pattern.size() &&
        (pattern[pi] == '?' || pattern[pi] == value[vi])) {
      ++pi;
      ++vi;
      continue;
    }
    if (pi < pattern.size() && pattern[pi] == '*') {
      star_pi = pi++;
      star_vi = vi;
      continue;
    }
    if (pi < pattern.size() && pattern[pi] == '[') {
      const auto close = pattern.find(']', pi + 1);
      if (close == std::string_view::npos) {
        return false;
      }
      const auto class_body = pattern.substr(pi + 1, close - pi - 1);
      bool negate = false;
      std::size_t ci = 0;
      if (!class_body.empty() &&
          (class_body[0] == '^' || class_body[0] == '!')) {
        negate = true;
        ci = 1;
      }
      bool matched = false;
      while (ci < class_body.size()) {
        if (ci + 2 < class_body.size() && class_body[ci + 1] == '-') {
          const auto lo = class_body[ci];
          const auto hi = class_body[ci + 2];
          if (value[vi] >= lo && value[vi] <= hi) {
            matched = true;
            break;
          }
          ci += 3;
        } else {
          if (class_body[ci] == value[vi]) {
            matched = true;
            break;
          }
          ++ci;
        }
      }
      if (matched == negate) {
        if (star_pi != std::string_view::npos) {
          pi = star_pi + 1;
          vi = ++star_vi;
          continue;
        }
        return false;
      }
      pi = close + 1;
      ++vi;
      continue;
    }
    if (star_pi != std::string_view::npos) {
      pi = star_pi + 1;
      vi = ++star_vi;
      continue;
    }
    return false;
  }
  while (pi < pattern.size() && pattern[pi] == '*') {
    ++pi;
  }
  return pi == pattern.size();
}

[[nodiscard]] bool is_list_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::lmove:
    case CommandType::rpoplpush:
    case CommandType::blpop:
    case CommandType::brpop:
    case CommandType::blmove:
    case CommandType::lmpop:
    case CommandType::blmpop:
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
    case CommandType::pma_lmove:
    case CommandType::pma_rpoplpush:
    case CommandType::pma_blpop:
    case CommandType::pma_brpop:
    case CommandType::pma_blmove:
    case CommandType::pma_lmpop:
    case CommandType::pma_blmpop:
    case CommandType::pma_llen:
    case CommandType::pma_lindex:
    case CommandType::pma_lrange:
    case CommandType::pma_lset:
    case CommandType::pma_ltrim:
    case CommandType::pma_lrem:
    case CommandType::pma_linsert:
    case CommandType::segmented_lpush:
    case CommandType::segmented_rpush:
    case CommandType::segmented_lpushx:
    case CommandType::segmented_rpushx:
    case CommandType::segmented_lpop:
    case CommandType::segmented_rpop:
    case CommandType::segmented_lmove:
    case CommandType::segmented_rpoplpush:
    case CommandType::segmented_blpop:
    case CommandType::segmented_brpop:
    case CommandType::segmented_blmove:
    case CommandType::segmented_lmpop:
    case CommandType::segmented_blmpop:
    case CommandType::segmented_llen:
    case CommandType::segmented_lindex:
    case CommandType::segmented_lrange:
    case CommandType::segmented_lset:
    case CommandType::segmented_ltrim:
    case CommandType::segmented_lrem:
    case CommandType::segmented_linsert:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] CommandType resolve_list_command(
    CommandType type, ListImplementation implementation) noexcept {
  switch (implementation) {
    case ListImplementation::Segmented:
      switch (type) {
        case CommandType::lpush:
          return CommandType::segmented_lpush;
        case CommandType::rpush:
          return CommandType::segmented_rpush;
        case CommandType::lpushx:
          return CommandType::segmented_lpushx;
        case CommandType::rpushx:
          return CommandType::segmented_rpushx;
        case CommandType::lpop:
          return CommandType::segmented_lpop;
        case CommandType::rpop:
          return CommandType::segmented_rpop;
        case CommandType::lmove:
          return CommandType::segmented_lmove;
        case CommandType::rpoplpush:
          return CommandType::segmented_rpoplpush;
        case CommandType::blpop:
          return CommandType::segmented_blpop;
        case CommandType::brpop:
          return CommandType::segmented_brpop;
        case CommandType::blmove:
          return CommandType::segmented_blmove;
        case CommandType::lmpop:
          return CommandType::segmented_lmpop;
        case CommandType::blmpop:
          return CommandType::segmented_blmpop;
        case CommandType::llen:
          return CommandType::segmented_llen;
        case CommandType::lindex:
          return CommandType::segmented_lindex;
        case CommandType::lrange:
          return CommandType::segmented_lrange;
        case CommandType::lset:
          return CommandType::segmented_lset;
        case CommandType::ltrim:
          return CommandType::segmented_ltrim;
        case CommandType::lrem:
          return CommandType::segmented_lrem;
        case CommandType::linsert:
          return CommandType::segmented_linsert;
        default:
          return type;
      }
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
        case CommandType::lmove:
          return CommandType::pma_lmove;
        case CommandType::rpoplpush:
          return CommandType::pma_rpoplpush;
        case CommandType::blpop:
          return CommandType::pma_blpop;
        case CommandType::brpop:
          return CommandType::pma_brpop;
        case CommandType::blmove:
          return CommandType::pma_blmove;
        case CommandType::lmpop:
          return CommandType::pma_lmpop;
        case CommandType::blmpop:
          return CommandType::pma_blmpop;
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
    // GET is omitted: its store path returns Missing/WrongType/Ok in one probe
    // (see CommandType::get), so the central type gate would be a second find.
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
  if (is_set_command(type)) {
    return KeyType::Set;
  }
  if (is_array_command(type)) {
    return KeyType::Array;
  }
  if (is_list_command(type)) {
    switch (type) {
      // These commands carry timeout/numkeys before their first actual key and
      // validate every declared key in their handler.
      case CommandType::lmpop:
      case CommandType::blmpop:
      case CommandType::pma_lmpop:
      case CommandType::pma_blmpop:
      case CommandType::segmented_lmpop:
      case CommandType::segmented_blmpop:
        return std::nullopt;
      default:
        break;
    }
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
    case CommandType::get:  // fused type check; still purges on access
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
    // Set *STORE: destination is first; sources are purged in the handlers.
    case CommandType::sinterstore:
    case CommandType::sunionstore:
    case CommandType::sdiffstore:
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
  return parse_i64(text);
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

struct Resp3WithscoresAppender {
  std::string& out;

  void operator()(std::string_view member, double score) const {
    resp::append_array_header(out, 2);
    resp::append_bulk_string(out, member);
    resp::append_double(out, score);
  }
};

struct Resp3WithscoresTextAppender {
  std::string& out;

  void operator()(std::string_view member, std::string_view score) const {
    resp::append_array_header(out, 2);
    resp::append_bulk_string(out, member);
    resp::append_double(out, score);
  }
};

void append_range_response(Store& store,
                           const Command& command,
                           bool reverse,
                           std::string& out,
                           CommandExecutionOptions options) {
  long long start = command.range_start;
  long long stop = command.range_stop;
  ScoreBound min_bound;
  ScoreBound max_bound;
  std::size_t score_offset = 0;
  std::optional<std::size_t> score_limit;
  if (command.range_by_score) {
    const auto first = parse_score_bound(command.args[1]);
    const auto second = parse_score_bound(command.args[2]);
    if (!first || !second) {
      resp::append_error(out, "ERR min or max is not a float");
      return;
    }
    min_bound = reverse ? *second : *first;
    max_bound = reverse ? *first : *second;
    if (command.range_has_limit) {
      score_offset = static_cast<std::size_t>(command.range_limit_offset);
      if (command.range_limit_count >= 0) {
        score_limit = static_cast<std::size_t>(command.range_limit_count);
      }
    }
  } else if (!command.range_indexes_parsed) {
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
  const auto version = response_version(options);

  // Reserve and write the array header in the counted callback so bounds are
  // computed once per range (the prior zrange_size + for_each split doubled
  // find_zset/range_bounds work and regressed WITHSCORES).
  auto append_header = [&out, with_scores, reserve_limit,
                        version](std::size_t entry_count) {
    const auto element_count = with_scores && version == resp::Version::resp2
                                   ? entry_count * 2
                                   : entry_count;
    resp::reserve_append_capacity(
        out,
        16 + entry_count * (with_scores ? 48 : 32),
        reserve_limit);
    resp::append_array_header(out, element_count);
  };

  auto stream_members = [&](auto&& count_fn, auto&& fn) {
    if (command.range_by_score) {
      return store.zrange_by_score_members_for_each_counted(
          key, min_bound.value, min_bound.exclusive, max_bound.value,
          max_bound.exclusive, reverse, score_offset, score_limit,
          std::forward<decltype(count_fn)>(count_fn),
          std::forward<decltype(fn)>(fn));
    }
    if (reverse) {
      return store.zrevrange_members_for_each_counted(
          key, start, stop, std::forward<decltype(count_fn)>(count_fn),
          std::forward<decltype(fn)>(fn));
    }
    return store.zrange_members_for_each_counted(
        key, start, stop, std::forward<decltype(count_fn)>(count_fn),
        std::forward<decltype(fn)>(fn));
  };
  auto stream_values = [&](auto&& count_fn, auto&& fn) {
    if (command.range_by_score) {
      return store.zrange_by_score_values_for_each_counted(
          key, min_bound.value, min_bound.exclusive, max_bound.value,
          max_bound.exclusive, reverse, score_offset, score_limit,
          std::forward<decltype(count_fn)>(count_fn),
          std::forward<decltype(fn)>(fn));
    }
    if (reverse) {
      return store.zrevrange_values_for_each_counted(
          key, start, stop, std::forward<decltype(count_fn)>(count_fn),
          std::forward<decltype(fn)>(fn));
    }
    return store.zrange_values_for_each_counted(
        key, start, stop, std::forward<decltype(count_fn)>(count_fn),
        std::forward<decltype(fn)>(fn));
  };
  auto stream_text_values = [&](auto&& count_fn, auto&& fn) {
    if (command.range_by_score) {
      return store.zrange_by_score_text_values_for_each_counted(
          key, min_bound.value, min_bound.exclusive, max_bound.value,
          max_bound.exclusive, reverse, score_offset, score_limit,
          std::forward<decltype(count_fn)>(count_fn),
          std::forward<decltype(fn)>(fn));
    }
    if (reverse) {
      return store.zrevrange_score_text_values_for_each_counted(
          key, start, stop, std::forward<decltype(count_fn)>(count_fn),
          std::forward<decltype(fn)>(fn));
    }
    return store.zrange_score_text_values_for_each_counted(
        key, start, stop, std::forward<decltype(count_fn)>(count_fn),
        std::forward<decltype(fn)>(fn));
  };

  if (with_scores) {
    if (version == resp::Version::resp3) {
      if (store.score_string_cache_enabled()) {
        Resp3WithscoresTextAppender appender{out};
        stream_text_values(append_header, appender);
      } else {
        Resp3WithscoresAppender appender{out};
        stream_values(append_header, appender);
      }
      return;
    }

    if (store.score_string_cache_enabled()) {
      WithscoresTextChunkAppender appender{out};
      stream_text_values(append_header, appender);
      appender.flush();
      return;
    }

    WithscoresChunkAppender appender{out};
    stream_values(append_header, appender);
    appender.flush();
    return;
  }

  auto append_member = [&out](std::string_view member) {
    resp::append_bulk_string(out, member);
  };
  stream_members(append_header, append_member);
}

}  // namespace

CommandType lookup_command_type(std::string_view name) noexcept {
  if (name.size() > 31) {
    return CommandType::unknown;
  }
  std::array<char, 32> upper{};
  for (std::size_t k = 0; k < name.size(); ++k) {
    upper[k] = ascii_upper_char(name[k]);
  }
  const CommandEntry* entry = CommandDispatch::lookup(upper.data(), name.size());
  return entry == nullptr ? CommandType::unknown : entry->type;
}

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
    // Capture qualified hash implementation once at parse (L). Avoids
    // re-scanning the name on every execute_command_into for H*.
    if (command.name.size() >= 10 && upper[0] == 'G' && upper[1] == 'O' &&
        upper[2] == 'B' && upper[3] == 'L' && upper[4] == 'I' &&
        upper[5] == 'N' && upper[6] == '.') {
      if (command.name.size() >= 10 && upper[7] == 'R' && upper[8] == 'T' &&
          upper[9] == '.') {
        command.hash_implementation_tag = 2;  // Realtime
      } else if (command.name.size() >= 16 && upper[7] == 'E' &&
                 upper[8] == 'F' && upper[9] == 'F' && upper[10] == 'I' &&
                 upper[11] == 'C' && upper[12] == 'E' && upper[13] == 'N' &&
                 upper[14] == 'T' && upper[15] == '.') {
        // GOBLIN.EFFICENT.* — hash non-RT family (historical spelling).
        command.hash_implementation_tag = 1;  // Efficient (hashes)
      } else if (command.name.size() >= 15 && upper[7] == 'C' &&
                 upper[8] == 'L' && upper[9] == 'A' && upper[10] == 'S' &&
                 upper[11] == 'S' && upper[12] == 'I' && upper[13] == 'C' &&
                 upper[14] == '.') {
        // GOBLIN.CLASSIC.* — array Redis-8.8-style family (not hashes).
        command.hash_implementation_tag = 1;  // Classic (arrays) / non-RT tag
      }
    }
  }
  if (entry == nullptr) {
    command.type = CommandType::unknown;
    return {.command = std::move(command)};
  }
  const CommandType looked_up_type = entry->type;

  // Arity/setup bodies are identical to the former equals_ci chain; only the
  // selection changed. equals_ci is still used for in-handler argument checks.
  switch (looked_up_type) {
    case CommandType::ping:
      if (command.args.size() > 1) {
        return parse_error(wrong_arity("ping"));
      }
      command.type = CommandType::ping;
      return {.command = std::move(command)};
    case CommandType::hello:
      command.type = CommandType::hello;
      return {.command = std::move(command)};
    case CommandType::auth:
      if (command.args.size() < 1 || command.args.size() > 2) {
        return parse_error(wrong_arity("auth"));
      }
      command.type = CommandType::auth;
      return {.command = std::move(command)};
    case CommandType::command:
      command.type = CommandType::command;
      return {.command = std::move(command)};
    case CommandType::client:
      if (command.args.empty()) {
        return parse_error(wrong_arity("client"));
      }
      command.type = CommandType::client;
      return {.command = std::move(command)};
    case CommandType::select:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("select"));
      }
      command.type = CommandType::select;
      return {.command = std::move(command)};
    case CommandType::quit:
      if (!command.args.empty()) {
        return parse_error(wrong_arity("quit"));
      }
      command.type = CommandType::quit;
      return {.command = std::move(command)};
    case CommandType::multi:
      if (!command.args.empty()) {
        return parse_error(wrong_arity("multi"));
      }
      command.type = CommandType::multi;
      return {.command = std::move(command)};
    case CommandType::exec:
      if (!command.args.empty()) {
        return parse_error(wrong_arity("exec"));
      }
      command.type = CommandType::exec;
      return {.command = std::move(command)};
    case CommandType::discard:
      if (!command.args.empty()) {
        return parse_error(wrong_arity("discard"));
      }
      command.type = CommandType::discard;
      return {.command = std::move(command)};
    case CommandType::watch:
      if (command.args.empty()) {
        return parse_error(wrong_arity("watch"));
      }
      command.type = CommandType::watch;
      return {.command = std::move(command)};
    case CommandType::unwatch:
      if (!command.args.empty()) {
        return parse_error(wrong_arity("unwatch"));
      }
      command.type = CommandType::unwatch;
      return {.command = std::move(command)};
    case CommandType::subscribe:
      if (command.args.empty()) {
        return parse_error(wrong_arity("subscribe"));
      }
      command.type = CommandType::subscribe;
      return {.command = std::move(command)};
    case CommandType::unsubscribe:
      command.type = CommandType::unsubscribe;
      return {.command = std::move(command)};
    case CommandType::psubscribe:
      if (command.args.empty()) {
        return parse_error(wrong_arity("psubscribe"));
      }
      command.type = CommandType::psubscribe;
      return {.command = std::move(command)};
    case CommandType::punsubscribe:
      command.type = CommandType::punsubscribe;
      return {.command = std::move(command)};
    case CommandType::publish:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("publish"));
      }
      command.type = CommandType::publish;
      return {.command = std::move(command)};
    case CommandType::pubsub:
      if (command.args.empty()) {
        return parse_error(wrong_arity("pubsub"));
      }
      command.type = CommandType::pubsub;
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
      if (command.args.size() < 3) {
        return parse_error(wrong_arity("zadd"));
      }
      command.type = CommandType::zadd;
      return {.command = std::move(command)};
    case CommandType::zincrby:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("zincrby"));
      }
      command.type = CommandType::zincrby;
      return {.command = std::move(command)};
    case CommandType::zcard:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("zcard"));
      }
      command.type = CommandType::zcard;
      return {.command = std::move(command)};
    case CommandType::zcount:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("zcount"));
      }
      command.type = CommandType::zcount;
      return {.command = std::move(command)};
    case CommandType::zrange: {
      if (command.args.size() < 3) {
        return parse_error(wrong_arity("zrange"));
      }
      if (auto error = parse_zrange_options(command, false, false, false);
          !error.empty()) {
        return parse_error(std::move(error));
      }
      command.type = CommandType::zrange;
      return {.command = std::move(command)};
    }
    case CommandType::zrangebyscore:
    case CommandType::zrevrangebyscore: {
      if (command.args.size() < 3) {
        return parse_error(wrong_arity(command.name));
      }
      const bool reverse = looked_up_type == CommandType::zrevrangebyscore;
      if (auto error = parse_zrange_options(command, true, reverse, true);
          !error.empty()) {
        return parse_error(std::move(error));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    }
    case CommandType::zrank:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("zrank"));
      }
      command.type = CommandType::zrank;
      return {.command = std::move(command)};
    case CommandType::zrevrange: {
      if (command.args.size() < 3) {
        return parse_error(wrong_arity("zrevrange"));
      }
      if (auto error = parse_zrange_options(command, false, true, true);
          !error.empty()) {
        return parse_error(std::move(error));
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
    case CommandType::zmscore:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("zmscore"));
      }
      command.type = CommandType::zmscore;
      return {.command = std::move(command)};
    case CommandType::zpopmin:
    case CommandType::zpopmax:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::zscan:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("zscan"));
      }
      command.type = CommandType::zscan;
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
    case CommandType::hscan:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("hscan"));
      }
      command.type = CommandType::hscan;
      return {.command = std::move(command)};
    case CommandType::sadd:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("sadd"));
      }
      command.type = CommandType::sadd;
      return {.command = std::move(command)};
    case CommandType::srem:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("srem"));
      }
      command.type = CommandType::srem;
      return {.command = std::move(command)};
    case CommandType::scard:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("scard"));
      }
      command.type = CommandType::scard;
      return {.command = std::move(command)};
    case CommandType::sismember:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("sismember"));
      }
      command.type = CommandType::sismember;
      return {.command = std::move(command)};
    case CommandType::smismember:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("smismember"));
      }
      command.type = CommandType::smismember;
      return {.command = std::move(command)};
    case CommandType::smembers:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("smembers"));
      }
      command.type = CommandType::smembers;
      return {.command = std::move(command)};
    case CommandType::spop:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity("spop"));
      }
      command.type = CommandType::spop;
      return {.command = std::move(command)};
    case CommandType::srandmember:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity("srandmember"));
      }
      command.type = CommandType::srandmember;
      return {.command = std::move(command)};
    case CommandType::smove:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity("smove"));
      }
      command.type = CommandType::smove;
      return {.command = std::move(command)};
    case CommandType::sinter:
      if (command.args.size() < 1) {
        return parse_error(wrong_arity("sinter"));
      }
      command.type = CommandType::sinter;
      return {.command = std::move(command)};
    case CommandType::sunion:
      if (command.args.size() < 1) {
        return parse_error(wrong_arity("sunion"));
      }
      command.type = CommandType::sunion;
      return {.command = std::move(command)};
    case CommandType::sdiff:
      if (command.args.size() < 1) {
        return parse_error(wrong_arity("sdiff"));
      }
      command.type = CommandType::sdiff;
      return {.command = std::move(command)};
    case CommandType::sinterstore:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("sinterstore"));
      }
      command.type = CommandType::sinterstore;
      return {.command = std::move(command)};
    case CommandType::sunionstore:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("sunionstore"));
      }
      command.type = CommandType::sunionstore;
      return {.command = std::move(command)};
    case CommandType::sdiffstore:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("sdiffstore"));
      }
      command.type = CommandType::sdiffstore;
      return {.command = std::move(command)};
    case CommandType::sintercard:
      // SINTERCARD numkeys key [key ...] [LIMIT limit]
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("sintercard"));
      }
      command.type = CommandType::sintercard;
      return {.command = std::move(command)};
    case CommandType::sscan:
      // SSCAN key cursor [MATCH pattern] [COUNT count]
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("sscan"));
      }
      command.type = CommandType::sscan;
      return {.command = std::move(command)};
    case CommandType::arreserve:
      // GOBLIN.RT.ARRESERVE key max-index value-slots encoded-bytes
      if (command.args.size() != 4) {
        return parse_error(wrong_arity("goblin.rt.arreserve"));
      }
      command.type = CommandType::arreserve;
      return {.command = std::move(command)};
    case CommandType::arset:
      // ARSET key index value [value ...]
      if (command.args.size() < 3) {
        return parse_error(wrong_arity("arset"));
      }
      command.type = CommandType::arset;
      return {.command = std::move(command)};
    case CommandType::arget:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("arget"));
      }
      command.type = CommandType::arget;
      return {.command = std::move(command)};
    case CommandType::armset:
      // ARMSET key index value [index value ...]
      if (command.args.size() < 3 || (command.args.size() % 2) == 0) {
        return parse_error(wrong_arity("armset"));
      }
      command.type = CommandType::armset;
      return {.command = std::move(command)};
    case CommandType::armget:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("armget"));
      }
      command.type = CommandType::armget;
      return {.command = std::move(command)};
    case CommandType::arlen:
    case CommandType::arcount:
    case CommandType::arnext:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::ardel:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("ardel"));
      }
      command.type = CommandType::ardel;
      return {.command = std::move(command)};
    case CommandType::arinsert:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity("arinsert"));
      }
      command.type = CommandType::arinsert;
      return {.command = std::move(command)};
    case CommandType::arseek:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity("arseek"));
      }
      command.type = CommandType::arseek;
      return {.command = std::move(command)};
    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::pma_lpush:
    case CommandType::pma_rpush:
    case CommandType::pma_lpushx:
    case CommandType::pma_rpushx:
    case CommandType::segmented_lpush:
    case CommandType::segmented_rpush:
    case CommandType::segmented_lpushx:
    case CommandType::segmented_rpushx:
      if (command.args.size() < 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::pma_lpop:
    case CommandType::pma_rpop:
    case CommandType::segmented_lpop:
    case CommandType::segmented_rpop:
      if (command.args.size() != 1 && command.args.size() != 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::lmove:
    case CommandType::pma_lmove:
    case CommandType::segmented_lmove:
      if (command.args.size() != 4) {
        return parse_error(wrong_arity(command.name));
      }
      if (!equals_ci(command.args[2], "LEFT") &&
          !equals_ci(command.args[2], "RIGHT")) {
        return parse_error(syntax_error());
      }
      if (!equals_ci(command.args[3], "LEFT") &&
          !equals_ci(command.args[3], "RIGHT")) {
        return parse_error(syntax_error());
      }
      command.list_pop_front = equals_ci(command.args[2], "LEFT");
      command.list_push_front = equals_ci(command.args[3], "LEFT");
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::rpoplpush:
    case CommandType::pma_rpoplpush:
    case CommandType::segmented_rpoplpush:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.list_pop_front = false;
      command.list_push_front = true;
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::blmove:
    case CommandType::pma_blmove:
    case CommandType::segmented_blmove: {
      if (command.args.size() != 5) {
        return parse_error(wrong_arity(command.name));
      }
      if (!equals_ci(command.args[2], "LEFT") &&
          !equals_ci(command.args[2], "RIGHT")) {
        return parse_error(syntax_error());
      }
      if (!equals_ci(command.args[3], "LEFT") &&
          !equals_ci(command.args[3], "RIGHT")) {
        return parse_error(syntax_error());
      }
      const auto timeout = parse_score(command.args[4]);
      if (!timeout) {
        return parse_error("ERR timeout is not a float or out of range");
      }
      if (*timeout < 0.0) {
        return parse_error("ERR timeout is negative");
      }
      command.list_pop_front = equals_ci(command.args[2], "LEFT");
      command.list_push_front = equals_ci(command.args[3], "LEFT");
      command.list_timeout_seconds = *timeout;
      command.type = looked_up_type;
      return {.command = std::move(command)};
    }
    case CommandType::blpop:
    case CommandType::brpop:
    case CommandType::pma_blpop:
    case CommandType::pma_brpop:
    case CommandType::segmented_blpop:
    case CommandType::segmented_brpop: {
      if (command.args.size() < 2) {
        return parse_error(wrong_arity(command.name));
      }
      const auto timeout = parse_score(command.args.back());
      if (!timeout) {
        return parse_error("ERR timeout is not a float or out of range");
      }
      if (*timeout < 0.0) {
        return parse_error("ERR timeout is negative");
      }
      command.list_key_offset = 0;
      command.list_key_count = command.args.size() - 1;
      command.list_timeout_seconds = *timeout;
      command.list_pop_front =
          looked_up_type == CommandType::blpop ||
          looked_up_type == CommandType::pma_blpop ||
          looked_up_type == CommandType::segmented_blpop;
      command.type = looked_up_type;
      return {.command = std::move(command)};
    }
    case CommandType::lmpop:
    case CommandType::pma_lmpop:
    case CommandType::segmented_lmpop:
    case CommandType::blmpop:
    case CommandType::pma_blmpop:
    case CommandType::segmented_blmpop: {
      const bool blocking = looked_up_type == CommandType::blmpop ||
                            looked_up_type == CommandType::pma_blmpop ||
                            looked_up_type == CommandType::segmented_blmpop;
      const std::size_t minimum = blocking ? 4 : 3;
      if (command.args.size() < minimum) {
        return parse_error(wrong_arity(command.name));
      }
      std::size_t numkeys_offset = 0;
      if (blocking) {
        const auto timeout = parse_score(command.args[0]);
        if (!timeout) {
          return parse_error("ERR timeout is not a float or out of range");
        }
        if (*timeout < 0.0) {
          return parse_error("ERR timeout is negative");
        }
        command.list_timeout_seconds = *timeout;
        numkeys_offset = 1;
      }
      const auto numkeys = parse_ll(command.args[numkeys_offset]);
      if (!numkeys) {
        return parse_error(integer_range_error());
      }
      if (*numkeys <= 0) {
        return parse_error("ERR numkeys should be greater than 0");
      }
      if (static_cast<unsigned long long>(*numkeys) >
          command.args.size()) {
        return parse_error(syntax_error());
      }
      const auto key_count = static_cast<std::size_t>(*numkeys);
      const auto side_offset = numkeys_offset + 1 + key_count;
      if (side_offset >= command.args.size() ||
          (command.args.size() != side_offset + 1 &&
           command.args.size() != side_offset + 3)) {
        return parse_error(syntax_error());
      }
      if (!equals_ci(command.args[side_offset], "LEFT") &&
          !equals_ci(command.args[side_offset], "RIGHT")) {
        return parse_error(syntax_error());
      }
      command.list_key_offset = numkeys_offset + 1;
      command.list_key_count = key_count;
      command.list_pop_front = equals_ci(command.args[side_offset], "LEFT");
      if (command.args.size() == side_offset + 3) {
        if (!equals_ci(command.args[side_offset + 1], "COUNT")) {
          return parse_error(syntax_error());
        }
        const auto count = parse_ll(command.args[side_offset + 2]);
        if (!count) {
          return parse_error(integer_range_error());
        }
        if (*count <= 0) {
          return parse_error("ERR count should be greater than 0");
        }
        command.list_count = static_cast<std::size_t>(*count);
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    }
    case CommandType::llen:
    case CommandType::pma_llen:
    case CommandType::segmented_llen:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::lindex:
    case CommandType::pma_lindex:
    case CommandType::segmented_lindex:
      if (command.args.size() != 2) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::lrange:
    case CommandType::ltrim:
    case CommandType::pma_lrange:
    case CommandType::pma_ltrim:
    case CommandType::segmented_lrange:
    case CommandType::segmented_ltrim:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity(command.name));
      }
      if (!parse_range_indexes(command)) {
        return parse_error(integer_range_error());
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::lset:
    case CommandType::lrem:
    case CommandType::pma_lset:
    case CommandType::pma_lrem:
    case CommandType::segmented_lset:
    case CommandType::segmented_lrem:
      if (command.args.size() != 3) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::linsert:
    case CommandType::pma_linsert:
    case CommandType::segmented_linsert:
      if (command.args.size() != 4) {
        return parse_error(wrong_arity(command.name));
      }
      command.type = looked_up_type;
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
    case CommandType::scan:
      if (command.args.empty()) {
        return parse_error(wrong_arity("scan"));
      }
      command.type = CommandType::scan;
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
      command.type = looked_up_type;
      return {.command = std::move(command)};
    case CommandType::ttl:
    case CommandType::pttl:
    case CommandType::persist:
    case CommandType::expiretime:
    case CommandType::pexpiretime:
      if (command.args.size() != 1) {
        return parse_error(wrong_arity("ttl"));
      }
      command.type = looked_up_type;
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

bool command_mutates_store(CommandType type) noexcept {
  switch (type) {
    case CommandType::zadd:
    case CommandType::zincrby:
    case CommandType::zpopmin:
    case CommandType::zpopmax:
    case CommandType::zrem:
    case CommandType::zremrangebyscore:
    case CommandType::hset:
    case CommandType::hsetnx:
    case CommandType::hdel:
    case CommandType::hincrby:
    case CommandType::sadd:
    case CommandType::srem:
    case CommandType::spop:
    case CommandType::smove:
    case CommandType::sinterstore:
    case CommandType::sunionstore:
    case CommandType::sdiffstore:
    case CommandType::arreserve:
    case CommandType::arset:
    case CommandType::armset:
    case CommandType::ardel:
    case CommandType::arinsert:
    case CommandType::arseek:
    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::lmove:
    case CommandType::rpoplpush:
    case CommandType::blpop:
    case CommandType::brpop:
    case CommandType::blmove:
    case CommandType::lmpop:
    case CommandType::blmpop:
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
    case CommandType::pma_lmove:
    case CommandType::pma_rpoplpush:
    case CommandType::pma_blpop:
    case CommandType::pma_brpop:
    case CommandType::pma_blmove:
    case CommandType::pma_lmpop:
    case CommandType::pma_blmpop:
    case CommandType::pma_lset:
    case CommandType::pma_ltrim:
    case CommandType::pma_lrem:
    case CommandType::pma_linsert:
    case CommandType::segmented_lpush:
    case CommandType::segmented_rpush:
    case CommandType::segmented_lpushx:
    case CommandType::segmented_rpushx:
    case CommandType::segmented_lpop:
    case CommandType::segmented_rpop:
    case CommandType::segmented_lmove:
    case CommandType::segmented_rpoplpush:
    case CommandType::segmented_blpop:
    case CommandType::segmented_brpop:
    case CommandType::segmented_blmove:
    case CommandType::segmented_lmpop:
    case CommandType::segmented_blmpop:
    case CommandType::segmented_lset:
    case CommandType::segmented_ltrim:
    case CommandType::segmented_lrem:
    case CommandType::segmented_linsert:
    case CommandType::set:
    case CommandType::getset:
    case CommandType::setnx:
    case CommandType::getdel:
    case CommandType::append:
    case CommandType::incr:
    case CommandType::decr:
    case CommandType::incrby:
    case CommandType::decrby:
    case CommandType::incrbyfloat:
    case CommandType::setrange:
    case CommandType::mset:
    case CommandType::del:
    case CommandType::expire:
    case CommandType::pexpire:
    case CommandType::expireat:
    case CommandType::pexpireat:
    case CommandType::persist:
    case CommandType::goblin_cad:
    case CommandType::goblin_caexpire:
    case CommandType::goblin_cas:
    case CommandType::goblin_td_leaderboard_rescore:
    case CommandType::goblin_increx:
    case CommandType::goblin_zwindow:
    case CommandType::goblin_incrbound:
    case CommandType::goblin_decrpos:
    case CommandType::goblin_hcad:
    case CommandType::goblin_hsetgt:
    case CommandType::goblin_claim:
      return true;
    default:
      return false;
  }
}

// Public accessor for the INFO text so the SBE dispatch can reply it without
// duplicating build_info_string (which stays an internal helper here).
std::string render_server_info(const Store& store) { return build_info_string(store); }

// GOBLIN.MEMORY's flat [name, value, ...] fields for a zset/hash/list/set key
// (nullopt if the key is none of those), so the SBE dispatch can shape them into
// a map reply and the RESP path into an array -- one field list, two encodings.
std::optional<std::vector<std::string>> goblin_memory_fields(const Store& store,
                                                             std::string_view key) {
  if (const auto z = store.zset_memory_stats(key)) return memory_stats_fields(*z);
  if (const auto h = store.hash_memory_stats(key)) return hash_memory_stats_fields(*h);
  if (const auto l = store.list_memory_stats(key)) return list_memory_stats_fields(*l);
  if (const auto s = store.set_memory_stats(key)) return set_memory_stats_fields(*s);
  if (const auto a = store.array_memory_stats(key)) {
    return array_memory_stats_fields(*a);
  }
  return std::nullopt;
}

namespace {

constexpr std::string_view kCommandNames[] = {
#include "command_catalog.inc"
};

[[nodiscard]] bool valid_client_token(std::string_view value,
                                      bool allow_empty = true) noexcept {
  if (value.empty()) {
    return allow_empty;
  }
  if (value.size() > 256) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e;
  });
}

[[nodiscard]] bool authenticate(const CommandExecutionOptions& options,
                                std::string_view username,
                                std::string_view password,
                                std::string& out) {
  if (options.auth_database == nullptr) {
    resp::append_error(
        out,
        "ERR AUTH called without any password configured for this server");
    return false;
  }
  if (!options.auth_database->verify(username, password)) {
    resp::append_error(
        out,
        "WRONGPASS invalid username-password pair or user is disabled.");
    return false;
  }
  if (options.authenticated != nullptr) {
    *options.authenticated = true;
  }
  if (options.authenticated_username != nullptr) {
    options.authenticated_username->assign(username);
  }
  return true;
}

[[nodiscard]] std::string lowercase_ascii(std::string_view value) {
  std::string result(value);
  for (char& byte : result) {
    if (byte >= 'A' && byte <= 'Z') {
      byte = static_cast<char>(byte + ('a' - 'A'));
    }
  }
  return result;
}

[[nodiscard]] int command_arity(CommandType type) noexcept {
  switch (type) {
    case CommandType::quit:
    case CommandType::multi:
    case CommandType::exec:
    case CommandType::discard:
    case CommandType::unwatch:
      return 1;
    case CommandType::select:
    case CommandType::echo:
    case CommandType::get:
    case CommandType::getdel:
    case CommandType::strlen:
    case CommandType::incr:
    case CommandType::decr:
    case CommandType::key_type:
    case CommandType::ttl:
    case CommandType::pttl:
    case CommandType::persist:
    case CommandType::expiretime:
    case CommandType::pexpiretime:
    case CommandType::zcard:
    case CommandType::hlen:
    case CommandType::hgetall:
    case CommandType::hkeys:
    case CommandType::hvals:
    case CommandType::scard:
    case CommandType::smembers:
    case CommandType::llen:
    case CommandType::pma_llen:
    case CommandType::segmented_llen:
    case CommandType::arlen:
    case CommandType::arcount:
    case CommandType::arnext:
    case CommandType::goblin_memory:
    case CommandType::goblin_load:
    case CommandType::goblin_decrpos:
      return 2;
    case CommandType::setnx:
    case CommandType::getset:
    case CommandType::append:
    case CommandType::incrby:
    case CommandType::decrby:
    case CommandType::incrbyfloat:
    case CommandType::hget:
    case CommandType::hexists:
    case CommandType::hstrlen:
    case CommandType::sismember:
    case CommandType::lindex:
    case CommandType::pma_lindex:
    case CommandType::segmented_lindex:
    case CommandType::zscore:
    case CommandType::zrank:
    case CommandType::zrevrank:
    case CommandType::publish:
    case CommandType::arget:
    case CommandType::arseek:
    case CommandType::goblin_cad:
    case CommandType::goblin_increx:
    case CommandType::rpoplpush:
    case CommandType::pma_rpoplpush:
    case CommandType::segmented_rpoplpush:
      return 3;
    case CommandType::getrange:
    case CommandType::setrange:
    case CommandType::hincrby:
    case CommandType::hsetnx:
    case CommandType::lset:
    case CommandType::lrem:
    case CommandType::pma_lset:
    case CommandType::pma_lrem:
    case CommandType::segmented_lset:
    case CommandType::segmented_lrem:
    case CommandType::lrange:
    case CommandType::pma_lrange:
    case CommandType::segmented_lrange:
    case CommandType::ltrim:
    case CommandType::pma_ltrim:
    case CommandType::segmented_ltrim:
    case CommandType::smove:
    case CommandType::zincrby:
    case CommandType::zcount:
    case CommandType::zremrangebyscore:
    case CommandType::expire:
    case CommandType::pexpire:
    case CommandType::expireat:
    case CommandType::pexpireat:
    case CommandType::goblin_caexpire:
    case CommandType::goblin_cas:
    case CommandType::goblin_incrbound:
    case CommandType::goblin_hcad:
    case CommandType::goblin_hsetgt:
      return 4;
    case CommandType::arreserve:
    case CommandType::linsert:
    case CommandType::pma_linsert:
    case CommandType::segmented_linsert:
    case CommandType::goblin_claim:
    case CommandType::lmove:
    case CommandType::pma_lmove:
    case CommandType::segmented_lmove:
      return 5;
    case CommandType::goblin_td_leaderboard_rescore:
    case CommandType::goblin_zwindow:
    case CommandType::blmove:
    case CommandType::pma_blmove:
    case CommandType::segmented_blmove:
      return 6;
    case CommandType::ping:
    case CommandType::info:
    case CommandType::command:
    case CommandType::hello:
    case CommandType::unsubscribe:
    case CommandType::punsubscribe:
      return -1;
    case CommandType::auth:
    case CommandType::client:
    case CommandType::watch:
    case CommandType::subscribe:
    case CommandType::psubscribe:
    case CommandType::pubsub:
    case CommandType::spop:
    case CommandType::srandmember:
    case CommandType::sinter:
    case CommandType::sunion:
    case CommandType::sdiff:
    case CommandType::mget:
    case CommandType::del:
    case CommandType::exists:
    case CommandType::scan:
    case CommandType::goblin_optimize:
    case CommandType::goblin_save:
      return -2;
    case CommandType::eval:
    case CommandType::evalsha:
    case CommandType::luau_eval:
    case CommandType::luau_evalsha:
    case CommandType::wren_eval:
    case CommandType::wren_evalsha:
    case CommandType::tcl_eval:
    case CommandType::tcl_evalsha:
    case CommandType::upython_eval:
    case CommandType::upython_evalsha:
    case CommandType::quickjs_eval:
    case CommandType::quickjs_evalsha:
    case CommandType::zrem:
    case CommandType::zmscore:
    case CommandType::zscan:
    case CommandType::hscan:
    case CommandType::hmget:
    case CommandType::hdel:
    case CommandType::sadd:
    case CommandType::srem:
    case CommandType::smismember:
    case CommandType::sinterstore:
    case CommandType::sintercard:
    case CommandType::sunionstore:
    case CommandType::sdiffstore:
    case CommandType::sscan:
    case CommandType::armget:
    case CommandType::ardel:
    case CommandType::arinsert:
    case CommandType::lpush:
    case CommandType::rpush:
    case CommandType::lpushx:
    case CommandType::rpushx:
    case CommandType::pma_lpush:
    case CommandType::pma_rpush:
    case CommandType::pma_lpushx:
    case CommandType::pma_rpushx:
    case CommandType::segmented_lpush:
    case CommandType::segmented_rpush:
    case CommandType::segmented_lpushx:
    case CommandType::segmented_rpushx:
    case CommandType::set:
    case CommandType::mset:
    case CommandType::blpop:
    case CommandType::brpop:
    case CommandType::pma_blpop:
    case CommandType::pma_brpop:
    case CommandType::segmented_blpop:
    case CommandType::segmented_brpop:
      return -3;
    case CommandType::lmpop:
    case CommandType::pma_lmpop:
    case CommandType::segmented_lmpop:
      return -4;
    case CommandType::blmpop:
    case CommandType::pma_blmpop:
    case CommandType::segmented_blmpop:
      return -5;
    case CommandType::script:
    case CommandType::luau_script:
    case CommandType::wren_script:
    case CommandType::tcl_script:
    case CommandType::upython_script:
    case CommandType::quickjs_script:
      return -2;
    case CommandType::zadd:
    case CommandType::hset:
    case CommandType::arset:
    case CommandType::armset:
      return -4;
    case CommandType::zrange:
    case CommandType::zrangebyscore:
    case CommandType::zrevrangebyscore:
    case CommandType::zrevrange:
      return -4;
    case CommandType::zpopmin:
    case CommandType::zpopmax:
    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::pma_lpop:
    case CommandType::pma_rpop:
    case CommandType::segmented_lpop:
    case CommandType::segmented_rpop:
      return -2;
    case CommandType::unknown:
      return 0;
  }
  return 0;
}

struct CommandKeyRange {
  int first{0};
  int last{0};
  int step{0};
};

[[nodiscard]] CommandKeyRange command_key_range(CommandType type) noexcept {
  switch (type) {
    case CommandType::eval:
    case CommandType::evalsha:
    case CommandType::luau_eval:
    case CommandType::luau_evalsha:
    case CommandType::wren_eval:
    case CommandType::wren_evalsha:
    case CommandType::tcl_eval:
    case CommandType::tcl_evalsha:
    case CommandType::upython_eval:
    case CommandType::upython_evalsha:
    case CommandType::quickjs_eval:
    case CommandType::quickjs_evalsha:
      return {3, -1, 1};
    case CommandType::sintercard:
      return {2, -1, 1};
    case CommandType::mset:
      return {1, -1, 2};
    case CommandType::mget:
    case CommandType::del:
    case CommandType::exists:
    case CommandType::sinter:
    case CommandType::sunion:
    case CommandType::sdiff:
    case CommandType::sinterstore:
    case CommandType::sunionstore:
    case CommandType::sdiffstore:
      return {1, -1, 1};
    case CommandType::smove:
      return {1, 2, 1};
    case CommandType::lmove:
    case CommandType::rpoplpush:
    case CommandType::blmove:
    case CommandType::pma_lmove:
    case CommandType::pma_rpoplpush:
    case CommandType::pma_blmove:
    case CommandType::segmented_lmove:
    case CommandType::segmented_rpoplpush:
    case CommandType::segmented_blmove:
      return {1, 2, 1};
    case CommandType::blpop:
    case CommandType::brpop:
    case CommandType::pma_blpop:
    case CommandType::pma_brpop:
    case CommandType::segmented_blpop:
    case CommandType::segmented_brpop:
      return {1, -2, 1};
    case CommandType::lmpop:
    case CommandType::blmpop:
    case CommandType::pma_lmpop:
    case CommandType::pma_blmpop:
    case CommandType::segmented_lmpop:
    case CommandType::segmented_blmpop:
      return {};
    case CommandType::watch:
      return {1, -1, 1};
    default:
      return command_has_key_arg(type) ? CommandKeyRange{1, 1, 1}
                                       : CommandKeyRange{};
  }
}

[[nodiscard]] bool connection_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::auth:
    case CommandType::hello:
    case CommandType::command:
    case CommandType::client:
    case CommandType::select:
    case CommandType::quit:
    case CommandType::ping:
    case CommandType::echo:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool scripting_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::eval:
    case CommandType::evalsha:
    case CommandType::script:
    case CommandType::luau_eval:
    case CommandType::luau_evalsha:
    case CommandType::luau_script:
    case CommandType::wren_eval:
    case CommandType::wren_evalsha:
    case CommandType::wren_script:
    case CommandType::tcl_eval:
    case CommandType::tcl_evalsha:
    case CommandType::tcl_script:
    case CommandType::upython_eval:
    case CommandType::upython_evalsha:
    case CommandType::upython_script:
    case CommandType::quickjs_eval:
    case CommandType::quickjs_evalsha:
    case CommandType::quickjs_script:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool transaction_command(CommandType type) noexcept {
  return type == CommandType::multi || type == CommandType::exec ||
         type == CommandType::discard || type == CommandType::watch ||
         type == CommandType::unwatch;
}

[[nodiscard]] bool command_may_write(CommandType type) noexcept {
  if (command_mutates_store(type) || type == CommandType::goblin_load) {
    return true;
  }
  switch (type) {
    case CommandType::eval:
    case CommandType::evalsha:
    case CommandType::luau_eval:
    case CommandType::luau_evalsha:
    case CommandType::wren_eval:
    case CommandType::wren_evalsha:
    case CommandType::tcl_eval:
    case CommandType::tcl_evalsha:
    case CommandType::upython_eval:
    case CommandType::upython_evalsha:
    case CommandType::quickjs_eval:
    case CommandType::quickjs_evalsha:
      return true;
    default:
      return false;
  }
}

void append_command_descriptor(std::string& out, std::string_view name,
                               CommandType type) {
  const auto keys = command_key_range(type);
  const bool writes = command_may_write(type);
  const bool pubsub = type == CommandType::subscribe ||
                      type == CommandType::unsubscribe ||
                      type == CommandType::psubscribe ||
                      type == CommandType::punsubscribe ||
                      type == CommandType::publish ||
                      type == CommandType::pubsub;
  std::string_view command_flag = "readonly";
  std::string_view acl_category = "@read";
  if (pubsub) {
    command_flag = "pubsub";
    acl_category = "@pubsub";
  } else if (scripting_command(type)) {
    command_flag = writes ? "write" : "readonly";
    acl_category = "@scripting";
  } else if (transaction_command(type)) {
    command_flag = "fast";
    acl_category = "@transaction";
  } else if (connection_command(type)) {
    command_flag = "fast";
    acl_category = "@connection";
  } else if (writes) {
    command_flag = "write";
    acl_category = "@write";
  }

  // Redis 7+ descriptor shape: legacy first-key metadata followed by ACL
  // categories, tips, key specs, and subcommands. Key specs remain empty because
  // the legacy fields fully describe Goblin's current command set.
  resp::append_array_header(out, 10);
  resp::append_bulk_string(out, lowercase_ascii(name));
  resp::append_integer(out, command_arity(type));
  resp::append_array_header(out, 1);
  resp::append_bulk_string(out, command_flag);
  resp::append_integer(out, keys.first);
  resp::append_integer(out, keys.last);
  resp::append_integer(out, keys.step);
  resp::append_array_header(out, 1);
  resp::append_bulk_string(out, acl_category);
  resp::append_array_header(out, 0);  // tips
  resp::append_array_header(out, 0);  // key specifications
  resp::append_array_header(out, 0);  // subcommands
}

void execute_command_introspection(const Command& command, std::string& out,
                                   resp::Version version) {
  if (command.args.empty()) {
    resp::append_array_header(out, std::size(kCommandNames));
    for (const auto name : kCommandNames) {
      append_command_descriptor(out, name, lookup_command_type(name));
    }
    return;
  }
  if (!equals_ci(command.args.front(), "INFO")) {
    resp::append_error(out, "ERR unknown subcommand for COMMAND");
    return;
  }
  resp::append_array_header(out, command.args.size() - 1);
  for (const auto name : command.args.subspan(1)) {
    const auto type = lookup_command_type(name);
    if (type == CommandType::unknown) {
      resp::append_null(out, version);
    } else {
      append_command_descriptor(out, name, type);
    }
  }
}

}  // namespace

void execute_command_into(Store& store,
                          const Command& command,
                          std::string& out,
                          CommandExecutionOptions options) {
  const auto type =
      resolve_list_command(command.type, store.list_implementation());
  const auto version = response_version(options);
  // Prefer the parse-time tag (L); fall back to name scan for hand-built Commands.
  // GOBLIN.RT.* / GOBLIN.EFFICENT.* select hash implementation (not CLASSIC).
  const std::optional<HashImplementation> hash_implementation =
      command.hash_implementation_tag == 2
          ? std::optional{HashImplementation::Realtime}
      : starts_with_ci(command.name, "GOBLIN.EFFICENT.")
          ? std::optional{HashImplementation::Efficient}
          : qualified_hash_implementation(command.name);
  // GOBLIN.RT.AR* → RT, GOBLIN.CLASSIC.AR* → Classic (Redis 8.8-style arrays).
  const ArrayImplementation array_implementation =
      command.hash_implementation_tag == 2 ? ArrayImplementation::Realtime
      : starts_with_ci(command.name, "GOBLIN.CLASSIC.")
          ? ArrayImplementation::Classic
          : store.array_implementation();
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
    case CommandType::auth: {
      const std::string_view username =
          command.args.size() == 1 ? std::string_view("default")
                                   : command.args[0];
      const std::string_view password = command.args.back();
      if (authenticate(options, username, password, out)) {
        resp::append_simple_string(out, "OK");
      }
      return;
    }
    case CommandType::hello: {
      auto selected = version;
      if (command.args.empty()) {
        append_hello_response(out, selected, options.connection_id);
        return;
      }

      const auto requested = parse_i64(command.args.front());
      if (!requested || (*requested != 2 && *requested != 3)) {
        resp::append_error(out, "NOPROTO unsupported protocol version");
        return;
      }
      selected = *requested == 3 ? resp::Version::resp3
                                 : resp::Version::resp2;

      bool saw_auth = false;
      bool saw_setname = false;
      std::string_view auth_username;
      std::string_view auth_password;
      std::string_view requested_name;
      std::size_t index = 1;
      while (index < command.args.size()) {
        if (equals_ci(command.args[index], "AUTH") && !saw_auth &&
            index + 2 < command.args.size()) {
          saw_auth = true;
          auth_username = command.args[index + 1];
          auth_password = command.args[index + 2];
          index += 3;
          continue;
        }
        if (equals_ci(command.args[index], "SETNAME") && !saw_setname &&
            index + 1 < command.args.size()) {
          saw_setname = true;
          requested_name = command.args[index + 1];
          index += 2;
          continue;
        }
        resp::append_error(out, syntax_error());
        return;
      }
      if (saw_setname && !valid_client_token(requested_name)) {
        resp::append_error(
            out,
            "ERR Client names cannot contain spaces, newlines or special characters.");
        return;
      }
      if (saw_auth &&
          !authenticate(options, auth_username, auth_password, out)) {
        return;
      }
      if (options.resp_version != nullptr) {
        *options.resp_version = selected;
      }
      if (saw_setname && options.client_name != nullptr) {
        options.client_name->assign(requested_name);
      }
      append_hello_response(out, selected, options.connection_id);
      return;
    }
    case CommandType::command:
      execute_command_introspection(command, out, version);
      return;
    case CommandType::client: {
      const auto subcommand = command.args.front();
      if (equals_ci(subcommand, "SETNAME")) {
        if (command.args.size() != 2) {
          resp::append_error(out, wrong_arity("client|setname"));
          return;
        }
        if (!valid_client_token(command.args[1])) {
          resp::append_error(
              out,
              "ERR Client names cannot contain spaces, newlines or special characters.");
          return;
        }
        if (options.client_name != nullptr) {
          options.client_name->assign(command.args[1]);
        }
        resp::append_simple_string(out, "OK");
        return;
      }
      if (equals_ci(subcommand, "GETNAME")) {
        if (command.args.size() != 1) {
          resp::append_error(out, wrong_arity("client|getname"));
          return;
        }
        if (options.client_name == nullptr || options.client_name->empty()) {
          resp::append_null(out, version);
        } else {
          resp::append_bulk_string(out, *options.client_name);
        }
        return;
      }
      if (equals_ci(subcommand, "ID")) {
        if (command.args.size() != 1) {
          resp::append_error(out, wrong_arity("client|id"));
          return;
        }
        resp::append_integer(out, static_cast<long long>(options.connection_id));
        return;
      }
      if (equals_ci(subcommand, "SETINFO")) {
        if (command.args.size() != 3) {
          resp::append_error(out, wrong_arity("client|setinfo"));
          return;
        }
        if (!valid_client_token(command.args[2], false)) {
          resp::append_error(out, "ERR invalid client library information");
          return;
        }
        if (equals_ci(command.args[1], "LIB-NAME")) {
          if (options.client_library_name != nullptr) {
            options.client_library_name->assign(command.args[2]);
          }
        } else if (equals_ci(command.args[1], "LIB-VER")) {
          if (options.client_library_version != nullptr) {
            options.client_library_version->assign(command.args[2]);
          }
        } else {
          resp::append_error(out, "ERR unknown CLIENT SETINFO option");
          return;
        }
        resp::append_simple_string(out, "OK");
        return;
      }
      resp::append_error(out, "ERR unknown subcommand for CLIENT");
      return;
    }
    case CommandType::select: {
      const auto database = parse_i64(command.args.front());
      if (!database) {
        resp::append_error(out, integer_range_error());
      } else if (*database != 0) {
        resp::append_error(out, "ERR DB index is out of range");
      } else {
        resp::append_simple_string(out, "OK");
      }
      return;
    }
    case CommandType::quit:
      resp::append_simple_string(out, "OK");
      if (options.quit_requested != nullptr) {
        *options.quit_requested = true;
      }
      return;
    case CommandType::multi:
    case CommandType::exec:
    case CommandType::discard:
    case CommandType::watch:
    case CommandType::unwatch:
      resp::append_error(out,
                         "ERR transaction command requires a live client connection");
      return;
    case CommandType::subscribe:
    case CommandType::unsubscribe:
    case CommandType::psubscribe:
    case CommandType::punsubscribe:
    case CommandType::pubsub:
      resp::append_error(out, "ERR Pub/Sub requires a live client connection");
      return;
    case CommandType::publish:
      if (options.nested_dispatch.publish == nullptr) {
        resp::append_error(out, "ERR Pub/Sub requires a live client connection");
      } else {
        resp::append_integer(
            out, options.nested_dispatch.publish(options.nested_dispatch.context,
                                                 command.args[0], command.args[1]));
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
      ZAddOptions add_options;
      bool return_changed = false;
      std::size_t first_score = 1;
      for (; first_score < command.args.size(); ++first_score) {
        const auto option = command.args[first_score];
        bool* flag = nullptr;
        if (equals_ci(option, "NX")) flag = &add_options.nx;
        else if (equals_ci(option, "XX")) flag = &add_options.xx;
        else if (equals_ci(option, "GT")) flag = &add_options.gt;
        else if (equals_ci(option, "LT")) flag = &add_options.lt;
        else if (equals_ci(option, "CH")) {
          // CH changes only the integer reply, not the storage condition.
          if (return_changed) {
            resp::append_error(out, syntax_error());
            return;
          }
          return_changed = true;
          continue;
        } else if (equals_ci(option, "INCR")) flag = &add_options.increment;
        else break;
        if (*flag) {
          resp::append_error(out, syntax_error());
          return;
        }
        *flag = true;
      }
      if ((add_options.nx &&
           (add_options.xx || add_options.gt || add_options.lt)) ||
          (add_options.gt && add_options.lt) ||
          first_score >= command.args.size() ||
          (command.args.size() - first_score) % 2 != 0 ||
          (add_options.increment &&
           command.args.size() - first_score != 2)) {
        resp::append_error(out, syntax_error());
        return;
      }

      static thread_local std::vector<ZAddItem> items;
      items.clear();
      items.reserve((command.args.size() - first_score) / 2);
      for (std::size_t i = first_score; i < command.args.size(); i += 2) {
        const auto score = parse_zset_score(command.args[i]);
        if (!score) {
          resp::append_error(out, "ERR value is not a valid float");
          return;
        }
        items.push_back(ZAddItem{.score = *score,
                                 .member = command.args[i + 1]});
      }
      const auto result = store.zadd(key, items, add_options);
      if (result.invalid_score) {
        resp::append_error(out, "ERR resulting score is not a number (NaN)");
        return;
      }
      if (add_options.increment) {
        if (!result.increment_score) {
          resp::append_null(out, version);
        } else if (version == resp::Version::resp3) {
          resp::append_double(out, *result.increment_score);
        } else {
          resp::append_bulk_double(out, *result.increment_score);
        }
        return;
      }
      resp::append_integer(out, return_changed ? result.changed : result.added);
      return;
    }

    case CommandType::zincrby: {
      const auto increment = parse_zset_score(command.args[1]);
      if (!increment) {
        resp::append_error(out, "ERR value is not a valid float");
        return;
      }
      const ZAddItem item{.score = *increment,
                          .member = command.args[2]};
      const auto result = store.zadd(
          command.args[0], std::span<const ZAddItem>(&item, 1),
          ZAddOptions{.increment = true});
      if (result.invalid_score) {
        resp::append_error(out, "ERR resulting score is not a number (NaN)");
      } else if (version == resp::Version::resp3) {
        resp::append_double(out, *result.increment_score);
      } else {
        resp::append_bulk_double(out, *result.increment_score);
      }
      return;
    }

    case CommandType::zcard:
      resp::append_integer(out, store.zcard(command.args[0]));
      return;

    case CommandType::zcount: {
      const auto lo = parse_score_bound(command.args[1]);
      const auto hi = parse_score_bound(command.args[2]);
      if (!lo || !hi) {
        resp::append_error(out, "ERR min or max is not a float");
        return;
      }
      resp::append_integer(out, store.zcount(command.args[0], lo->value,
                                             lo->exclusive, hi->value,
                                             hi->exclusive));
      return;
    }

    case CommandType::zrange:
    case CommandType::zrangebyscore:
    case CommandType::zrevrangebyscore:
      append_range_response(store, command, command.range_reverse, out, options);
      return;

    case CommandType::zrank: {
      const auto rank = store.zrank(command.args[0], command.args[1]);
      if (!rank) {
        resp::append_null(out, version);
      } else {
        resp::append_integer(out, static_cast<long long>(*rank));
      }
      return;
    }

    case CommandType::zrevrange:
      append_range_response(store, command, command.range_reverse, out, options);
      return;

    case CommandType::zrevrank: {
      const auto rank = store.zrevrank(command.args[0], command.args[1]);
      if (!rank) {
        resp::append_null(out, version);
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

    case CommandType::zmscore: {
      const auto members = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      resp::append_array_header(out, members.size());
      store.zmscore_for_each(command.args[0], members,
                             [&out, version](std::optional<double> score) {
        if (!score) {
          resp::append_null(out, version);
        } else if (version == resp::Version::resp3) {
          resp::append_double(out, *score);
        } else {
          resp::append_bulk_double(out, *score);
        }
      });
      return;
    }

    case CommandType::zpopmin:
    case CommandType::zpopmax: {
      std::uint64_t count = 1;
      if (command.args.size() == 2) {
        const auto parsed = parse_u64(command.args[1]);
        if (!parsed || *parsed > std::numeric_limits<std::size_t>::max()) {
          resp::append_error(out, integer_range_error());
          return;
        }
        count = *parsed;
      }
      const auto entries = store.zpop(
          command.args[0], static_cast<std::size_t>(count),
          command.type == CommandType::zpopmax);
      if (version == resp::Version::resp3) {
        resp::append_array_header(out, entries.size());
        for (const auto& entry : entries) {
          resp::append_array_header(out, 2);
          resp::append_bulk_string(out, entry.member);
          resp::append_double(out, entry.score);
        }
      } else {
        resp::append_array_header(out, entries.size() * 2);
        for (const auto& entry : entries) {
          resp::append_bulk_string(out, entry.member);
          resp::append_bulk_double(out, entry.score);
        }
      }
      return;
    }

    case CommandType::zscan: {
      const auto cursor = parse_u64(command.args[1]);
      if (!cursor) {
        resp::append_error(out, "ERR invalid cursor");
        return;
      }
      std::string_view pattern;
      bool has_pattern = false;
      std::size_t count = 10;
      for (std::size_t i = 2; i < command.args.size();) {
        if (equals_ci(command.args[i], "MATCH") &&
            i + 1 < command.args.size()) {
          pattern = command.args[i + 1];
          has_pattern = true;
          i += 2;
          continue;
        }
        if (equals_ci(command.args[i], "COUNT") &&
            i + 1 < command.args.size()) {
          const auto parsed = parse_u64(command.args[i + 1]);
          if (!parsed || *parsed == 0 ||
              *parsed > std::numeric_limits<std::size_t>::max()) {
            resp::append_error(out, syntax_error());
            return;
          }
          count = static_cast<std::size_t>(*parsed);
          i += 2;
          continue;
        }
        resp::append_error(out, syntax_error());
        return;
      }

      static thread_local std::vector<std::pair<std::string_view, double>>
          matches;
      matches.clear();
      const auto next = store.zscan(
          command.args[0], *cursor, count,
          [pattern, has_pattern](std::string_view member, double score) {
            if (!has_pattern || scan_glob_match(pattern, member)) {
              matches.emplace_back(member, score);
            }
          });
      resp::append_array_header(out, 2);
      resp::append_bulk_string(out, std::to_string(next));
      resp::append_array_header(out, matches.size() * 2);
      for (const auto& [member, score] : matches) {
        resp::append_bulk_string(out, member);
        resp::append_bulk_double(out, score);
      }
      return;
    }

    case CommandType::zscore: {
      const auto score = store.zscore(command.args[0], command.args[1]);
      if (!score) {
        resp::append_null(out, version);
      } else if (version == resp::Version::resp3) {
        resp::append_double(out, *score);
      } else {
        resp::append_bulk_double(out, *score);
      }
      return;
    }

    case CommandType::hset: {
      const auto& key = command.args[0];
      for (std::size_t index = 1; index + 1 < command.args.size(); index += 2) {
        if (command.args[index].size() > HashStorage::kMaxFieldBytes ||
            !store.value_fits(command.args[index + 1])) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      // Common case: HSET k f v -- skip the multi-field vector.
      if (command.args.size() == 3) {
        resp::append_integer(
            out, store.hset(key, command.args[1], command.args[2],
                            hash_implementation));
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
      resp::append_integer(out,
                           store.hset_many(key, pairs, hash_implementation));
      return;
    }

    case CommandType::hsetnx:
      if (command.args[1].size() > HashStorage::kMaxFieldBytes ||
          !store.value_fits(command.args[2])) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(
          out, store.hsetnx(command.args[0], command.args[1], command.args[2],
                            hash_implementation));
      return;

    case CommandType::hget: {
      const auto value = store.hget(command.args[0], command.args[1]);
      if (!value) {
        resp::append_null(out, version);
      } else {
        resp::append_bulk_string(out, *value);
      }
      return;
    }

    case CommandType::hmget: {
      resp::append_array_header(out, command.args.size() - 1);
      const auto fields = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      store.hash_mget(command.args[0], fields,
                      [&](std::optional<EncodedStringView> value) {
                        if (!value) {
                          resp::append_null(out, version);
                        } else {
                          resp::append_bulk_string(out, *value);
                        }
                      });
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
      store.hash_for_each_sized(
          command.args[0],
          [&](std::size_t count) {
            if (version == resp::Version::resp3) {
              resp::append_map_header(out, count);
            } else {
              resp::append_array_header(out, count * 2);
            }
          },
          [&out](std::string_view field, EncodedStringView value) {
            resp::append_bulk_string(out, field);
            resp::append_bulk_string(out, value);
          });
      return;
    }

    case CommandType::hkeys: {
      store.hash_for_each_sized(
          command.args[0],
          [&](std::size_t count) { resp::append_array_header(out, count); },
          [&out](std::string_view field, EncodedStringView) {
            resp::append_bulk_string(out, field);
          });
      return;
    }

    case CommandType::hvals: {
      store.hash_for_each_sized(
          command.args[0],
          [&](std::size_t count) { resp::append_array_header(out, count); },
          [&out](std::string_view, EncodedStringView value) {
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
      const auto result = store.hincrby(command.args[0], command.args[1], *delta,
                                        hash_implementation);
      if (!result) {
        resp::append_error(
            out, "ERR hash value is not an integer or out of range");
      } else {
        resp::append_integer(out, *result);
      }
      return;
    }

    case CommandType::hscan: {
      const auto cursor = parse_u64(command.args[1]);
      if (!cursor) {
        resp::append_error(out, "ERR invalid cursor");
        return;
      }
      std::string_view pattern;
      bool has_pattern = false;
      bool no_values = false;
      std::size_t count = 10;
      for (std::size_t i = 2; i < command.args.size();) {
        if (equals_ci(command.args[i], "MATCH") &&
            i + 1 < command.args.size()) {
          pattern = command.args[i + 1];
          has_pattern = true;
          i += 2;
          continue;
        }
        if (equals_ci(command.args[i], "COUNT") &&
            i + 1 < command.args.size()) {
          const auto parsed = parse_u64(command.args[i + 1]);
          if (!parsed || *parsed == 0 ||
              *parsed > std::numeric_limits<std::size_t>::max()) {
            resp::append_error(out, syntax_error());
            return;
          }
          count = static_cast<std::size_t>(*parsed);
          i += 2;
          continue;
        }
        if (equals_ci(command.args[i], "NOVALUES")) {
          no_values = true;
          ++i;
          continue;
        }
        resp::append_error(out, syntax_error());
        return;
      }

      std::string page;
      std::size_t item_count = 0;
      const auto next = store.hscan(
          command.args[0], *cursor, count,
          [&](std::string_view field) {
            return !has_pattern || scan_glob_match(pattern, field);
          },
          [&](std::string_view field, EncodedStringView value) {
            resp::append_bulk_string(page, field);
            ++item_count;
            if (!no_values) {
              resp::append_bulk_string(page, value);
              ++item_count;
            }
          });
      resp::append_array_header(out, 2);
      resp::append_bulk_string(out, std::to_string(next));
      resp::append_array_header(out, item_count);
      out.append(page);
      return;
    }

    case CommandType::sadd: {
      for (std::size_t index = 1; index < command.args.size(); ++index) {
        if (!store.value_fits(command.args[index])) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      const auto members = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      resp::append_integer(out, store.sadd(command.args[0], members));
      return;
    }

    case CommandType::srem: {
      const auto members = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      resp::append_integer(out, store.srem(command.args[0], members));
      return;
    }

    case CommandType::scard:
      resp::append_integer(out, store.scard(command.args[0]));
      return;

    case CommandType::sismember:
      resp::append_integer(
          out, store.sismember(command.args[0], command.args[1]) ? 1 : 0);
      return;

    case CommandType::smismember: {
      const auto members = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      resp::append_array_header(out, members.size());
      store.smismember(command.args[0], members, [&](bool present) {
        resp::append_integer(out, present ? 1 : 0);
      });
      return;
    }

    case CommandType::smembers: {
      store.smembers_for_each(
          command.args[0],
          [&](std::size_t count) { resp::append_array_header(out, count); },
          [&out](EncodedStringView member) {
            resp::append_bulk_string(out, member);
          });
      return;
    }

    case CommandType::spop: {
      if (command.args.size() == 1) {
        const auto member = store.spop(command.args[0]);
        if (!member) {
          resp::append_null(out, version);
        } else {
          resp::append_bulk_string(out, *member);
        }
        return;
      }
      const auto count = parse_ll(command.args[1]);
      if (!count || *count < 0) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto members =
          store.spop(command.args[0], static_cast<std::size_t>(*count));
      resp::append_array_header(out, members.size());
      for (const auto& member : members) {
        resp::append_bulk_string(out, member);
      }
      return;
    }

    case CommandType::srandmember: {
      if (command.args.size() == 1) {
        const auto member = store.srandmember(command.args[0]);
        if (!member) {
          resp::append_null(out, version);
        } else {
          resp::append_bulk_string(out, *member);
        }
        return;
      }
      const auto count = parse_ll(command.args[1]);
      if (!count) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (*count >= 0) {
        const auto members = store.srandmember(
            command.args[0], static_cast<std::size_t>(*count), /*unique=*/true);
        resp::append_array_header(out, members.size());
        for (const auto& member : members) {
          resp::append_bulk_string(out, member);
        }
      } else {
        const auto n = static_cast<std::size_t>(-*count);
        const auto members =
            store.srandmember(command.args[0], n, /*unique=*/false);
        resp::append_array_header(out, members.size());
        for (const auto& member : members) {
          resp::append_bulk_string(out, member);
        }
      }
      return;
    }

    case CommandType::smove: {
      // Destination must also be a set-or-missing (source already gated).
      if (!store.ttl_empty()) {
        (void)store.purge_if_expired(command.args[1], store.now_ms());
      }
      if (const auto actual = store.key_type(command.args[1]);
          actual.has_value() && *actual != KeyType::Set) {
        resp::append_error(out, kWrongType);
        return;
      }
      if (!store.value_fits(command.args[2])) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(
          out, store.smove(command.args[0], command.args[1], command.args[2]));
      return;
    }

    case CommandType::sinter:
    case CommandType::sunion:
    case CommandType::sdiff: {
      const auto keys = std::span<const std::string_view>(command.args);
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      for (std::size_t i = 0; i < keys.size(); ++i) {
        if (!store.ttl_empty()) {
          (void)store.purge_if_expired(keys[i], now);
        }
        if (const auto actual = store.key_type(keys[i]);
            actual.has_value() && *actual != KeyType::Set) {
          resp::append_error(out, kWrongType);
          return;
        }
      }
      const auto members =
          type == CommandType::sinter   ? store.sinter(keys)
          : type == CommandType::sunion ? store.sunion(keys)
                                       : store.sdiff(keys);
      resp::append_array_header(out, members.size());
      for (const auto& member : members) {
        resp::append_bulk_string(out, member);
      }
      return;
    }

    case CommandType::sinterstore:
    case CommandType::sunionstore:
    case CommandType::sdiffstore: {
      const auto destination = command.args[0];
      const auto keys = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      for (const auto key : keys) {
        if (!store.ttl_empty()) {
          (void)store.purge_if_expired(key, now);
        }
        if (const auto actual = store.key_type(key);
            actual.has_value() && *actual != KeyType::Set) {
          resp::append_error(out, kWrongType);
          return;
        }
      }
      const auto stored =
          type == CommandType::sinterstore   ? store.sinterstore(destination, keys)
          : type == CommandType::sunionstore ? store.sunionstore(destination, keys)
                                            : store.sdiffstore(destination, keys);
      resp::append_integer(out, stored);
      return;
    }

    case CommandType::sintercard: {
      // SINTERCARD numkeys key [key ...] [LIMIT limit]
      const auto numkeys = parse_ll(command.args[0]);
      if (!numkeys || *numkeys < 1) {
        resp::append_error(out, "ERR numkeys should be greater than 0");
        return;
      }
      const auto nkeys = static_cast<std::size_t>(*numkeys);
      if (command.args.size() < 1 + nkeys) {
        resp::append_error(out, wrong_arity("sintercard"));
        return;
      }
      std::size_t limit = 0;
      std::size_t pos = 1 + nkeys;
      if (pos < command.args.size()) {
        if (pos + 1 >= command.args.size() ||
            !equals_ci(command.args[pos], "LIMIT")) {
          resp::append_error(out, syntax_error());
          return;
        }
        const auto lim = parse_ll(command.args[pos + 1]);
        if (!lim || *lim < 0) {
          resp::append_error(out, integer_range_error());
          return;
        }
        limit = static_cast<std::size_t>(*lim);
        pos += 2;
      }
      if (pos != command.args.size()) {
        resp::append_error(out, syntax_error());
        return;
      }
      const auto keys =
          std::span<const std::string_view>(command.args.data() + 1, nkeys);
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      for (const auto key : keys) {
        if (!store.ttl_empty()) {
          (void)store.purge_if_expired(key, now);
        }
        if (const auto actual = store.key_type(key);
            actual.has_value() && *actual != KeyType::Set) {
          resp::append_error(out, kWrongType);
          return;
        }
      }
      resp::append_integer(out, store.sintercard(keys, limit));
      return;
    }

    case CommandType::sscan: {
      // SSCAN key cursor [MATCH pattern] [COUNT count]
      const auto cursor = parse_ll(command.args[1]);
      if (!cursor || *cursor < 0) {
        resp::append_error(out, "ERR invalid cursor");
        return;
      }
      std::string_view pattern;
      std::size_t count = 10;
      for (std::size_t i = 2; i < command.args.size();) {
        if (equals_ci(command.args[i], "MATCH")) {
          if (i + 1 >= command.args.size()) {
            resp::append_error(out, syntax_error());
            return;
          }
          pattern = command.args[i + 1];
          i += 2;
        } else if (equals_ci(command.args[i], "COUNT")) {
          if (i + 1 >= command.args.size()) {
            resp::append_error(out, syntax_error());
            return;
          }
          const auto c = parse_ll(command.args[i + 1]);
          if (!c || *c < 1) {
            resp::append_error(out, integer_range_error());
            return;
          }
          count = static_cast<std::size_t>(*c);
          i += 2;
        } else {
          resp::append_error(out, syntax_error());
          return;
        }
      }
      std::vector<std::string> members;
      const auto next = store.sscan(
          command.args[0], static_cast<std::uint64_t>(*cursor), count,
          [&](std::string_view member) {
            return pattern.empty() || scan_glob_match(pattern, member);
          },
          [&](std::string member) { members.push_back(std::move(member)); });
      // Reply: [next_cursor, [members...]]
      resp::append_array_header(out, 2);
      resp::append_bulk_string(out, std::to_string(next));
      resp::append_array_header(out, members.size());
      for (const auto& member : members) {
        resp::append_bulk_string(out, member);
      }
      return;
    }

    case CommandType::arreserve: {
      const auto max_index = parse_ll(command.args[1]);
      const auto value_capacity = parse_ll(command.args[2]);
      const auto value_bytes = parse_ll(command.args[3]);
      if (!max_index || *max_index < 0 || !value_capacity ||
          *value_capacity <= 0 ||
          static_cast<unsigned long long>(*value_capacity) >=
              static_cast<unsigned long long>(ArrayStorage::kEmptyId) ||
          !value_bytes || *value_bytes <= 0 ||
          static_cast<unsigned long long>(*value_bytes) >
              std::numeric_limits<std::uint32_t>::max()) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const bool created = store.find_array(command.args[0]) == nullptr;
      try {
        auto& array = store.get_or_create_array(
            command.args[0], ArrayImplementation::Realtime);
        array.reserve_realtime(
            static_cast<std::uint64_t>(*max_index),
            static_cast<std::size_t>(*value_capacity),
            static_cast<std::size_t>(*value_bytes));
        store.signal_key_modified(command.args[0]);
        resp::append_integer(out, 1);
      } catch (const std::length_error& ex) {
        if (created) {
          if (auto* array = store.find_array(command.args[0]); array != nullptr) {
            store.erase_if_empty(command.args[0], *array);
          }
        }
        resp::append_error(out, ex.what());
      }
      return;
    }

    case CommandType::arset: {
      const auto index = parse_ll(command.args[1]);
      if (!index || *index < 0) {
        resp::append_error(out, integer_range_error());
        return;
      }
      for (std::size_t i = 2; i < command.args.size(); ++i) {
        if (!store.value_fits(command.args[i])) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      try {
        auto& array =
            store.get_or_create_array(command.args[0], array_implementation);
        std::size_t written = 0;
        for (std::size_t i = 2; i < command.args.size(); ++i) {
          (void)array.set(static_cast<std::uint64_t>(*index) + written,
                          command.args[i]);
          ++written;
        }
        if (written != 0) {
          store.signal_key_modified(command.args[0]);
        }
        resp::append_integer(out, static_cast<long long>(written));
      } catch (const std::length_error& ex) {
        resp::append_error(out, ex.what());
      }
      return;
    }

    case CommandType::arget: {
      const auto index = parse_ll(command.args[1]);
      if (!index || *index < 0) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto* array = store.find_array(command.args[0]);
      if (array == nullptr) {
        resp::append_null(out, version);
        return;
      }
      const auto value = array->get(static_cast<std::uint64_t>(*index));
      if (!value) {
        resp::append_null(out, version);
      } else {
        resp::append_bulk_string(out, *value);
      }
      return;
    }

    case CommandType::armset: {
      try {
        auto& array =
            store.get_or_create_array(command.args[0], array_implementation);
        long long applied = 0;
        for (std::size_t i = 1; i + 1 < command.args.size(); i += 2) {
          const auto index = parse_ll(command.args[i]);
          if (!index || *index < 0) {
            resp::append_error(out, integer_range_error());
            return;
          }
          if (!store.value_fits(command.args[i + 1])) {
            resp::append_error(out, kValueTooLarge);
            return;
          }
          (void)array.set(static_cast<std::uint64_t>(*index),
                          command.args[i + 1]);
          ++applied;
          store.signal_key_modified(command.args[0]);
        }
        resp::append_integer(out, applied);
      } catch (const std::length_error& ex) {
        resp::append_error(out, ex.what());
      }
      return;
    }

    case CommandType::armget: {
      const auto* array = store.find_array(command.args[0]);
      resp::append_array_header(out, command.args.size() - 1);
      for (std::size_t i = 1; i < command.args.size(); ++i) {
        const auto index = parse_ll(command.args[i]);
        if (!index || *index < 0) {
          resp::append_error(out, integer_range_error());
          return;
        }
        if (array == nullptr) {
          resp::append_null(out, version);
          continue;
        }
        const auto value = array->get(static_cast<std::uint64_t>(*index));
        if (!value) {
          resp::append_null(out, version);
        } else {
          resp::append_bulk_string(out, *value);
        }
      }
      return;
    }

    case CommandType::arlen: {
      const auto* array = store.find_array(command.args[0]);
      resp::append_integer(
          out, array == nullptr ? 0
                                : static_cast<long long>(array->length()));
      return;
    }

    case CommandType::arcount: {
      const auto* array = store.find_array(command.args[0]);
      resp::append_integer(
          out, array == nullptr ? 0
                                : static_cast<long long>(array->count()));
      return;
    }

    case CommandType::ardel: {
      auto* array = store.find_array(command.args[0]);
      if (array == nullptr) {
        resp::append_integer(out, 0);
        return;
      }
      long long removed = 0;
      for (std::size_t i = 1; i < command.args.size(); ++i) {
        const auto index = parse_ll(command.args[i]);
        if (!index || *index < 0) {
          resp::append_error(out, integer_range_error());
          return;
        }
        removed +=
            array->del(static_cast<std::uint64_t>(*index)) ? 1 : 0;
      }
      if (removed != 0 &&
          (array->count() != 0 || array->realtime_reserved())) {
        store.signal_key_modified(command.args[0]);
      }
      store.erase_if_empty(command.args[0], *array);
      resp::append_integer(out, removed);
      return;
    }

    case CommandType::arinsert: {
      for (std::size_t i = 1; i < command.args.size(); ++i) {
        if (!store.value_fits(command.args[i])) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      try {
        auto& array =
            store.get_or_create_array(command.args[0], array_implementation);
        long long first = -1;
        for (std::size_t i = 1; i < command.args.size(); ++i) {
          const auto index = array.insert(command.args[i]);
          if (first < 0) {
            first = static_cast<long long>(index);
          }
        }
        if (first >= 0) {
          store.signal_key_modified(command.args[0]);
        }
        resp::append_integer(out, first < 0 ? 0 : first);
      } catch (const std::length_error& ex) {
        resp::append_error(out, ex.what());
      }
      return;
    }

    case CommandType::arnext: {
      const auto* array = store.find_array(command.args[0]);
      resp::append_integer(
          out, array == nullptr
                   ? 0
                   : static_cast<long long>(array->next_insert()));
      return;
    }

    case CommandType::arseek: {
      const auto index = parse_ll(command.args[1]);
      if (!index || *index < 0) {
        resp::append_error(out, integer_range_error());
        return;
      }
      try {
        auto& array =
            store.get_or_create_array(command.args[0], array_implementation);
        (void)array.seek(static_cast<std::uint64_t>(*index));
        store.signal_key_modified(command.args[0]);
        resp::append_integer(out, 1);
      } catch (const std::length_error& ex) {
        resp::append_error(out, ex.what());
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
    case CommandType::pma_rpushx:
    case CommandType::segmented_lpush:
    case CommandType::segmented_rpush:
    case CommandType::segmented_lpushx:
    case CommandType::segmented_rpushx: {
      for (std::size_t index = 1; index < command.args.size(); ++index) {
        if (!store.value_fits(command.args[index])) {
          resp::append_error(out, kValueTooLarge);
          return;
        }
      }
      const auto values = std::span<const std::string_view>(
          command.args.data() + 1, command.args.size() - 1);
      const bool front = type == CommandType::pma_lpush ||
                         type == CommandType::pma_lpushx ||
                         type == CommandType::segmented_lpush ||
                         type == CommandType::segmented_lpushx;
      const bool only_if_exists = type == CommandType::pma_lpushx ||
                                  type == CommandType::pma_rpushx ||
                                  type == CommandType::segmented_lpushx ||
                                  type == CommandType::segmented_rpushx;
      const auto implementation =
          type == CommandType::segmented_lpush ||
                  type == CommandType::segmented_rpush ||
                  type == CommandType::segmented_lpushx ||
                  type == CommandType::segmented_rpushx
              ? ListImplementation::Segmented
              : ListImplementation::Pma;
      const auto length = front
                              ? store.lpush(command.args[0], values,
                                            only_if_exists, implementation)
                              : store.rpush(command.args[0], values,
                                            only_if_exists, implementation);
      resp::append_integer(out, length);
      return;
    }

    case CommandType::lpop:
    case CommandType::rpop:
    case CommandType::pma_lpop:
    case CommandType::pma_rpop:
    case CommandType::segmented_lpop:
    case CommandType::segmented_rpop: {
      const bool front = type == CommandType::pma_lpop ||
                         type == CommandType::segmented_lpop;
      if (command.args.size() == 1) {
        const auto value = front ? store.lpop(command.args[0])
                                 : store.rpop(command.args[0]);
        if (value) {
          resp::append_bulk_string(out, *value);
        } else {
          resp::append_null(out, version);
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
        resp::append_null(out, version);
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

    case CommandType::lmove:
    case CommandType::rpoplpush:
    case CommandType::blmove:
    case CommandType::pma_lmove:
    case CommandType::pma_rpoplpush:
    case CommandType::pma_blmove:
    case CommandType::segmented_lmove:
    case CommandType::segmented_rpoplpush:
    case CommandType::segmented_blmove: {
      const bool blocking = type == CommandType::blmove ||
                            type == CommandType::pma_blmove ||
                            type == CommandType::segmented_blmove;
      const auto source = command.args[0];
      const auto destination = command.args[1];

      // Redis does not inspect a wrong-type destination until the source can
      // actually produce an item. This matters to BLMOVE waiting on an empty
      // source and to LMOVE against a missing source.
      if (store.llen(source) != 0) {
        if (destination != source) {
          if (!store.ttl_empty()) {
            (void)store.purge_if_expired(destination, store.now_ms());
          }
          if (const auto destination_type = store.key_type(destination);
              destination_type && *destination_type != KeyType::List) {
            resp::append_error(out, kWrongType);
            return;
          }
        }
        const auto implementation =
            type == CommandType::segmented_lmove ||
                    type == CommandType::segmented_rpoplpush ||
                    type == CommandType::segmented_blmove
                ? ListImplementation::Segmented
                : ListImplementation::Pma;
        const auto value = store.lmove(
            source, destination, command.list_pop_front,
            command.list_push_front, implementation);
        if (value) {
          resp::append_bulk_string(out, *value);
          return;
        }
      }

      if (blocking && options.blocking_lists.park != nullptr &&
          options.blocking_lists.park(options.blocking_lists.context, command)) {
        return;
      }
      resp::append_null(out, version);
      return;
    }

    case CommandType::blpop:
    case CommandType::brpop:
    case CommandType::lmpop:
    case CommandType::blmpop:
    case CommandType::pma_blpop:
    case CommandType::pma_brpop:
    case CommandType::pma_lmpop:
    case CommandType::pma_blmpop:
    case CommandType::segmented_blpop:
    case CommandType::segmented_brpop:
    case CommandType::segmented_lmpop:
    case CommandType::segmented_blmpop: {
      const bool blocking = type == CommandType::blpop ||
                            type == CommandType::brpop ||
                            type == CommandType::blmpop ||
                            type == CommandType::pma_blpop ||
                            type == CommandType::pma_brpop ||
                            type == CommandType::pma_blmpop ||
                            type == CommandType::segmented_blpop ||
                            type == CommandType::segmented_brpop ||
                            type == CommandType::segmented_blmpop;
      const bool single = type == CommandType::blpop ||
                          type == CommandType::brpop ||
                          type == CommandType::pma_blpop ||
                          type == CommandType::pma_brpop ||
                          type == CommandType::segmented_blpop ||
                          type == CommandType::segmented_brpop;
      const auto keys = command.args.subspan(command.list_key_offset,
                                             command.list_key_count);
      const auto now = store.now_ms();
      for (const auto key : keys) {
        if (!store.ttl_empty()) {
          (void)store.purge_if_expired(key, now);
        }
        if (const auto actual = store.key_type(key);
            actual && *actual != KeyType::List) {
          resp::append_error(out, kWrongType);
          return;
        }
        if (store.llen(key) == 0) {
          continue;
        }

        if (single) {
          const auto value = command.list_pop_front ? store.lpop(key)
                                                    : store.rpop(key);
          if (!value) {
            continue;
          }
          resp::append_array_header(out, 2);
          resp::append_bulk_string(out, key);
          resp::append_bulk_string(out, *value);
          return;
        }

        const auto values = command.list_pop_front
                                ? store.lpop(key, command.list_count)
                                : store.rpop(key, command.list_count);
        if (values.empty()) {
          continue;
        }
        resp::append_array_header(out, 2);
        resp::append_bulk_string(out, key);
        resp::append_array_header(out, values.size());
        for (const auto& value : values) {
          resp::append_bulk_string(out, value);
        }
        return;
      }

      if (blocking && options.blocking_lists.park != nullptr &&
          options.blocking_lists.park(options.blocking_lists.context, command)) {
        return;
      }
      append_null_array(out, version);
      return;
    }

    case CommandType::llen:
    case CommandType::pma_llen:
    case CommandType::segmented_llen:
      resp::append_integer(out,
                           static_cast<long long>(store.llen(command.args[0])));
      return;

    case CommandType::lindex:
    case CommandType::pma_lindex:
    case CommandType::segmented_lindex: {
      const auto index = parse_ll(command.args[1]);
      if (!index) {
        resp::append_error(out, integer_range_error());
        return;
      }
      const auto value = store.lindex(command.args[0], *index);
      if (value) {
        resp::append_bulk_string(out, *value);
      } else {
        resp::append_null(out, version);
      }
      return;
    }

    case CommandType::lrange:
    case CommandType::pma_lrange:
    case CommandType::segmented_lrange: {
      const auto values = store.lrange(command.args[0], command.range_start,
                                       command.range_stop);
      resp::append_array_header(out, values.size());
      for (const auto value : values) {
        resp::append_bulk_string(out, value);
      }
      return;
    }

    case CommandType::lset:
    case CommandType::pma_lset:
    case CommandType::segmented_lset: {
      const auto index = parse_ll(command.args[1]);
      if (!index) {
        resp::append_error(out, integer_range_error());
        return;
      }
      if (!store.value_fits(command.args[2])) {
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
    case CommandType::segmented_ltrim:
      store.ltrim(command.args[0], command.range_start, command.range_stop);
      resp::append_simple_string(out, "OK");
      return;

    case CommandType::lrem:
    case CommandType::pma_lrem:
    case CommandType::segmented_lrem: {
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
    case CommandType::pma_linsert:
    case CommandType::segmented_linsert: {
      bool before = false;
      if (equals_ci(command.args[1], "BEFORE")) {
        before = true;
      } else if (!equals_ci(command.args[1], "AFTER")) {
        resp::append_error(out, syntax_error());
        return;
      }
      if (!store.value_fits(command.args[3])) {
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
      if (!store.value_fits(value)) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      // Common case: SET k v -- no options. Skip exists()/now_ms()/option loop.
      if (command.args.size() == 2) {
        store.set(key, value);
        resp::append_simple_string(out, "OK");
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
          current->append_to(*old_value);
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
          resp::append_null(out, version);
        }
      } else if (condition_met) {
        resp::append_simple_string(out, "OK");
      } else {
        resp::append_null(out, version);
      }
      return;
    }
    case CommandType::get: {
      // One keyspace probe (type gate skipped for GET; see is_typed_string_command).
      const auto result = store.get_result(command.args[0]);
      switch (result.status) {
        case StringLookup::Missing:
          resp::append_null(out, version);
          break;
        case StringLookup::WrongType:
          resp::append_error(out, kWrongType);
          break;
        case StringLookup::Ok:
          resp::append_bulk_string(out, result.value);
          break;
      }
      return;
    }
    case CommandType::getset: {
      const auto& value = command.args[1];
      if (!store.value_fits(value)) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      const auto previous = store.get_set(command.args[0], value);
      if (previous) {
        resp::append_bulk_string(out, *previous);
      } else {
        resp::append_null(out, version);
      }
      return;
    }
    case CommandType::setnx: {
      const auto& value = command.args[1];
      if (!store.value_fits(value)) {
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
        resp::append_null(out, version);
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
      const auto length = store.append(command.args[0], value);
      if (!length) {
        resp::append_error(out, kValueTooLarge);
        return;
      }
      resp::append_integer(out, static_cast<long long>(*length));
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
      if (version == resp::Version::resp3) {
        resp::append_double(out, *result);
      } else {
        resp::append_bulk_string(out, *result);
      }
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
        if (!store.value_fits(command.args[i + 1])) {
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
          resp::append_null(out, version);
        } else {
          resp::append_bulk_string(out, *value);
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
    case CommandType::scan: {
      const auto cursor = parse_u64(command.args[0]);
      if (!cursor) {
        resp::append_error(out, "ERR invalid cursor");
        return;
      }
      std::string_view pattern;
      std::string_view type_filter;
      bool has_pattern = false;
      bool has_type = false;
      std::size_t count = 10;
      for (std::size_t i = 1; i < command.args.size();) {
        if (equals_ci(command.args[i], "MATCH") &&
            i + 1 < command.args.size()) {
          pattern = command.args[i + 1];
          has_pattern = true;
          i += 2;
          continue;
        }
        if (equals_ci(command.args[i], "COUNT") &&
            i + 1 < command.args.size()) {
          const auto parsed = parse_u64(command.args[i + 1]);
          if (!parsed || *parsed == 0 ||
              *parsed > std::numeric_limits<std::size_t>::max()) {
            resp::append_error(out, syntax_error());
            return;
          }
          count = static_cast<std::size_t>(*parsed);
          i += 2;
          continue;
        }
        if (equals_ci(command.args[i], "TYPE") &&
            i + 1 < command.args.size()) {
          type_filter = command.args[i + 1];
          has_type = true;
          i += 2;
          continue;
        }
        resp::append_error(out, syntax_error());
        return;
      }

      std::vector<std::string_view> keys;
      const auto now = store.ttl_empty() ? std::uint64_t{0} : store.now_ms();
      const auto next = store.scan(
          *cursor, count, now,
          [&](std::string_view key, KeyType type) {
            if ((!has_pattern || scan_glob_match(pattern, key)) &&
                (!has_type || equals_ci(type_filter, key_type_name(type)))) {
              keys.push_back(key);
            }
          });
      resp::append_array_header(out, 2);
      resp::append_bulk_string(out, std::to_string(next));
      resp::append_array_header(out, keys.size());
      for (const auto key : keys) {
        resp::append_bulk_string(out, key);
      }
      return;
    }
    case CommandType::key_type: {
      const auto type = store.key_type(command.args[0]);
      resp::append_simple_string(out, type ? key_type_name(*type) : "none");
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
        out.append(memory_stats_response(*zstats, version));
      } else if (const auto hstats = store.hash_memory_stats(command.args[0]);
                 hstats) {
        out.append(hash_memory_stats_response(*hstats, version));
      } else if (const auto lstats = store.list_memory_stats(command.args[0]);
                 lstats) {
        out.append(list_memory_stats_response(*lstats, version));
      } else if (const auto sstats = store.set_memory_stats(command.args[0]);
                 sstats) {
        out.append(set_memory_stats_response(*sstats, version));
      } else if (const auto astats = store.array_memory_stats(command.args[0]);
                 astats) {
        out.append(array_memory_stats_response(*astats, version));
      } else {
        resp::append_null(out, version);
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
        resp::append_null(out, version);
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
      if (!store.value_fits(new_value)) {
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
      if (!store.value_fits(command.args[2])) {
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
        resp::append_null(out, version);
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
