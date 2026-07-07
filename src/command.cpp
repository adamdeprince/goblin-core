#include "goblin/core/command.hpp"

#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace goblin::core {
namespace {

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

[[nodiscard]] std::string memory_stats_response(const ZSetMemoryStats& stats) {
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

  std::vector<std::string_view> views;
  views.reserve(fields.size());
  for (const auto& field : fields) {
    views.push_back(field);
  }
  return resp::array(views);
}

[[nodiscard]] std::string hash_memory_stats_response(const HashMemoryStats& stats) {
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
  add("total_allocated_bytes", stats.total_allocated_bytes);

  std::vector<std::string_view> views;
  views.reserve(fields.size());
  for (const auto& field : fields) {
    views.push_back(field);
  }
  return resp::array(views);
}

constexpr std::string_view kWrongType =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

[[nodiscard]] bool is_zset_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::zadd:
    case CommandType::zcard:
    case CommandType::zrange:
    case CommandType::zrank:
    case CommandType::zrevrange:
    case CommandType::zrevrank:
    case CommandType::zrem:
    case CommandType::zscore:
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
      return true;
    default:
      return false;
  }
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

  if (equals_ci(command.name, "PING")) {
    if (command.args.size() > 1) {
      return parse_error(wrong_arity("ping"));
    }
    command.type = CommandType::ping;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "ZADD")) {
    if (command.args.size() < 3 || (command.args.size() - 1) % 2 != 0) {
      return parse_error(wrong_arity("zadd"));
    }
    command.type = CommandType::zadd;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "ZCARD")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("zcard"));
    }
    command.type = CommandType::zcard;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "ZRANGE")) {
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

  if (equals_ci(command.name, "ZRANK")) {
    if (command.args.size() != 2) {
      return parse_error(wrong_arity("zrank"));
    }
    command.type = CommandType::zrank;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "ZREVRANGE")) {
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

  if (equals_ci(command.name, "ZREVRANK")) {
    if (command.args.size() != 2) {
      return parse_error(wrong_arity("zrevrank"));
    }
    command.type = CommandType::zrevrank;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "ZREM")) {
    if (command.args.size() < 2) {
      return parse_error(wrong_arity("zrem"));
    }
    command.type = CommandType::zrem;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "ZSCORE")) {
    if (command.args.size() != 2) {
      return parse_error(wrong_arity("zscore"));
    }
    command.type = CommandType::zscore;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HSET")) {
    if (command.args.size() < 3 || (command.args.size() - 1) % 2 != 0) {
      return parse_error(wrong_arity("hset"));
    }
    command.type = CommandType::hset;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HSETNX")) {
    if (command.args.size() != 3) {
      return parse_error(wrong_arity("hsetnx"));
    }
    command.type = CommandType::hsetnx;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HGET")) {
    if (command.args.size() != 2) {
      return parse_error(wrong_arity("hget"));
    }
    command.type = CommandType::hget;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HMGET")) {
    if (command.args.size() < 2) {
      return parse_error(wrong_arity("hmget"));
    }
    command.type = CommandType::hmget;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HDEL")) {
    if (command.args.size() < 2) {
      return parse_error(wrong_arity("hdel"));
    }
    command.type = CommandType::hdel;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HGETALL")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("hgetall"));
    }
    command.type = CommandType::hgetall;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HKEYS")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("hkeys"));
    }
    command.type = CommandType::hkeys;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HVALS")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("hvals"));
    }
    command.type = CommandType::hvals;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HLEN")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("hlen"));
    }
    command.type = CommandType::hlen;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HEXISTS")) {
    if (command.args.size() != 2) {
      return parse_error(wrong_arity("hexists"));
    }
    command.type = CommandType::hexists;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HSTRLEN")) {
    if (command.args.size() != 2) {
      return parse_error(wrong_arity("hstrlen"));
    }
    command.type = CommandType::hstrlen;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "HINCRBY")) {
    if (command.args.size() != 3) {
      return parse_error(wrong_arity("hincrby"));
    }
    command.type = CommandType::hincrby;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "GOBLIN.MEMORY")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("goblin.memory"));
    }
    command.type = CommandType::goblin_memory;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "GOBLIN.OPTIMIZE")) {
    if (command.args.size() != 1 && command.args.size() != 2) {
      return parse_error(wrong_arity("goblin.optimize"));
    }
    command.type = CommandType::goblin_optimize;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "GOBLIN.SAVE")) {
    if (command.args.size() != 1 && command.args.size() != 2) {
      return parse_error(wrong_arity("goblin.save"));
    }
    command.type = CommandType::goblin_save;
    return {.command = std::move(command)};
  }

  if (equals_ci(command.name, "GOBLIN.LOAD")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("goblin.load"));
    }
    command.type = CommandType::goblin_load;
    return {.command = std::move(command)};
  }

  command.type = CommandType::unknown;
  return {.command = std::move(command)};
}

void execute_command_into(Store& store,
                          const Command& command,
                          std::string& out,
                          CommandExecutionOptions options) {
  // WRONGTYPE: a key holds at most one type. A zset command on a hash key (or a
  // hash command on a zset key) is rejected before it operates. GOBLIN.* commands
  // resolve the type themselves and are exempt.
  if (!command.args.empty()) {
    if (is_zset_command(command.type) && store.key_is_hash(command.args[0])) {
      resp::append_error(out, kWrongType);
      return;
    }
    if (is_hash_command(command.type) && store.key_is_zset(command.args[0])) {
      resp::append_error(out, kWrongType);
      return;
    }
  }

  switch (command.type) {
    case CommandType::ping:
      if (command.args.empty()) {
        resp::append_simple_string(out, "PONG");
      } else {
        resp::append_bulk_string(out, command.args.front());
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
      long long added = 0;
      for (std::size_t i = 1; i < command.args.size(); i += 2) {
        added += store.hset(key, command.args[i], command.args[i + 1]);
      }
      resp::append_integer(out, added);
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
      long long removed = 0;
      for (std::size_t i = 1; i < command.args.size(); ++i) {
        removed += store.hdel(command.args[0], command.args[i]) ? 1 : 0;
      }
      resp::append_integer(out, removed);
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

    case CommandType::goblin_memory: {
      if (const auto zstats = store.zset_memory_stats(command.args[0]); zstats) {
        out.append(memory_stats_response(*zstats));
      } else if (const auto hstats = store.hash_memory_stats(command.args[0]);
                 hstats) {
        out.append(hash_memory_stats_response(*hstats));
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
