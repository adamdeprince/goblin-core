#include "goblin/core/command.hpp"

#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
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
  fields.reserve(34);

  auto add = [&fields](std::string_view name, std::size_t value) {
    fields.emplace_back(name);
    fields.push_back(std::to_string(value));
  };

  add("member_count", stats.member_count);
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

  auto append_header = [&out,
                        with_scores = command.with_scores,
                        reserve_limit = options.output_reserve_limit](
                           std::size_t entry_count) {
    resp::reserve_append_capacity(
        out,
        16 + entry_count * (with_scores ? 48 : 32),
        reserve_limit);
    resp::append_array_header(out, with_scores ? entry_count * 2 : entry_count);
  };

  if (command.with_scores) {
    if (store.score_string_cache_enabled()) {
      auto append_cached = [&out](std::string_view member,
                                  std::string_view score_text) {
        resp::append_bulk_string(out, member);
        resp::append_bulk_string(out, score_text);
      };
      if (reverse) {
        store.zrevrange_score_text_values_for_each_counted(
            command.args[0], start, stop, append_header, append_cached);
      } else {
        store.zrange_score_text_values_for_each_counted(
            command.args[0], start, stop, append_header, append_cached);
      }
    } else {
      auto append_score = [&out](std::string_view member, double score) {
        resp::append_bulk_string(out, member);
        resp::append_bulk_finite_double(out, score);
      };
      if (reverse) {
        store.zrevrange_values_for_each_counted(
            command.args[0], start, stop, append_header, append_score);
      } else {
        store.zrange_values_for_each_counted(
            command.args[0], start, stop, append_header, append_score);
      }
    }
    return;
  }

  auto append_member = [&out](std::string_view member) {
    resp::append_bulk_string(out, member);
  };
  if (reverse) {
    store.zrevrange_members_for_each_counted(
        command.args[0], start, stop, append_header, append_member);
  } else {
    store.zrange_members_for_each_counted(
        command.args[0], start, stop, append_header, append_member);
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

  if (equals_ci(command.name, "GOBLIN.MEMORY")) {
    if (command.args.size() != 1) {
      return parse_error(wrong_arity("goblin.memory"));
    }
    command.type = CommandType::goblin_memory;
    return {.command = std::move(command)};
  }

  command.type = CommandType::unknown;
  return {.command = std::move(command)};
}

void execute_command_into(Store& store,
                          const Command& command,
                          std::string& out,
                          CommandExecutionOptions options) {
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

    case CommandType::goblin_memory: {
      const auto stats = store.zset_memory_stats(command.args[0]);
      if (!stats) {
        resp::append_null_bulk_string(out);
      } else {
        out.append(memory_stats_response(*stats));
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
