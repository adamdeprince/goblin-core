#include "goblin/core/command.hpp"
#include "goblin/core/chunked_sorted_list.hpp"
#include "goblin/core/hash.hpp"
#include "goblin/core/compact_listpack.hpp"
#include "goblin/core/key_arena.hpp"

#include "goblin/core/rdb.hpp"
#include "goblin/core/luau_script.hpp"
#include "goblin/core/quickjs_script.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/script.hpp"
#include "goblin/core/server.hpp"
#include "goblin/core/tcl_script.hpp"
#include "goblin/core/upython_script.hpp"
#include "goblin/core/wren_script.hpp"
#include "goblin/core/store.hpp"
#include "goblin/core/string_value.hpp"
#include "goblin/core/keyspace_storage.hpp"
#include "goblin/core/ttl_set.hpp"
#include "goblin/core/swiss_table.hpp"

// Tests must validate even in Release builds; keep assert() live regardless of
// the build's NDEBUG setting.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct BadHasher {
  std::size_t operator()(const std::string&) const noexcept {
    return 7;
  }
};

struct IntLess {
  [[nodiscard]] bool operator()(int lhs, int rhs) const noexcept {
    return lhs < rhs;
  }
};

std::string execute_fields(goblin::core::Store& store,
                           std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> field_views(fields);
  auto parsed = goblin::core::parse_command(field_views);
  assert(parsed.ok());
  return goblin::core::execute_command(store, *parsed.command);
}

void test_resp_parser_incremental() {
  goblin::core::RespParser parser;
  parser.append("*4\r\n$4\r\nZADD\r\n$7\r\nleaders\r\n");
  assert(!parser.pop().has_value());

  parser.append("$2\r\n42\r\n$5\r\nalice\r\n");
  auto frame = parser.pop();
  assert(frame.has_value());
  assert((frame->fields ==
          std::vector<std::string_view>{"ZADD", "leaders", "42", "alice"}));
  assert(!parser.pop().has_value());
}

void test_inline_parser() {
  goblin::core::RespParser parser;
  parser.append("ZRANGE leaders 0 -1 WITHSCORES\r\n");
  auto frame = parser.pop();
  assert(frame.has_value());
  assert((frame->fields ==
          std::vector<std::string_view>{"ZRANGE", "leaders", "0", "-1", "WITHSCORES"}));
}

void test_protocol_error() {
  goblin::core::RespParser parser;
  parser.append("*1\r\n+OK\r\n");
  assert(parser.has_error());
}

void test_resp_parser_pipeline_and_pop_into() {
  goblin::core::RespParser parser;
  // Two pipelined commands in a single append share the field pool.
  parser.append(
      "*3\r\n$6\r\nZSCORE\r\n$1\r\nk\r\n$3\r\nfoo\r\n"
      "*2\r\n$5\r\nZRANK\r\n$3\r\nbar\r\n");

  std::vector<std::string_view> fields;
  assert(parser.pop_into(fields));
  assert((fields == std::vector<std::string_view>{"ZSCORE", "k", "foo"}));
  assert(parser.pop_into(fields));
  assert((fields == std::vector<std::string_view>{"ZRANK", "bar"}));
  assert(!parser.pop_into(fields));

  // After a full drain the pool and buffer compact; a fresh batch must still
  // resolve field views correctly against the shifted buffer.
  parser.append("*2\r\n$5\r\nZCARD\r\n$4\r\nzkey\r\n");
  assert(parser.pop_into(fields));
  assert((fields == std::vector<std::string_view>{"ZCARD", "zkey"}));
  assert(!parser.pop_into(fields));

  // pop() and pop_into() read the same wire; interleave them and include an
  // empty bulk field.
  parser.append(
      "*1\r\n$4\r\nPING\r\n"
      "*2\r\n$6\r\nZSCORE\r\n$0\r\n\r\n");
  auto frame = parser.pop();
  assert(frame.has_value());
  assert((frame->fields == std::vector<std::string_view>{"PING"}));
  assert(parser.pop_into(fields));
  assert((fields == std::vector<std::string_view>{"ZSCORE", ""}));
  assert(!parser.pop_into(fields));
  assert(!parser.has_error());
}

void test_command_perfect_hash() {
  using CT = goblin::core::CommandType;
  auto type_of = [](std::initializer_list<std::string_view> f) {
    std::vector<std::string_view> fields(f);
    auto p = goblin::core::parse_command(fields);
    assert(p.ok());
    return p.command->type;
  };
  // Case-folded three ways, each command resolves to the same type.
  assert(type_of({"PING"}) == CT::ping);
  assert(type_of({"ping"}) == CT::ping);
  assert(type_of({"PiNg"}) == CT::ping);
  assert(type_of({"zscore", "k", "m"}) == CT::zscore);
  assert(type_of({"zRaNgE", "k", "0", "-1"}) == CT::zrange);
  assert(type_of({"hincrby", "h", "f", "1"}) == CT::hincrby);
  assert(type_of({"goblin.memory", "k"}) == CT::goblin_memory);
  assert(type_of({"Goblin.Optimize", "k"}) == CT::goblin_optimize);
  // Unknowns, near-misses, the length boundary, an embedded NUL.
  assert(type_of({"PIN", "x"}) == CT::unknown);
  assert(type_of({"PINGG"}) == CT::unknown);
  assert(type_of({"ZZZ"}) == CT::unknown);
  assert(type_of({"GOBLIN.NOPE", "x"}) == CT::unknown);
  assert(type_of({"THISNAMEISTOOLONG"}) == CT::unknown);  // 17 bytes > 15
  assert(type_of({std::string_view("ZAD\0", 4)}) == CT::unknown);
  // The hash does not swallow arity errors.
  std::vector<std::string_view> bad{"ZADD", "k"};
  auto p = goblin::core::parse_command(bad);
  assert(!p.ok());
  assert(p.error == "ERR wrong number of arguments for 'zadd' command");
}

void test_resp_bulk_writer_small_header_table() {
  assert(goblin::core::resp::bulk_string("") == "$0\r\n\r\n");

  const std::string short_value(64, 'x');
  assert(goblin::core::resp::bulk_string(short_value) ==
         "$64\r\n" + short_value + "\r\n");

  const std::string fallback_value(65, 'y');
  assert(goblin::core::resp::bulk_string(fallback_value) ==
         "$65\r\n" + fallback_value + "\r\n");
}

void test_resp_array_writer_small_header_table() {
  std::string out;
  goblin::core::resp::append_array_header(out, 0);
  assert(out == "*0\r\n");

  out.clear();
  goblin::core::resp::append_array_header(out, 256);
  assert(out == "*256\r\n");

  out.clear();
  goblin::core::resp::append_array_header(out, 257);
  assert(out == "*257\r\n");
}

void test_command_dispatch() {
  goblin::core::Store store;

  assert(execute_fields(store, {"ZADD", "leaders", "42", "alice", "17", "bob"}) ==
         ":2\r\n");

  const auto memory = execute_fields(store, {"GOBLIN.MEMORY", "leaders"});
  assert(memory.starts_with("*38\r\n"));
  assert(memory.find("member_index_allocated_bytes") != std::string::npos);
  assert(memory.find("rank_cache_mode") != std::string::npos);
  assert(memory.find("score_width") != std::string::npos);
  assert(memory.find("$3\r\ni16\r\n") != std::string::npos);
  assert(memory.find("$3\r\noff\r\n") != std::string::npos);
  assert(memory.find("rank_location_cache_allocated_bytes") != std::string::npos);
  assert(memory.find("score_string_cache_allocated_bytes") != std::string::npos);
  assert(memory.find("score_index_allocated_bytes") != std::string::npos);

  goblin::core::Store cache_store(
      goblin::core::StoreOptions{
          .rank_cache_mode = goblin::core::RankCacheMode::BlockHint,
      });
  assert(execute_fields(cache_store, {"ZADD", "leaders", "42", "alice"}) ==
         ":1\r\n");
  const auto cache_memory = execute_fields(
      cache_store,
      {"GOBLIN.MEMORY", "leaders"});
  assert(cache_memory.find("rank_cache_mode") != std::string::npos);
  assert(cache_memory.find("$10\r\nblock-hint\r\n") != std::string::npos);

  assert(execute_fields(store, {"ZCARD", "leaders"}) == ":2\r\n");

  assert(execute_fields(store, {"ZSCORE", "leaders", "alice"}) == "$2\r\n42\r\n");

  assert(execute_fields(store, {"ZRANK", "leaders", "alice"}) == ":1\r\n");
  assert(execute_fields(store, {"ZREVRANK", "leaders", "alice"}) == ":0\r\n");

  assert(execute_fields(store, {"ZRANGE", "leaders", "0", "-1"}) ==
         "*2\r\n$3\r\nbob\r\n$5\r\nalice\r\n");

  assert(execute_fields(store, {"ZRANGE", "leaders", "0", "-1", "WITHSCORES"}) ==
         "*4\r\n$3\r\nbob\r\n$2\r\n17\r\n$5\r\nalice\r\n$2\r\n42\r\n");

  assert(execute_fields(store, {"ZREVRANGE", "leaders", "0", "-1"}) ==
         "*2\r\n$5\r\nalice\r\n$3\r\nbob\r\n");

  assert(execute_fields(store, {"ZREVRANGE", "leaders", "0", "-1", "WITHSCORES"}) ==
         "*4\r\n$5\r\nalice\r\n$2\r\n42\r\n$3\r\nbob\r\n$2\r\n17\r\n");

  assert(execute_fields(store, {"ZREM", "leaders", "bob", "missing"}) == ":1\r\n");
  assert(execute_fields(store, {"ZCARD", "leaders"}) == ":1\r\n");
}

void test_echo_info() {
  goblin::core::Store store;
  // ECHO returns its one argument as a bulk string.
  assert(execute_fields(store, {"ECHO", "hello"}) == "$5\r\nhello\r\n");
  // Arity is rejected at parse time (ECHO needs exactly one arg; INFO 0 or 1).
  auto rejects = [](std::initializer_list<std::string_view> f) {
    std::vector<std::string_view> v(f);
    return !goblin::core::parse_command(v).ok();
  };
  assert(rejects({"ECHO"}));
  assert(rejects({"ECHO", "a", "b"}));
  assert(rejects({"INFO", "a", "b"}));
  // INFO returns a bulk string carrying used_memory_rss (what the memory
  // benchmarks read) + redis_version; with or without a section arg.
  const auto info = execute_fields(store, {"INFO", "memory"});
  assert(info.starts_with("$"));
  assert(info.find("used_memory_rss:") != std::string::npos);
  assert(info.find("redis_version:") != std::string::npos);
  assert(execute_fields(store, {"INFO"}).find("used_memory_rss:") !=
         std::string::npos);
  // PING regression guard (shares the same arg-branch shape as ECHO).
  assert(execute_fields(store, {"PING"}) == "+PONG\r\n");
  assert(execute_fields(store, {"PING", "hey"}) == "$3\r\nhey\r\n");
}

void test_range_command_parses_indexes() {
  std::vector<std::string_view> fields{"ZRANGE", "leaders", "-2", "-1"};
  auto parsed = goblin::core::parse_command(fields);
  assert(parsed.ok());
  assert(parsed.command->range_indexes_parsed);
  assert(parsed.command->range_start == -2);
  assert(parsed.command->range_stop == -1);

  fields = {"ZREVRANGE", "leaders", "0", "3", "WITHSCORES"};
  parsed = goblin::core::parse_command(fields);
  assert(parsed.ok());
  assert(parsed.command->range_indexes_parsed);
  assert(parsed.command->range_start == 0);
  assert(parsed.command->range_stop == 3);
  assert(parsed.command->with_scores);

  fields = {"ZRANGE", "leaders", "x", "-1"};
  parsed = goblin::core::parse_command(fields);
  assert(!parsed.ok());
  assert(parsed.error == "ERR value is not an integer or out of range");

  fields = {"ZRANGE", "leaders", "x", "-1", "BOGUS"};
  parsed = goblin::core::parse_command(fields);
  assert(!parsed.ok());
  assert(parsed.error == "ERR syntax error");
}

void test_zadd_updates_existing_members() {
  goblin::core::Store store;

  assert(execute_fields(store, {"ZADD", "z", "3", "a"}) == ":1\r\n");
  assert(execute_fields(store, {"ZADD", "z", "1", "a"}) == ":0\r\n");
  assert(execute_fields(store, {"ZRANGE", "z", "0", "-1", "WITHSCORES"}) ==
         "*2\r\n$1\r\na\r\n$1\r\n1\r\n");
}

void test_score_string_cache_updates_with_scores() {
  goblin::core::Store store(
      goblin::core::StoreOptions{.score_string_cache = true});

  assert(execute_fields(store, {"ZADD", "z", "1.5", "a", "2", "b"}) == ":2\r\n");
  assert(execute_fields(store, {"ZRANGE", "z", "0", "-1", "WITHSCORES"}) ==
         "*4\r\n$1\r\na\r\n$3\r\n1.5\r\n$1\r\nb\r\n$1\r\n2\r\n");

  assert(execute_fields(store, {"ZADD", "z", "3.25", "a"}) == ":0\r\n");
  assert(execute_fields(store, {"ZRANGE", "z", "0", "-1", "WITHSCORES"}) ==
         "*4\r\n$1\r\nb\r\n$1\r\n2\r\n$1\r\na\r\n$4\r\n3.25\r\n");

  assert(execute_fields(store, {"ZREM", "z", "b"}) == ":1\r\n");
  assert(execute_fields(store, {"ZRANGE", "z", "0", "-1", "WITHSCORES"}) ==
         "*2\r\n$1\r\na\r\n$4\r\n3.25\r\n");

  const auto stats = store.zset_memory_stats("z");
  assert(stats.has_value());
  assert(stats->score_string_cache_bytes > 0);
  assert(stats->score_string_cache_allocated_bytes > 0);
}

void test_swiss_table_string_view_lookup() {
  goblin::core::SwissTable<std::string,
                           int,
                           goblin::core::StringTableHash,
                           goblin::core::StringTableEqual>
      table;

  table.try_emplace("alpha", 1);
  table.try_emplace("beta", 2);

  const std::string owned = "alpha";
  const std::string_view view = owned;
  assert(table.find(view) != nullptr);
  assert(*table.find(view) == 1);
  assert(table.find(std::string_view("gamma")) == nullptr);
  assert(table.erase(std::string_view("beta")));
  assert(table.find("beta") == nullptr);
}

void test_resp_bulk_wire_size_matches_output() {
  for (std::size_t len = 0; len < 65; ++len) {
    const std::string payload(len, 'x');
    std::string out;
    goblin::core::resp::append_bulk_string(out, payload);
    assert(out.size() ==
           goblin::core::resp::detail::bulk_string_wire_size(len));
  }
}

void test_zrange_withscores_batch_matches_streaming_append() {
  const std::array<std::pair<std::string_view, double>, 4> entries{
      std::pair<std::string_view, double>{"a", 1.0},
      {"b", 2.0},
      {"c", -7.0},
      {"member:0000000042", 42.0},
  };

  std::string streaming;
  goblin::core::resp::append_array_header(streaming, entries.size() * 2);
  for (const auto& entry : entries) {
    goblin::core::resp::append_bulk_member_and_finite_double(
        streaming, entry.first, entry.second);
  }

  std::string batched;
  goblin::core::resp::append_array_header(batched, entries.size() * 2);
  goblin::core::resp::append_bulk_withscores_batch(batched, entries);
  assert(batched == streaming);

  std::string chunked;
  goblin::core::resp::append_array_header(chunked, entries.size() * 2);
  goblin::core::resp::append_bulk_withscores_chunk(chunked, entries);
  assert(chunked == streaming);
}

void test_zrange_withscores_fused_matches_legacy_append() {
  goblin::core::Store store;
  assert(execute_fields(store, {"ZADD", "z", "1", "a", "2", "b"}) == ":2\r\n");

  const auto expected =
      execute_fields(store, {"ZRANGE", "z", "0", "-1", "WITHSCORES"});

  std::string fused;
  goblin::core::resp::append_array_header(fused, 4);
  goblin::core::resp::append_bulk_member_and_finite_double(fused, "a", 1.0);
  goblin::core::resp::append_bulk_member_and_finite_double(fused, "b", 2.0);
  assert(fused == expected);

  std::string integer_scores;
  goblin::core::resp::append_array_header(integer_scores, 4);
  goblin::core::resp::append_bulk_member_and_finite_double(
      integer_scores, "a", 42.0);
  goblin::core::resp::append_bulk_member_and_finite_double(
      integer_scores, "b", -7.0);
  assert(integer_scores ==
         "*4\r\n$1\r\na\r\n$2\r\n42\r\n$1\r\nb\r\n$2\r\n-7\r\n");

  std::string legacy;
  goblin::core::resp::append_array_header(legacy, 4);
  goblin::core::resp::append_bulk_string(legacy, "a");
  goblin::core::resp::append_bulk_finite_double(legacy, 1.0);
  goblin::core::resp::append_bulk_string(legacy, "b");
  goblin::core::resp::append_bulk_finite_double(legacy, 2.0);
  assert(legacy == expected);
}

void test_swiss_table_insert_find_update() {
  goblin::core::SwissTable<std::string, int> table;

  auto [one, inserted_one] = table.try_emplace("one", 1);
  assert(inserted_one);
  assert(*one == 1);

  auto [again, inserted_again] = table.try_emplace("one", 11);
  assert(!inserted_again);
  assert(again == one);
  assert(*again == 1);

  auto [assigned, was_inserted] = table.insert_or_assign("one", 10);
  assert(!was_inserted);
  assert(*assigned == 10);
  assert(table.contains("one"));
  assert(*table.find("one") == 10);
}

void test_swiss_table_collision_probe_and_growth() {
  goblin::core::SwissTable<std::string, int, BadHasher> table;

  for (int i = 0; i < 200; ++i) {
    auto key = "key-" + std::to_string(i);
    auto [value, inserted] = table.try_emplace(key, i);
    assert(inserted);
    assert(*value == i);
  }

  assert(table.size() == 200);
  for (int i = 0; i < 200; ++i) {
    auto key = "key-" + std::to_string(i);
    auto* value = table.find(key);
    assert(value != nullptr);
    assert(*value == i);
  }
}

void test_swiss_table_erase_reuses_tombstones() {
  goblin::core::SwissTable<std::string, int, BadHasher> table;

  for (int i = 0; i < 32; ++i) {
    table.try_emplace("key-" + std::to_string(i), i);
  }
  assert(table.erase("key-3"));
  assert(table.erase("key-7"));
  assert(!table.erase("missing"));
  assert(table.find("key-3") == nullptr);

  auto [value, inserted] = table.try_emplace("new", 99);
  assert(inserted);
  assert(*value == 99);
  assert(*table.find("key-8") == 8);
}

void test_chunked_sorted_list_splits_and_ranges() {
  goblin::core::ChunkedSortedList<int, IntLess, 8> list;

  for (int i = 63; i >= 0; --i) {
    list.insert(i);
  }

  assert(list.size() == 64);
  assert(list.block_count() > 1);

  auto range = list.range(10, 7);
  assert((range == std::vector<int>{10, 11, 12, 13, 14, 15, 16}));
  assert(list.at(63) == 63);
  assert(list.rank(10) == 10U);
  assert(list.lower_bound_rank(10) == 10U);
  assert(list.lower_bound_rank(64) == 64U);
  assert(!list.rank(100).has_value());
  assert(list.validate());
}

void test_chunked_sorted_list_erase_rebalances() {
  goblin::core::ChunkedSortedList<int, IntLess, 8> list;

  for (int i = 0; i < 40; ++i) {
    list.insert(i);
  }
  for (int i = 5; i < 35; ++i) {
    assert(list.erase_one(i));
  }

  assert(!list.erase_one(100));
  auto range = list.range(0, list.size());
  assert((range == std::vector<int>{0, 1, 2, 3, 4, 35, 36, 37, 38, 39}));
  assert(list.validate());
}

void test_zset_uses_chunked_ordering() {
  goblin::core::ZSet zset;

  // Enough entries to force at least one block split at the default load factor
  // (a block splits past 2*load), so block_count() > 1 regardless of the default.
  const int kN =
      static_cast<int>(2 * goblin::core::ZSetScoreIndex::kDefaultLoad + 8);
  for (int i = kN - 1; i >= 0; --i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }

  assert(zset.size() == static_cast<std::size_t>(kN));
  assert(zset.block_count() > 1);

  auto range = zset.range(510, 514);
  assert(range.size() == 5);
  assert(range[0].score == 510.0);
  assert(range[4].score == 514.0);
  assert(zset.check_invariants());
}

void test_zset_score_rank_and_remove() {
  goblin::core::ZSet zset;

  assert(zset.add(20.0, "b") == 1);
  assert(zset.add(10.0, "a") == 1);
  assert(zset.add(30.0, "c") == 1);
  assert(zset.add(5.0, "b") == 0);
  assert(zset.add(std::numeric_limits<double>::quiet_NaN(), "nan") == 0);

  assert(zset.size() == 3);
  assert(zset.score("b") == 5.0);
  assert(zset.rank("b") == 0U);
  assert(zset.rank("a") == 1U);
  assert(zset.reverse_rank("c") == 0U);
  assert(zset.reverse_rank("a") == 1U);
  assert(zset.reverse_rank("b") == 2U);
  assert(!zset.rank("missing").has_value());
  assert(!zset.reverse_rank("missing").has_value());

  auto range = zset.range(0, -1);
  assert(range.size() == 3);
  assert(range[0].member == "b");
  assert(range[1].member == "a");
  assert(range[2].member == "c");

  range = zset.reverse_range(0, -1);
  assert(range.size() == 3);
  assert(range[0].member == "c");
  assert(range[1].member == "a");
  assert(range[2].member == "b");

  range = zset.reverse_range(-2, -1);
  assert(range.size() == 2);
  assert(range[0].member == "a");
  assert(range[1].member == "b");

  assert(zset.remove("a"));
  assert(!zset.remove("a"));
  assert(!zset.score("a").has_value());
  assert(zset.check_invariants());
}

void test_zset_equal_score_lex_order_and_id_reuse() {
  goblin::core::ZSet zset;

  assert(zset.add(1.0, "delta") == 1);
  assert(zset.add(1.0, "alpha") == 1);
  assert(zset.add(1.0, "charlie") == 1);
  assert(zset.add(1.0, "bravo") == 1);

  auto range = zset.range(0, -1);
  assert(range.size() == 4);
  assert(range[0].member == "alpha");
  assert(range[1].member == "bravo");
  assert(range[2].member == "charlie");
  assert(range[3].member == "delta");

  assert(zset.remove("bravo"));
  assert(zset.add(1.0, "beta") == 1);
  range = zset.range(0, -1);
  assert(range.size() == 4);
  assert(range[0].member == "alpha");
  assert(range[1].member == "beta");
  assert(range[2].member == "charlie");
  assert(range[3].member == "delta");
  assert(zset.rank("beta") == 1U);
  assert(zset.reverse_rank("beta") == 2U);

  range = zset.reverse_range(0, -1);
  assert(range.size() == 4);
  assert(range[0].member == "delta");
  assert(range[1].member == "charlie");
  assert(range[2].member == "beta");
  assert(range[3].member == "alpha");
  assert(zset.check_invariants());
}

void test_zset_compact_rebuilds_indexes_and_storage() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 64; ++i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  for (int i = 0; i < 64; i += 2) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(zset.size() == 32);
  assert(zset.allocated_member_slots() == 32);
  assert(zset.free_member_slots() == 0);
  const auto old_member_index_capacity = zset.member_index_capacity();
  assert(zset.check_invariants());

  zset.compact();

  assert(zset.size() == 32);
  assert(zset.allocated_member_slots() == 32);
  assert(zset.free_member_slots() == 0);
  assert(zset.member_index_capacity() <= old_member_index_capacity);
  assert(zset.check_invariants());

  auto range = zset.range(0, 2);
  assert(range.size() == 3);
  assert(range[0].member == "member-1");
  assert(range[1].member == "member-3");
  assert(range[2].member == "member-5");
  assert(zset.score("member-3") == 3.0);
  assert(zset.rank("member-3") == 1U);

  assert(zset.add(0.5, "new-member") == 1);
  assert(zset.rank("new-member") == 0U);
  assert(zset.check_invariants());
}

void test_zset_auto_compacts_after_large_removal_batch() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 20000; ++i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  const auto old_member_index_capacity = zset.member_index_capacity();

  for (int i = 0; i < 6000; ++i) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(zset.size() == 14000);
  assert(zset.allocated_member_slots() == 14000);
  assert(zset.free_member_slots() == 0);
  assert(!zset.should_compact_after_removal(6000));
  assert(!zset.compact_after_removal_if_needed(6000));

  assert(zset.size() == 14000);
  assert(zset.allocated_member_slots() == 14000);
  assert(zset.free_member_slots() == 0);
  assert(zset.member_index_capacity() <= old_member_index_capacity);
  assert(zset.rank("member-6000") == 0U);
  assert(zset.score("member-19999") == 19999.0);
  assert(zset.check_invariants());
}

void test_zset_dense_delete_moves_last_member() {
  goblin::core::ZSet zset;

  assert(zset.add(10.0, "a") == 1);
  assert(zset.add(20.0, "b") == 1);
  assert(zset.add(30.0, "c") == 1);
  assert(zset.add(40.0, "d") == 1);

  assert(zset.remove("b"));
  assert(zset.size() == 3);
  assert(zset.allocated_member_slots() == 3);
  assert(zset.free_member_slots() == 0);
  assert(!zset.score("b").has_value());
  assert(zset.score("d") == 40.0);
  assert(zset.rank("d") == 2U);
  assert(zset.check_invariants());

  assert(zset.add(15.0, "e") == 1);
  assert(zset.allocated_member_slots() == 4);
  assert(zset.rank("e") == 1U);
  assert(zset.check_invariants());
}

void test_zset_dense_delete_same_score_multi_block() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 1024; ++i) {
    assert(zset.add(1.0, "member-" + std::to_string(i)) == 1);
  }

  for (int i = 0; i < 300; ++i) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(zset.size() == 724);
  assert(zset.allocated_member_slots() == 724);
  assert(zset.free_member_slots() == 0);
  assert(!zset.score("member-0").has_value());
  assert(zset.score("member-1023") == 1.0);

  auto range = zset.range(0, -1);
  assert(range.size() == 724);
  for (std::size_t i = 1; i < range.size(); ++i) {
    assert(range[i - 1].member < range[i].member);
    assert(range[i].score == 1.0);
  }
  assert(zset.check_invariants());
}

void test_zset_member_index_cleanup_removes_tombstones() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 20000; ++i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  const auto old_member_index_capacity = zset.member_index_capacity();

  for (int i = 0; i < 5000; ++i) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(zset.member_index_tombstones() == 5000);
  assert(!zset.cleanup_member_index_after_removal_if_needed(128));
  assert(zset.rehash_member_index_same_capacity());
  assert(zset.member_index_tombstones() == 0);
  assert(zset.member_index_capacity() == old_member_index_capacity);
  assert(zset.allocated_member_slots() == 15000);
  assert(zset.free_member_slots() == 0);
  assert(zset.rank("member-5000") == 0U);
  assert(zset.score("member-19999") == 19999.0);
  assert(zset.check_invariants());
}

void test_zset_member_index_cleanup_rehashes_many_tombstones() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 20000; ++i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  const auto old_member_index_capacity = zset.member_index_capacity();

  for (int i = 0; i < 10000; ++i) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(zset.member_index_tombstones() == 10000);
  assert(zset.cleanup_member_index_after_removal_if_needed(128));
  assert(zset.member_index_tombstones() == 0);
  assert(zset.member_index_capacity() == old_member_index_capacity);
  assert(zset.allocated_member_slots() == 10000);
  assert(zset.free_member_slots() == 0);
  assert(zset.rank("member-10000") == 0U);
  assert(zset.score("member-19999") == 19999.0);
  assert(zset.check_invariants());
}

void test_zset_member_index_cleanup_shrinks_sparse_table() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 20000; ++i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  const auto old_member_index_capacity = zset.member_index_capacity();

  for (int i = 0; i < 17000; ++i) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(zset.member_index_tombstones() == 17000);
  assert(zset.cleanup_member_index_after_removal_if_needed(128));
  assert(zset.member_index_tombstones() == 0);
  assert(zset.member_index_capacity() < old_member_index_capacity);
  assert(zset.allocated_member_slots() == 3000);
  assert(zset.free_member_slots() == 0);
  assert(zset.rank("member-17000") == 0U);
  assert(zset.score("member-19999") == 19999.0);
  assert(zset.check_invariants());
}

void test_zset_skips_auto_compaction_for_small_removals() {
  goblin::core::ZSet zset;

  for (int i = 0; i < 1000; ++i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  for (int i = 0; i < 100; ++i) {
    assert(zset.remove("member-" + std::to_string(i)));
  }

  assert(!zset.should_compact_after_removal(100));
  assert(!zset.compact_after_removal_if_needed(100));
  assert(zset.allocated_member_slots() == 900);
  assert(zset.free_member_slots() == 0);
  assert(zset.check_invariants());
}

void test_store_zset_methods() {
  goblin::core::Store store;

  assert(store.zadd("z", 2.0, "two") == 1);
  assert(store.zadd("z", 1.0, "one") == 1);
  assert(store.zadd("z", 3.0, "three") == 1);
  assert(store.zadd("z", 0.5, "two") == 0);

  assert(store.zcard("z") == 3);
  assert(store.zscore("z", "two") == 0.5);
  assert(store.zrank("z", "two") == 0U);

  const std::vector<std::string_view> remove_one{"one"};
  assert(store.zrem("z", remove_one) == 1);
  assert(store.zcard("z") == 2);
  assert(!store.zscore("z", "one").has_value());

  const std::vector<std::string_view> remove_rest{"two", "three"};
  assert(store.zrem("z", remove_rest) == 2);
  assert(store.zcard("z") == 0);
  assert(!store.zrank("z", "two").has_value());
}

void test_store_multiple_zsets() {
  goblin::core::Store store;

  // The unified keyspace holds every zset in one namespace; memory_stats reports
  // the live zset count in overflow_zset_count (there is no inline/overflow split
  // any more).
  assert(store.zadd("one", 1.0, "a") == 1);
  assert(store.memory_stats().overflow_zset_count == 1);

  assert(store.zadd("two", 2.0, "b") == 1);
  assert(store.memory_stats().overflow_zset_count == 2);

  assert(store.zscore("one", "a") == 1.0);
  assert(store.zscore("two", "b") == 2.0);

  // Emptying a zset removes its key from the keyspace.
  const std::vector<std::string_view> remove_a{"a"};
  assert(store.zrem("one", remove_a) == 1);
  assert(store.zcard("one") == 0);
  assert(!store.exists("one"));
  assert(store.zscore("two", "b") == 2.0);
  assert(store.memory_stats().overflow_zset_count == 1);

  assert(store.zadd("three", 3.0, "c") == 1);
  assert(store.zscore("two", "b") == 2.0);
  assert(store.zscore("three", "c") == 3.0);
  assert(store.memory_stats().overflow_zset_count == 2);
}

void test_store_shared_member_layer() {
  // Shared member layer is a large-zset optimization; listpack-off forces full.
  goblin::core::Store store(
      goblin::core::StoreOptions{.zset_listpack_max_entries = 0});

  for (int i = 0; i < 128; ++i) {
    assert(store.zadd("key-a", static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }

  for (int i = 0; i < 128; ++i) {
    assert(store.zadd("key-b", static_cast<double>(i), "member-" + std::to_string(i)) == 0);
  }

  assert(store.zcard("key-a") == 128);
  assert(store.zcard("key-b") == 128);
  assert(store.zscore("key-a", "member-0") == 0.0);
  assert(store.zscore("key-b", "member-0") == 0.0);
  assert(store.zrank("key-a", "member-64") == 64U);
  assert(store.zrank("key-b", "member-64") == 64U);

  const auto stats_a = store.zset_memory_stats("key-a");
  const auto stats_b = store.zset_memory_stats("key-b");
  assert(stats_a.has_value());
  assert(stats_b.has_value());
  assert(stats_a->member_layer_share_count >= 2);
  assert(stats_b->member_layer_share_count >= 2);
  assert(stats_a->score_index_share_count == 1);
  assert(stats_b->score_index_share_count == 1);

  assert(store.zadd("key-b", 99.0, "member-0") == 0);
  assert(store.zscore("key-a", "member-0") == 0.0);
  assert(store.zscore("key-b", "member-0") == 99.0);

  const std::vector<std::string_view> remove_one{"member-1"};
  assert(store.zrem("key-b", remove_one) == 1);
  assert(store.zcard("key-b") == 127);
  assert(store.zcard("key-a") == 128);
  assert(store.zscore("key-a", "member-1") == 1.0);
  assert(!store.zscore("key-b", "member-1").has_value());
}

// Two keys sharing a member layer/arena, then EACH doing a structural append of a
// distinct new member, is the sequence that used to corrupt: fork() shallow-copied
// the shared tail block and both sides kept the same next_offset, so the two
// appends landed at the same arena offset and clobbered each other. The COW-on-
// shared-active-block guard in reserve_run_bytes closes it.
void test_store_shared_layer_structural_append_cow() {
  // Shared-layer CoW is a large-zset optimization; listpack-off forces full.
  goblin::core::Store store(
      goblin::core::StoreOptions{.zset_listpack_max_entries = 0});

  for (int i = 0; i < 128; ++i) {
    const auto member = "member-" + std::to_string(i);
    assert(store.zadd("key-a", static_cast<double>(i), member) == 1);
  }
  for (int i = 0; i < 128; ++i) {
    const auto member = "member-" + std::to_string(i);
    assert(store.zadd("key-b", static_cast<double>(i), member) == 0);  // shares key-a
  }
  const auto stats = store.zset_memory_stats("key-a");
  assert(stats.has_value() && stats->member_layer_share_count >= 2);

  // Distinct new members, same length, appended into the shared partial tail block.
  assert(store.zadd("key-a", 1000.0, "only-in-a") == 1);
  assert(store.zadd("key-b", 2000.0, "only-in-b") == 1);

  // Each new member reads back its own bytes (pre-fix, key-b's write overwrote
  // key-a's, so the index lookup for "only-in-a" missed).
  assert(store.zscore("key-a", "only-in-a") == 1000.0);
  assert(store.zscore("key-b", "only-in-b") == 2000.0);
  assert(!store.zscore("key-a", "only-in-b").has_value());
  assert(!store.zscore("key-b", "only-in-a").has_value());
  // Shared members remain intact on both sides.
  assert(store.zscore("key-a", "member-0") == 0.0);
  assert(store.zscore("key-b", "member-127") == 127.0);
  assert(store.zcard("key-a") == 129);
  assert(store.zcard("key-b") == 129);
}

// The small-collection win: a tiny zset must no longer cost a full 1 MiB arena
// block. The first block starts sub-page and grows, so a single-member set's
// member arena is a handful of bytes.
void test_zset_small_collection_footprint() {
  goblin::core::Store store;

  assert(store.zadd("tiny", 1.0, "m") == 1);
  const auto stats = store.zset_memory_stats("tiny");
  assert(stats.has_value());
  assert(stats->member_storage_allocated_bytes < 4096);  // was chunk_bytes (1 MiB)

  constexpr int kCount = 2000;
  for (int i = 0; i < kCount; ++i) {
    assert(store.zadd("z-" + std::to_string(i), 1.0, "member") == 1);
  }
  // Pre-change: >= kCount * 1 MiB (~2 GiB). Now a few MiB of indexes + tiny arenas.
  const auto total = store.memory_stats().total_allocated_bytes;
  assert(total < static_cast<std::size_t>(kCount) * 16384);
}

// Correctness of the growable arena across many blocks: small chunk_bytes forces
// grow -> freeze -> fresh-block cycling, and near-max members force grow-to-fit
// and straddle into fresh blocks. Every member must round-trip (score + bytes).
void test_zset_arena_growth_spans_blocks() {
  goblin::core::StoreOptions opts;
  opts.zset_chunk_bytes = std::size_t{64} << 10;  // 64 KiB (= kMinChunkBytes)
  goblin::core::Store store(opts);

  std::vector<std::pair<double, std::string>> expected;
  for (int i = 0; i < 2000; ++i) {
    std::string member =
        "member-" + std::to_string(i) + std::string(static_cast<std::size_t>(i % 11), 'x');
    assert(store.zadd("z", static_cast<double>(i), member) == 1);
    expected.emplace_back(static_cast<double>(i), member);
  }
  for (int k = 0; k < 3; ++k) {  // ~60 KiB members: grow-to-fit + straddle
    std::string big(std::size_t{60} << 10, static_cast<char>('A' + k));
    assert(store.zadd("z", 100000.0 + k, big) == 1);
    expected.emplace_back(100000.0 + k, big);
  }
  for (const auto& [score, member] : expected) {
    const auto got = store.zscore("z", member);
    assert(got.has_value() && *got == score);
  }
  assert(store.zcard("z") == expected.size());
}

void test_store_shared_member_layer_skips_unrelated_keys() {
  goblin::core::Store store;

  assert(store.zadd("one", 1.0, "a") == 1);
  assert(store.zadd("two", 2.0, "b") == 1);

  assert(store.zcard("one") == 1);
  assert(store.zcard("two") == 1);
  assert(store.zscore("one", "a") == 1.0);
  assert(store.zscore("two", "b") == 2.0);
  assert(!store.zscore("one", "b").has_value());
  assert(!store.zscore("two", "a").has_value());
}

void test_store_many_zsets_coexist() {
  goblin::core::Store store;

  // Many zsets share one keyspace; each resolves independently and all are
  // counted (the old inline-slot cap no longer applies).
  assert(store.zadd("one", 1.0, "a") == 1);
  assert(store.zadd("two", 2.0, "b") == 1);
  assert(store.zadd("three", 3.0, "c") == 1);
  assert(store.memory_stats().overflow_zset_count == 3);
  assert(store.zscore("one", "a") == 1.0);
  assert(store.zscore("two", "b") == 2.0);
  assert(store.zscore("three", "c") == 3.0);
}

std::string joined_value(const goblin::core::Store& store, std::string_view key) {
  const auto value = store.get(key);
  assert(value.has_value());
  std::string out;
  out.append(value->head);
  out.append(value->tail);
  return out;
}

void test_store_strings() {
  using goblin::core::KeyType;
  goblin::core::Store store;

  store.set("k", "42");
  assert(joined_value(store, "k") == "42");
  assert(store.get("k")->tail.empty());  // small values are fully inline
  assert(store.strlen("k") == 2U);
  assert(store.key_type("k") == KeyType::String);
  assert(store.exists("k") && store.key_is_string("k"));

  // 14 bytes is the largest fully-inline value; 15 spills a tail into the arena.
  store.set("edge", "0123456789abcd");  // 14
  assert(store.get("edge")->tail.empty());
  store.set("edge2", "0123456789abcde");  // 15
  assert(!store.get("edge2")->tail.empty());

  const std::string big(1000, 'x');
  store.set("big", big);
  assert(joined_value(store, "big") == big);
  assert(!store.get("big")->tail.empty());
  assert(store.strlen("big") == 1000U);

  // Overwrite spilled -> inline reclaims the tail; the value stays correct.
  store.set("big", "hi");
  assert(joined_value(store, "big") == "hi");
  assert(store.get("big")->tail.empty());

  assert(!store.set_nx("k", "nope"));    // exists
  assert(store.set_nx("fresh", "yes"));  // absent
  assert(joined_value(store, "fresh") == "yes");

  // APPEND creates, then grows across the inline boundary.
  assert(store.append("app", "abc") == 3U);
  assert(store.append("app", "defghijklmnop") == 16U);
  assert(joined_value(store, "app") == "abcdefghijklmnop");

  assert(store.get_set("fresh", "changed") == std::optional<std::string>("yes"));
  assert(joined_value(store, "fresh") == "changed");
  assert(store.get_del("fresh") == std::optional<std::string>("changed"));
  assert(!store.exists("fresh"));

  assert(store.incr_by("counter", 1) == 1LL);
  assert(store.incr_by("counter", 41) == 42LL);
  assert(store.incr_by("counter", -2) == 40LL);
  assert(joined_value(store, "counter") == "40");
  store.set("notint", "abc");
  assert(!store.incr_by("notint", 1).has_value());
  assert(store.incr_by_float("f", 3.5) == std::optional<std::string>("3.5"));
  assert(store.incr_by_float("f", 1.5) == std::optional<std::string>("5"));

  assert(store.del("k"));
  assert(!store.exists("k"));
  assert(!store.del("k"));

  // Clobber: SET over a zset replaces it -- one object per name.
  assert(store.zadd("z", 1.0, "m") == 1);
  assert(store.key_type("z") == KeyType::Zset);
  store.set("z", "now-a-string");
  assert(store.key_type("z") == KeyType::String);
  assert(!store.zscore("z", "m").has_value());
  assert(joined_value(store, "z") == "now-a-string");

  assert(store.key_type("absent") == std::nullopt);
  assert(!store.get("absent").has_value());
  assert(!store.strlen("absent").has_value());
}

void test_store_string_arena_compaction() {
  goblin::core::Store store;

  std::vector<std::string> keys;
  for (int i = 0; i < 200; ++i) {
    std::string key = "c" + std::to_string(i);
    store.set(key, std::string(1024, static_cast<char>('a' + (i % 26))));
    keys.push_back(std::move(key));
  }
  // Overwrite each with a short value: ~200 KiB of tails go dead, past the
  // compaction floor, so the arena rebuilds at least once mid-stream.
  for (const auto& key : keys) {
    store.set(key, "short-" + key);
  }
  for (const auto& key : keys) {
    assert(joined_value(store, key) == "short-" + key);
  }
  // Values written after a compaction are addressed in the fresh arena.
  for (int i = 0; i < 100; ++i) {
    store.set("z" + std::to_string(i), std::string(2000, 'Z'));
  }
  for (int i = 0; i < 100; ++i) {
    const auto value = store.get("z" + std::to_string(i));
    assert(value.has_value() && value->size() == 2000);
  }
}

void test_string_commands() {
  goblin::core::Store store;

  assert(execute_fields(store, {"SET", "k", "hello"}) == "+OK\r\n");
  assert(execute_fields(store, {"GET", "k"}) == "$5\r\nhello\r\n");
  assert(execute_fields(store, {"STRLEN", "k"}) == ":5\r\n");
  assert(execute_fields(store, {"TYPE", "k"}) == "+string\r\n");
  assert(execute_fields(store, {"EXISTS", "k"}) == ":1\r\n");
  assert(execute_fields(store, {"GET", "absent"}) == "$-1\r\n");
  assert(execute_fields(store, {"TYPE", "absent"}) == "+none\r\n");

  // A value past the inline cap round-trips through the arena tail.
  const std::string big(500, 'x');
  assert(execute_fields(store, {"SET", "big", big}) == "+OK\r\n");
  assert(execute_fields(store, {"GET", "big"}) == "$500\r\n" + big + "\r\n");

  // Values over the 64 KiB ceiling are rejected with a pointer to the store.
  const std::string too_big(70000, 'z');
  assert(execute_fields(store, {"SET", "toobig", too_big}) ==
         "-ERR value is larger than the 64 KiB limit; use "
         "https://goblin-store.dev\r\n");

  assert(execute_fields(store, {"SETNX", "k", "nope"}) == ":0\r\n");
  assert(execute_fields(store, {"SETNX", "fresh", "yes"}) == ":1\r\n");
  assert(execute_fields(store, {"GET", "fresh"}) == "$3\r\nyes\r\n");
  assert(execute_fields(store, {"SET", "fresh", "x", "NX"}) == "$-1\r\n");
  assert(execute_fields(store, {"SET", "fresh2", "y", "NX"}) == "+OK\r\n");

  assert(execute_fields(store, {"APPEND", "a", "foo"}) == ":3\r\n");
  assert(execute_fields(store, {"APPEND", "a", "bar"}) == ":6\r\n");
  assert(execute_fields(store, {"GET", "a"}) == "$6\r\nfoobar\r\n");

  assert(execute_fields(store, {"INCR", "n"}) == ":1\r\n");
  assert(execute_fields(store, {"INCRBY", "n", "9"}) == ":10\r\n");
  assert(execute_fields(store, {"DECR", "n"}) == ":9\r\n");
  assert(execute_fields(store, {"DECRBY", "n", "4"}) == ":5\r\n");
  assert(execute_fields(store, {"SET", "n", "abc"}) == "+OK\r\n");
  assert(execute_fields(store, {"INCR", "n"}) ==
         "-ERR value is not an integer or out of range\r\n");
  assert(execute_fields(store, {"INCRBYFLOAT", "f", "3.5"}) == "$3\r\n3.5\r\n");
  assert(execute_fields(store, {"INCRBYFLOAT", "f", "1.5"}) == "$1\r\n5\r\n");

  assert(execute_fields(store, {"GETSET", "k", "world"}) == "$5\r\nhello\r\n");
  assert(execute_fields(store, {"GET", "k"}) == "$5\r\nworld\r\n");
  assert(execute_fields(store, {"GETDEL", "k"}) == "$5\r\nworld\r\n");
  assert(execute_fields(store, {"EXISTS", "k"}) == ":0\r\n");

  assert(execute_fields(store, {"MSET", "x", "1", "y", "2"}) == "+OK\r\n");
  assert(execute_fields(store, {"MGET", "x", "y", "absent"}) ==
         "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$-1\r\n");
  assert(execute_fields(store, {"DEL", "x", "y", "absent"}) == ":2\r\n");

  // WRONGTYPE gating, both directions, and MGET's nil-not-error rule.
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"GET", "z"}) == wrongtype);
  assert(execute_fields(store, {"APPEND", "z", "x"}) == wrongtype);
  assert(execute_fields(store, {"INCR", "z"}) == wrongtype);
  assert(execute_fields(store, {"SET", "s", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"ZADD", "s", "1", "m"}) == wrongtype);
  assert(execute_fields(store, {"MGET", "z"}) == "*1\r\n$-1\r\n");
  assert(execute_fields(store, {"TYPE", "z"}) == "+zset\r\n");

  // SET clobbers a zset -- one object per name, no WRONGTYPE.
  assert(execute_fields(store, {"SET", "z", "clobbered"}) == "+OK\r\n");
  assert(execute_fields(store, {"TYPE", "z"}) == "+string\r\n");
  assert(execute_fields(store, {"GET", "z"}) == "$9\r\nclobbered\r\n");
}

void test_goblin_cad() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // A missing key has nothing to compare -> 0.
  assert(execute_fields(store, {"GOBLIN.CAD", "lock", "tokenA"}) == ":0\r\n");

  // A mismatched token leaves the value untouched -> 0.
  assert(execute_fields(store, {"SET", "lock", "tokenA"}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "lock", "tokenB"}) == ":0\r\n");
  assert(execute_fields(store, {"GET", "lock"}) == "$6\r\ntokenA\r\n");

  // The matching token deletes it and reports one key removed; a second attempt
  // then finds nothing.
  assert(execute_fields(store, {"GOBLIN.CAD", "lock", "tokenA"}) == ":1\r\n");
  assert(execute_fields(store, {"EXISTS", "lock"}) == ":0\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "lock", "tokenA"}) == ":0\r\n");

  // A spilled (out-of-line) value is compared across the head/tail split: a
  // difference in the tail, and one in the inline prefix, both miss.
  const std::string big(500, 'x');
  assert(execute_fields(store, {"SET", "big", big}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "big", std::string(499, 'x') + "y"}) ==
         ":0\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "big", "y" + std::string(499, 'x')}) ==
         ":0\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "big", big}) == ":1\r\n");
  assert(execute_fields(store, {"EXISTS", "big"}) == ":0\r\n");

  // A non-string key is WRONGTYPE, exactly as GET would be.
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "z", "whatever"}) == wrongtype);

  // Arity: exactly key + expected (checked at the parse layer).
  std::vector<std::string_view> too_few = {"GOBLIN.CAD", "lock"};
  assert(!goblin::core::parse_command(too_few).ok());
  std::vector<std::string_view> too_many = {"GOBLIN.CAD", "lock", "a", "b"};
  assert(!goblin::core::parse_command(too_many).ok());
}

void test_goblin_caexpire() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // Deterministic TTL checks via the Store method (explicit now / when_ms).
  store.set("lock", "tok");
  const std::uint64_t now = 1000;
  assert(store.compare_and_expire("lock", "tok", now + 30000, now));  // match -> set
  assert(store.pttl_ms("lock", now) == 30000);
  // A mismatched token leaves the TTL untouched.
  assert(!store.compare_and_expire("lock", "nope", now + 5000, now));
  assert(store.pttl_ms("lock", now) == 30000);
  // Renewing again extends the lease.
  assert(store.compare_and_expire("lock", "tok", now + 45000, now));
  assert(store.pttl_ms("lock", now) == 45000);
  // A missing key has nothing to renew.
  assert(!store.compare_and_expire("absent", "tok", now + 1000, now));
  // A when_ms already at/past now deletes the key (PEXPIRE with a non-positive
  // TTL), and still counts as applied.
  assert(store.compare_and_expire("lock", "tok", now, now));
  assert(!store.exists("lock"));

  // Command layer: reply values, WRONGTYPE, and a non-integer ms.
  assert(execute_fields(store, {"SET", "k", "tok"}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.CAEXPIRE", "k", "nope", "1000"}) == ":0\r\n");
  assert(execute_fields(store, {"GOBLIN.CAEXPIRE", "k", "tok", "1000"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.CAEXPIRE", "absent", "tok", "1000"}) == ":0\r\n");
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.CAEXPIRE", "z", "x", "1000"}) == wrongtype);
  assert(execute_fields(store, {"GOBLIN.CAEXPIRE", "k", "tok", "abc"}).front() == '-');

  // Arity: exactly key + expected + ms (checked at the parse layer).
  std::vector<std::string_view> few = {"GOBLIN.CAEXPIRE", "k", "tok"};
  assert(!goblin::core::parse_command(few).ok());
  std::vector<std::string_view> many = {"GOBLIN.CAEXPIRE", "k", "tok", "1", "2"};
  assert(!goblin::core::parse_command(many).ok());
}

void test_goblin_cas() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // Basic swap: a mismatched token leaves the value; the matching token replaces
  // it and replies +OK (what SET returns), a mismatch replies 0.
  assert(execute_fields(store, {"SET", "k", "tok"}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.CAS", "k", "wrong", "new"}) == ":0\r\n");
  assert(execute_fields(store, {"GET", "k"}) == "$3\r\ntok\r\n");
  assert(execute_fields(store, {"GOBLIN.CAS", "k", "tok", "new"}) == "+OK\r\n");
  assert(execute_fields(store, {"GET", "k"}) == "$3\r\nnew\r\n");

  // A missing key: nothing to swap, and it is NOT created.
  assert(execute_fields(store, {"GOBLIN.CAS", "absent", "a", "b"}) == ":0\r\n");
  assert(execute_fields(store, {"EXISTS", "absent"}) == ":0\r\n");

  // TTL is preserved across a successful swap (the KEEPTTL point) -- checked
  // deterministically via the Store method with an explicit now.
  store.set("lock", "t0");
  const std::uint64_t now = 1000;
  assert(store.expire_at_ms("lock", now + 30000, now));
  assert(store.pttl_ms("lock", now) == 30000);
  assert(store.compare_and_set("lock", "t0", "t1"));   // swap the value...
  assert(store.pttl_ms("lock", now) == 30000);         // ... TTL survives
  const auto v = store.get("lock");
  assert(v && v->head == "t1" && v->tail.empty());     // ... value changed
  // A mismatched CAS touches neither the value nor the TTL.
  assert(!store.compare_and_set("lock", "wrong", "t2"));
  assert(store.pttl_ms("lock", now) == 30000);

  // A non-string key is WRONGTYPE, like GET.
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.CAS", "z", "x", "y"}) == wrongtype);

  // A new value over the 64 KiB cap is rejected (before any swap).
  const std::string huge(64 * 1024 + 1, 'x');
  assert(execute_fields(store, {"GOBLIN.CAS", "k", "new", huge}).front() == '-');

  // Arity: exactly key + expected + new (checked at the parse layer).
  std::vector<std::string_view> few = {"GOBLIN.CAS", "k", "tok"};
  assert(!goblin::core::parse_command(few).ok());
  std::vector<std::string_view> many = {"GOBLIN.CAS", "k", "tok", "a", "b"};
  assert(!goblin::core::parse_command(many).ok());
}

void test_goblin_td_leaderboard_rescore() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // Board: m0@100 .. m4@500 (score = activity timestamp, ascending).
  for (int i = 0; i < 5; ++i) {
    assert(execute_fields(store, {"ZADD", "lb", std::to_string((i + 1) * 100),
                                  "m" + std::to_string(i)}) == ":1\r\n");
  }

  // STEP: the window [now-hl, now] = [350, 500] covers m3, m4 (weight 1); the
  // rest are 0. Top-3 by weight desc, ties broken by ZRANGE order, is m3, m4,
  // then the first-seen 0-weight survivor (m0). tostring(1.0)="1", tostring(0.0)="0".
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "500", "150",
                                "3", "STEP"}) ==
         "*6\r\n$2\r\nm3\r\n$1\r\n1\r\n$2\r\nm4\r\n$1\r\n1\r\n$2\r\nm0\r\n$1\r\n0\r\n");

  // LINEAR and EXP both rank the most-recent (highest ts) member first; k pairs.
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "500", "100",
                                "2", "LINEAR"})
             .rfind("*4\r\n$2\r\nm4\r\n", 0) == 0);
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "500", "100",
                                "2", "EXP"})
             .rfind("*4\r\n$2\r\nm4\r\n", 0) == 0);

  // Errors and edge cases.
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "500", "100",
                                "3", "NOPE"}) ==
         "-ERR mode must be LINEAR, EXP or STEP\r\n");
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "absent", "500",
                                "100", "3", "LINEAR"}) == "*0\r\n");
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "500", "100",
                                "0", "LINEAR"}) == "*0\r\n");
  assert(execute_fields(store, {"SET", "s", "x"}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "s", "500", "100",
                                "3", "LINEAR"}) == wrongtype);
  assert(execute_fields(store, {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "x", "100",
                                "3", "LINEAR"})
             .front() == '-');  // now is not a number

  // Arity: exactly key + now + half_life + k + mode.
  std::vector<std::string_view> few = {"GOBLIN.TD_LEADERBOARD_RESCORE", "lb", "500",
                                       "100", "3"};
  assert(!goblin::core::parse_command(few).ok());
}

void test_memory_report() {
  goblin::core::Store store;

  // An empty store has allocated nothing and has nothing to reclaim.
  assert(store.memory_report().used_memory == 0);
  assert(store.memory_report().reclaimable_bytes == 0);

  // A zset grows used_memory with no dead bytes yet.
  for (int i = 0; i < 200; ++i) {
    (void)execute_fields(store, {"ZADD", "z", std::to_string(i),
                                 "member:" + std::to_string(i)});
  }
  const auto after_add = store.memory_report();
  assert(after_add.used_memory > 0);
  assert(after_add.reclaimable_bytes == 0);

  // Removing members orphans their arena bytes -> reclaimable > 0 (50 dead vs 150
  // live stays under the auto-compaction trigger, so it persists).
  for (int i = 0; i < 50; ++i) {
    (void)execute_fields(store, {"ZREM", "z", "member:" + std::to_string(i)});
  }
  assert(store.memory_report().reclaimable_bytes > 0);

  // Compaction reclaims every dead byte.
  (void)execute_fields(store, {"GOBLIN.OPTIMIZE", "z"});
  assert(store.memory_report().reclaimable_bytes == 0);

  // Hashes feed the same accounting: HDEL orphans field-value bytes.
  for (int i = 0; i < 200; ++i) {
    (void)execute_fields(store, {"HSET", "h", "f" + std::to_string(i),
                                 "value-" + std::to_string(i)});
  }
  assert(store.memory_report().reclaimable_bytes == 0);
  for (int i = 0; i < 50; ++i) {
    (void)execute_fields(store, {"HDEL", "h", "f" + std::to_string(i)});
  }
  assert(store.memory_report().reclaimable_bytes > 0);
}

void test_goblin_increx() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // Deterministic window via the Store method (explicit now / when).
  const std::uint64_t now = 1000;
  const auto v1 = store.incr_expire("rl", now + 100000, now);
  assert(v1 && *v1 == 1);
  assert(store.pttl_ms("rl", now) == 100000);  // window armed on the first write
  // A later increment keeps the running window (only a result of 1 arms it), so
  // the `when` here is ignored -- the fixed window ticks from the first hit.
  const auto v2 = store.incr_expire("rl", now + 50000, now);
  assert(v2 && *v2 == 2);
  assert(store.pttl_ms("rl", now) == 100000);
  const auto v3 = store.incr_expire("rl", now + 50000, now);
  assert(v3 && *v3 == 3);
  assert(store.pttl_ms("rl", now) == 100000);
  // A non-integer value increments nothing.
  store.set("bad", "abc");
  assert(!store.incr_expire("bad", now + 1000, now).has_value());

  // Command layer: replies, WRONGTYPE, and bad arguments.
  assert(execute_fields(store, {"GOBLIN.INCREX", "c", "100"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.INCREX", "c", "100"}) == ":2\r\n");
  assert(execute_fields(store, {"SET", "ni", "abc"}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.INCREX", "ni", "100"}).front() == '-');
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.INCREX", "z", "100"}) == wrongtype);
  assert(execute_fields(store, {"GOBLIN.INCREX", "c", "xyz"}).front() == '-');

  // Arity: exactly key + seconds (checked at the parse layer).
  std::vector<std::string_view> few = {"GOBLIN.INCREX", "c"};
  assert(!goblin::core::parse_command(few).ok());
  std::vector<std::string_view> many = {"GOBLIN.INCREX", "c", "1", "2"};
  assert(!goblin::core::parse_command(many).ok());
}

void test_zremrangebyscore() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // Full representation (past the listpack threshold): scores 1..100.
  for (int i = 1; i <= 100; ++i) {
    (void)execute_fields(store, {"ZADD", "z", std::to_string(i),
                                 "m" + std::to_string(i)});
  }
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "z", "10", "20"}) == ":11\r\n");
  assert(execute_fields(store, {"ZCARD", "z"}) == ":89\r\n");
  // Exclusive bounds: (30, 40) removes 31..39 only.
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "z", "(30", "(40"}) == ":9\r\n");
  // Open sides via -inf / +inf.
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "z", "-inf", "5"}) == ":5\r\n");
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "z", "95", "+inf"}) == ":6\r\n");
  assert(execute_fields(store, {"ZCARD", "z"}) == ":69\r\n");
  // Emptying the zset drops the key.
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "z", "-inf", "+inf"}) == ":69\r\n");
  assert(execute_fields(store, {"EXISTS", "z"}) == ":0\r\n");

  // Listpack representation (tiny) takes the same path correctly.
  for (int i = 1; i <= 5; ++i) {
    (void)execute_fields(store, {"ZADD", "lp", std::to_string(i),
                                 "x" + std::to_string(i)});
  }
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "lp", "2", "4"}) == ":3\r\n");
  assert(execute_fields(store, {"ZRANGE", "lp", "0", "-1"}) ==
         "*2\r\n$2\r\nx1\r\n$2\r\nx5\r\n");

  // Errors and edges.
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "absent", "0", "100"}) == ":0\r\n");
  assert(execute_fields(store, {"SET", "s", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "s", "0", "100"}) == wrongtype);
  assert(execute_fields(store, {"ZREMRANGEBYSCORE", "lp", "abc", "100"}).front() == '-');
  std::vector<std::string_view> bad = {"ZREMRANGEBYSCORE", "lp", "0"};
  assert(!goblin::core::parse_command(bad).ok());
}

void test_goblin_zwindow() {
  goblin::core::Store store;
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";

  // Deterministic admit/reject and TTL via the Store method (explicit clock).
  const std::uint64_t clock = 9000;
  assert(store.zwindow("rl", 1000, 990, 2, "a", clock + 10000, clock));   // admitted
  assert(store.zcard("rl") == 1);
  assert(store.pttl_ms("rl", clock) == 10000);                            // window armed
  assert(store.zwindow("rl", 1001, 991, 2, "b", clock + 10000, clock));   // admitted
  assert(store.zcard("rl") == 2);
  assert(!store.zwindow("rl", 1002, 992, 2, "c", clock + 10000, clock));  // full -> reject
  assert(store.zcard("rl") == 2);
  // Slide the window: now=1020, cutoff=1010 evicts a(1000) and b(1001), so there
  // is room again.
  assert(store.zwindow("rl", 1020, 1010, 2, "d", clock + 10000, clock));
  assert(store.zcard("rl") == 1);

  // Capacity 1 is a mutex: the holder wins, the contender is rejected.
  assert(store.zwindow("mx", 5000, 4970, 1, "o1", clock + 30000, clock));
  assert(!store.zwindow("mx", 5001, 4971, 1, "o2", clock + 30000, clock));

  // Command layer: admit/reject, WRONGTYPE, bad args, arity.
  assert(execute_fields(store, {"GOBLIN.ZWINDOW", "c", "1000", "10", "1", "x"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.ZWINDOW", "c", "1001", "10", "1", "y"}) == ":0\r\n");
  assert(execute_fields(store, {"SET", "str", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"GOBLIN.ZWINDOW", "str", "1000", "10", "1", "x"}) == wrongtype);
  assert(execute_fields(store, {"GOBLIN.ZWINDOW", "c", "notnum", "10", "1", "x"}).front() == '-');
  std::vector<std::string_view> few = {"GOBLIN.ZWINDOW", "c", "1000", "10", "1"};
  assert(!goblin::core::parse_command(few).ok());
}

void test_string_range_commands() {
  goblin::core::Store store;

  // GETRANGE: Redis index rules (inclusive, negatives from the end, clamped).
  assert(execute_fields(store, {"SET", "k", "Hello World"}) == "+OK\r\n");
  assert(execute_fields(store, {"GETRANGE", "k", "0", "4"}) == "$5\r\nHello\r\n");
  assert(execute_fields(store, {"GETRANGE", "k", "-5", "-1"}) == "$5\r\nWorld\r\n");
  assert(execute_fields(store, {"GETRANGE", "k", "0", "-1"}) ==
         "$11\r\nHello World\r\n");
  assert(execute_fields(store, {"GETRANGE", "k", "6", "100"}) ==
         "$5\r\nWorld\r\n");  // end clamps to the last index
  assert(execute_fields(store, {"GETRANGE", "k", "10", "5"}) ==
         "$0\r\n\r\n");  // start > end
  assert(execute_fields(store, {"GETRANGE", "absent", "0", "-1"}) == "$0\r\n\r\n");

  // GETRANGE spanning the inline/tail boundary (a spilled 20-byte value).
  assert(execute_fields(store, {"SET", "big", "0123456789ABCDEFGHIJ"}) ==
         "+OK\r\n");
  assert(execute_fields(store, {"GETRANGE", "big", "12", "17"}) ==
         "$6\r\nCDEFGH\r\n");

  // SETRANGE overwrites in place, zero-pads a gap, and reports the new length.
  assert(execute_fields(store, {"SET", "s", "Hello World"}) == "+OK\r\n");
  assert(execute_fields(store, {"SETRANGE", "s", "6", "Redis"}) == ":11\r\n");
  assert(execute_fields(store, {"GET", "s"}) == "$11\r\nHello Redis\r\n");
  assert(execute_fields(store, {"SETRANGE", "pad", "5", "xy"}) == ":7\r\n");
  assert(execute_fields(store, {"GET", "pad"}) ==
         std::string("$7\r\n") + std::string(5, '\0') + "xy\r\n");

  // SETRANGE with an empty value never creates or changes the string.
  assert(execute_fields(store, {"SETRANGE", "gone", "0", ""}) == ":0\r\n");
  assert(!store.exists("gone"));
  assert(execute_fields(store, {"SETRANGE", "s", "0", ""}) == ":11\r\n");

  // A negative offset is an error.
  assert(execute_fields(store, {"SETRANGE", "s", "-1", "x"}) ==
         "-ERR offset is out of range\r\n");

  // Both gate on WRONGTYPE.
  const std::string wrongtype =
      "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n";
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"GETRANGE", "z", "0", "-1"}) == wrongtype);
  assert(execute_fields(store, {"SETRANGE", "z", "0", "x"}) == wrongtype);
}

void test_string_snapshot_roundtrip() {
  using goblin::core::Store;
  Store store;
  store.set("k", "hello");
  store.set("big", std::string(1000, 'q'));  // spilled value
  store.set("empty", "");
  assert(store.zadd("z", 1.0, "m") == 1);  // mixed types share the keyspace
  assert(store.hset("h", "field", "value") == 1);

  std::ostringstream out(std::ios::binary);
  store.save(out);
  const std::string bytes = out.str();

  Store restored;
  std::istringstream in(bytes, std::ios::binary);
  const auto stats = restored.load(in);
  assert(stats.keys == 5);
  assert(joined_value(restored, "k") == "hello");
  assert(joined_value(restored, "big") == std::string(1000, 'q'));
  assert(joined_value(restored, "empty").empty());
  assert(restored.key_type("empty") == goblin::core::KeyType::String);
  assert(restored.zscore("z", "m") == 1.0);
  assert(restored.hget("h", "field").value() == "value");
}

void test_ttl_set() {
  goblin::core::TtlSet ttl;
  assert(ttl.empty());

  ttl.set(10, 5000);
  ttl.set(20, 3000);
  ttl.set(30, 8000);
  assert(ttl.size() == 3);
  assert(ttl.contains(10) && ttl.contains(20) && ttl.contains(30));
  assert(ttl.expiry(10) == std::optional<std::uint64_t>(5000));
  assert(!ttl.contains(99) && ttl.expiry(99) == std::nullopt);

  // Updating an expiry keeps one entry and re-sorts it.
  ttl.set(10, 1000);
  assert(ttl.expiry(10) == std::optional<std::uint64_t>(1000));
  assert(ttl.size() == 3);

  assert(ttl.clear(30));
  assert(!ttl.contains(30) && !ttl.clear(30));
  assert(ttl.size() == 2);

  // expire_due pops the soonest first, and only what is due.
  std::vector<std::uint64_t> expired;
  assert(ttl.expire_due(999, 100, [&](std::uint64_t id) {
    expired.push_back(id);
  }) == 0);
  assert(expired.empty());
  assert(ttl.expire_due(3000, 100, [&](std::uint64_t id) {
    expired.push_back(id);
  }) == 2);
  assert((expired == std::vector<std::uint64_t>{10, 20}));  // 1000 then 3000

  // The budget caps how many are expired per call.
  ttl.set(1, 100);
  ttl.set(2, 100);
  ttl.set(3, 100);
  assert(ttl.expire_due(200, 2, [](std::uint64_t) {}) == 2);
  assert(ttl.size() == 1);
  assert(ttl.expire_due(200, 100, [](std::uint64_t) {}) == 1);
  assert(ttl.empty());
  assert(ttl.empty());

  // rekey moves an entry to a new id; unknown ids are a no-op.
  ttl.set(100, 5000);
  ttl.rekey(100, 7);
  assert(!ttl.contains(100) && ttl.contains(7));
  assert(ttl.expiry(7) == std::optional<std::uint64_t>(5000));
  ttl.rekey(999, 1);
  assert(!ttl.contains(1));

  // 48-bit ids and expiries round-trip through the 32+16 packing.
  const std::uint64_t big_id = (std::uint64_t{1} << 47) | 0x1234U;
  const std::uint64_t big_expiry = (std::uint64_t{1} << 44) | 0xABCDU;
  ttl.set(big_id, big_expiry);
  assert(ttl.expiry(big_id) == std::optional<std::uint64_t>(big_expiry));
}

long long resp_integer(const std::string& reply) {
  assert(reply.size() >= 4 && reply.front() == ':');
  return std::stoll(reply.substr(1));
}

void test_ttl_commands() {
  goblin::core::Store store;

  assert(execute_fields(store, {"SET", "k", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"TTL", "k"}) == ":-1\r\n");       // key, no expiry
  assert(execute_fields(store, {"PTTL", "k"}) == ":-1\r\n");
  assert(execute_fields(store, {"TTL", "absent"}) == ":-2\r\n");  // no such key
  assert(execute_fields(store, {"PERSIST", "k"}) == ":0\r\n");    // nothing to drop

  // EXPIRE / TTL rounding / PERSIST.
  assert(execute_fields(store, {"EXPIRE", "k", "1000"}) == ":1\r\n");
  {
    const auto n = resp_integer(execute_fields(store, {"TTL", "k"}));
    assert(n > 990 && n <= 1000);
  }
  assert(execute_fields(store, {"PERSIST", "k"}) == ":1\r\n");
  assert(execute_fields(store, {"TTL", "k"}) == ":-1\r\n");
  assert(execute_fields(store, {"EXPIRE", "absent", "100"}) == ":0\r\n");

  // A past EXPIREAT deletes the key immediately (still replies 1).
  assert(execute_fields(store, {"SET", "gone", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"EXPIREAT", "gone", "1"}) == ":1\r\n");
  assert(execute_fields(store, {"EXISTS", "gone"}) == ":0\r\n");

  // A future PEXPIREAT: PTTL and PEXPIRETIME report it.
  const std::uint64_t future = store.now_ms() + 500000;
  assert(execute_fields(store, {"SET", "f", "v"}) == "+OK\r\n");
  assert(execute_fields(store,
                        {"PEXPIREAT", "f", std::to_string(future)}) == ":1\r\n");
  {
    const auto n = resp_integer(execute_fields(store, {"PTTL", "f"}));
    assert(n > 490000 && n <= 500000);
  }
  assert(execute_fields(store, {"PEXPIRETIME", "f"}) ==
         ":" + std::to_string(future) + "\r\n");

  // SET clears the TTL; SET ... KEEPTTL keeps it; SET ... EX sets one.
  assert(execute_fields(store, {"SET", "f", "v2"}) == "+OK\r\n");
  assert(execute_fields(store, {"TTL", "f"}) == ":-1\r\n");
  assert(execute_fields(store, {"EXPIRE", "f", "1000"}) == ":1\r\n");
  assert(execute_fields(store, {"SET", "f", "v3", "KEEPTTL"}) == "+OK\r\n");
  assert(resp_integer(execute_fields(store, {"TTL", "f"})) > 990);
  assert(execute_fields(store, {"SET", "g", "v", "EX", "100"}) == "+OK\r\n");
  {
    const auto n = resp_integer(execute_fields(store, {"TTL", "g"}));
    assert(n > 90 && n <= 100);
  }

  // Lazy expiration: a key with a past TTL is gone on next access.
  assert(execute_fields(store, {"SET", "lazy", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"PEXPIRE", "lazy", "1"}) == ":1\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(execute_fields(store, {"GET", "lazy"}) == "$-1\r\n");
  assert(execute_fields(store, {"EXISTS", "lazy"}) == ":0\r\n");

  // Active expiration deletes due keys in a batch.
  assert(execute_fields(store, {"SET", "a1", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"SET", "a2", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"PEXPIRE", "a1", "1"}) == ":1\r\n");
  assert(execute_fields(store, {"PEXPIRE", "a2", "1"}) == ":1\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(store.active_expire(store.now_ms(), 100) == 2);
  assert(execute_fields(store, {"EXISTS", "a1", "a2"}) == ":0\r\n");

  // TTL commands apply to any type, not just strings.
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"EXPIRE", "z", "1000"}) == ":1\r\n");
  assert(resp_integer(execute_fields(store, {"TTL", "z"})) > 990);
}

void test_ttl_snapshot_roundtrip() {
  using goblin::core::Store;
  Store store;
  const auto now = store.now_ms();
  const std::uint64_t k_expiry = now + 1000000;
  const std::uint64_t z_expiry = now + 2000000;
  store.set("k", "v");
  assert(store.expire_at_ms("k", k_expiry, now));
  store.set("noexp", "v");  // key, no TTL
  assert(store.zadd("z", 1.0, "m") == 1);
  assert(store.expire_at_ms("z", z_expiry, now));  // a TTL on a non-string

  std::ostringstream out(std::ios::binary);
  store.save(out);
  const std::string bytes = out.str();

  Store restored;
  std::istringstream in(bytes, std::ios::binary);
  (void)restored.load(in);

  // Absolute expiries survive the round trip; keys without a TTL stay that way.
  assert(restored.expiretime_ms("k") == static_cast<long long>(k_expiry));
  assert(restored.expiretime_ms("z") == static_cast<long long>(z_expiry));
  assert(restored.expiretime_ms("noexp") == -1);
  assert(restored.expiretime_ms("absent") == -2);
}

void test_ttl_load_drops_expired() {
  using goblin::core::Store;
  Store store;
  const auto now = store.now_ms();
  store.set("soon", "v");
  assert(store.expire_at_ms("soon", now + 20, now));       // ~20 ms out
  store.set("later", "v");
  assert(store.expire_at_ms("later", now + 1000000, now));  // far future
  assert(store.zadd("z", 1.0, "m") == 1);
  assert(store.expire_at_ms("z", now + 20, now));           // a soon-expired zset
  store.set("noexp", "v");

  std::ostringstream out(std::ios::binary);
  store.save(out);
  const std::string bytes = out.str();

  std::this_thread::sleep_for(std::chrono::milliseconds(60));  // "soon"/"z" pass

  Store restored;
  std::istringstream in(bytes, std::ios::binary);
  const auto stats = restored.load(in);

  // Keys already expired at load time are dropped, not resurrected.
  assert(!restored.exists("soon"));
  assert(!restored.exists("z"));
  assert(stats.keys == 2);  // only "later" and "noexp" remain
  assert(restored.exists("later"));
  assert(restored.expiretime_ms("later") ==
         static_cast<long long>(now + 1000000));
  assert(restored.exists("noexp"));
}

void test_expire_and_set_options() {
  goblin::core::Store store;

  // EXPIRE NX / XX.
  assert(execute_fields(store, {"SET", "k", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"EXPIRE", "k", "100", "XX"}) == ":0\r\n");  // no TTL
  assert(execute_fields(store, {"EXPIRE", "k", "100", "NX"}) == ":1\r\n");  // sets
  assert(execute_fields(store, {"EXPIRE", "k", "200", "NX"}) == ":0\r\n");  // has TTL
  assert(execute_fields(store, {"EXPIRE", "k", "200", "XX"}) == ":1\r\n");  // sets

  // EXPIRE GT / LT (current ~200s).
  assert(execute_fields(store, {"EXPIRE", "k", "100", "GT"}) == ":0\r\n");  // 100 < 200
  assert(execute_fields(store, {"EXPIRE", "k", "300", "GT"}) == ":1\r\n");  // 300 > 200
  assert(execute_fields(store, {"EXPIRE", "k", "400", "LT"}) == ":0\r\n");  // 400 > 300
  assert(execute_fields(store, {"EXPIRE", "k", "50", "LT"}) == ":1\r\n");   // 50 < 300

  // A key with no expiry is +infinity: GT never sets, LT always sets.
  assert(execute_fields(store, {"SET", "n", "v"}) == "+OK\r\n");
  assert(execute_fields(store, {"EXPIRE", "n", "100", "GT"}) == ":0\r\n");
  assert(execute_fields(store, {"EXPIRE", "n", "100", "LT"}) == ":1\r\n");

  // Incompatible flag combinations.
  assert(execute_fields(store, {"EXPIRE", "k", "1", "NX", "XX"}) ==
         "-ERR NX and XX, GT or LT options at the same time are not "
         "compatible\r\n");
  assert(execute_fields(store, {"EXPIRE", "k", "1", "GT", "LT"}) ==
         "-ERR GT and LT options at the same time are not compatible\r\n");

  // SET NX / XX.
  assert(execute_fields(store, {"SET", "fresh", "v", "XX"}) == "$-1\r\n");  // absent
  assert(!store.exists("fresh"));
  assert(execute_fields(store, {"SET", "fresh", "v", "NX"}) == "+OK\r\n");
  assert(execute_fields(store, {"SET", "fresh", "w", "XX"}) == "+OK\r\n");

  // SET ... GET returns the old value.
  assert(execute_fields(store, {"SET", "g", "old"}) == "+OK\r\n");
  assert(execute_fields(store, {"SET", "g", "new", "GET"}) == "$3\r\nold\r\n");
  assert(execute_fields(store, {"GET", "g"}) == "$3\r\nnew\r\n");
  assert(execute_fields(store, {"SET", "g2", "v", "GET"}) == "$-1\r\n");  // no old

  // SET NX GET on an existing key returns the old value but does not set.
  assert(execute_fields(store, {"SET", "g", "nope", "NX", "GET"}) ==
         "$3\r\nnew\r\n");
  assert(execute_fields(store, {"GET", "g"}) == "$3\r\nnew\r\n");

  // SET ... GET on a non-string key is WRONGTYPE and does not clobber it.
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"SET", "z", "v", "GET"}) ==
         "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
  assert(execute_fields(store, {"TYPE", "z"}) == "+zset\r\n");
}

void run_store_rank_cache_test(goblin::core::RankCacheMode mode) {
  goblin::core::Store store(goblin::core::StoreOptions{.rank_cache_mode = mode});

  for (int i = 0; i < 2048; ++i) {
    assert(store.zadd("z", static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }

  assert(store.zrank("z", "member-0") == 0U);
  assert(store.zrank("z", "member-1024") == 1024U);
  assert(store.zadd("z", 0.5, "member-1024") == 0);
  assert(store.zrank("z", "member-1024") == 1U);

  const std::vector<std::string_view> remove_members{"member-1", "member-2", "member-3"};
  assert(store.zrem("z", remove_members) == 3);
  assert(store.zrank("z", "member-1024") == 1U);
  assert(store.zscore("z", "member-2047") == 2047.0);

  const auto stats = store.zset_memory_stats("z");
  assert(stats.has_value());
  assert(stats->rank_cache_mode == mode);
  assert(stats->rank_location_cache_allocated_bytes > 0);
}

void test_store_rank_location_cache() {
  run_store_rank_cache_test(goblin::core::RankCacheMode::Exact);
  run_store_rank_cache_test(goblin::core::RankCacheMode::BlockHint);
}

std::size_t rank_cache_allocation_for(goblin::core::RankCacheMode mode) {
  goblin::core::Store store(goblin::core::StoreOptions{.rank_cache_mode = mode});
  for (int i = 0; i < 4096; ++i) {
    assert(store.zadd("z", static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }
  const auto stats = store.zset_memory_stats("z");
  assert(stats.has_value());
  assert(stats->rank_cache_mode == mode);
  return stats->rank_location_cache_allocated_bytes;
}

void test_block_hint_rank_cache_uses_narrow_storage() {
  const auto exact_bytes =
      rank_cache_allocation_for(goblin::core::RankCacheMode::Exact);
  const auto block_hint_bytes =
      rank_cache_allocation_for(goblin::core::RankCacheMode::BlockHint);
  assert(block_hint_bytes > 0);
  assert(block_hint_bytes < exact_bytes);
}

std::uint32_t add_score_index_member(goblin::core::ZSetMemberStorage& storage,
                                     goblin::core::ZSetScoreIndex& index,
                                     std::uint32_t member_number) {
  const auto member =
      "member-" + std::to_string(static_cast<unsigned long long>(member_number));
  const auto score = static_cast<double>(member_number);
  const auto member_id = storage.push_back(member, score);
  assert(member_id == member_number);
  index.insert(goblin::core::ZSetScoreEntry{.score = score, .member_id = member_id});
  return member_id;
}

// Scores are stored at the narrowest signed width that holds them, widening
// one-way (i16 -> i32 -> f64) on a value that doesn't fit; -0.0/fractional force
// f64. Every score reads back exactly through the double boundary.
void test_zset_score_width_promotes() {
  using goblin::core::ScoreWidth;

  goblin::core::ZSetMemberStorage storage;
  const auto a = storage.push_back("a", -32768.0);
  const auto b = storage.push_back("b", 32767.0);
  assert(storage.score_width() == ScoreWidth::I16);  // chess range
  assert(storage.score(a) == -32768.0 && storage.score(b) == 32767.0);

  const auto c = storage.push_back("c", 100000.0);  // beyond i16, within i32
  assert(storage.score_width() == ScoreWidth::I32);
  assert(storage.score(a) == -32768.0 && storage.score(c) == 100000.0);

  const auto d = storage.push_back("d", 1.5);  // fractional -> f64
  assert(storage.score_width() == ScoreWidth::F64);
  assert(storage.score(a) == -32768.0 && storage.score(c) == 100000.0 &&
         storage.score(d) == 1.5);

  goblin::core::ZSetMemberStorage big;  // integer beyond i32 -> f64
  const auto e = big.push_back("e", 5000000000.0);
  assert(big.score_width() == ScoreWidth::F64 && big.score(e) == 5000000000.0);

  goblin::core::ZSetMemberStorage neg_zero;  // -0.0 must not narrow to int 0
  const auto f = neg_zero.push_back("f", -0.0);
  assert(neg_zero.score_width() == ScoreWidth::F64 &&
         std::signbit(neg_zero.score(f)));

  goblin::core::ZSetMemberStorage upd;  // set_score widens too
  const auto g = upd.push_back("g", 10.0);
  assert(upd.score_width() == ScoreWidth::I16);
  upd.set_score(g, 2.5);
  assert(upd.score_width() == ScoreWidth::F64 && upd.score(g) == 2.5);
}

// Widening is one-way during normal ops; GOBLIN.OPTIMIZE rescans the live scores
// and demotes back to the narrowest width (its full rebuild re-derives it).
void test_zset_optimize_demotes_score_width() {
  using goblin::core::ScoreWidth;
  goblin::core::Store store;

  for (int i = 0; i < 100; ++i) {
    (void)store.zadd("z", static_cast<double>(i), "m" + std::to_string(i));
  }
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::I16);

  (void)store.zadd("z", 0.5, "frac");  // fractional forces f64
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::F64);

  const std::vector<std::string_view> to_remove{"frac"};
  (void)store.zrem("z", to_remove);  // removing it does NOT auto-demote
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::F64);

  (void)store.optimize("z", 0.97);  // OPTIMIZE rescans and demotes
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::I16);
  assert(store.zscore("z", "m50") == 50.0);
  assert(!store.zscore("z", "frac").has_value());
}

// The sorted score index stores scores at the width too; promoting it (i16 ->
// i32 -> f64 as out-of-range values arrive) must re-encode every block and keep
// the total order (ranks) and exact score values intact across many blocks.
void test_zset_score_index_width_ordering() {
  using goblin::core::ScoreWidth;
  goblin::core::Store store;

  for (int i = 0; i < 1000; ++i) {  // scores [-500, 499], i16, many blocks
    (void)store.zadd("z", static_cast<double>(i - 500), "m" + std::to_string(i));
  }
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::I16);

  (void)store.zadd("z", 100000.0, "big");  // forces i32 (index re-encodes)
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::I32);

  (void)store.zadd("z", 0.25, "frac");  // forces f64
  assert(store.zset_memory_stats("z")->score_width == ScoreWidth::F64);

  // Order and values intact across both promotions.
  assert(store.zcard("z") == 1002);
  assert(store.zrank("z", "m0") == 0);          // score -500, the minimum
  assert(store.zrank("z", "big") == 1001);      // score 100000, the maximum
  assert(store.zscore("z", "m750") == 250.0);
  assert(store.zscore("z", "big") == 100000.0);
  assert(store.zscore("z", "frac") == 0.25);
  // frac (0.25) sits between m500 (0.0) and m501 (1.0).
  assert(store.zrank("z", "frac") == store.zrank("z", "m500").value() + 1);
}

// Deleting most of a zset triggers the id-stable arena compaction: it reclaims the
// member-bytes arena (fresh page-aligned blocks, old munmap'd) while keeping member
// ids and the indexes valid, so every survivor still reads back its exact score.
void test_zset_arena_id_stable_reclaim() {
  goblin::core::Store store;
  constexpr int kN = 100000;
  std::vector<std::string> members;
  members.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    members.push_back("member-" + std::to_string(i) + "-padding");
    (void)store.zadd("z", static_cast<double>(i % 25000), members.back());
  }
  const auto grown = store.zset_memory_stats("z")->member_storage_allocated_bytes;

  std::vector<std::string_view> batch;  // delete the first 90%
  for (int i = 0; i < kN * 9 / 10; ++i) {
    batch.push_back(members[i]);
  }
  (void)store.zrem("z", batch);

  const auto shrunk = store.zset_memory_stats("z")->member_storage_allocated_bytes;
  assert(shrunk < grown / 2);                       // arena reclaimed
  assert(store.zcard("z") == kN - kN * 9 / 10);
  for (int i = kN * 9 / 10; i < kN; i += 137) {     // survivors intact
    assert(store.zscore("z", members[i]) == static_cast<double>(i % 25000));
  }
}

// CompactListpack: one pooled [header][entries]
// allocation so the object is a single pointer (sizeof == 8).
void test_compact_listpack() {
  using goblin::core::CompactListpack;
  using goblin::core::ScoreWidth;
  static_assert(sizeof(CompactListpack) == sizeof(void*));
  constexpr std::size_t kMax = 512;

  CompactListpack lp;
  assert(lp.empty() && lp.size() == 0 && lp.allocated_bytes() == 0);
  assert(lp.add(3.0, "c", kMax).changed);
  assert(lp.add(1.0, "a", kMax).changed);
  assert(lp.add(2.0, "b", kMax).changed);
  assert(lp.size() == 3);
  assert(lp.score("a") == 1.0 && lp.score("b") == 2.0 && lp.score("c") == 3.0);
  assert(!lp.score("z").has_value());
  assert(lp.rank("a") == 0U && lp.rank("c") == 2U);

  std::vector<std::string> fwd, rev;
  lp.for_each([&](double, std::string_view m) { fwd.emplace_back(m); });
  assert(fwd.size() == 3 && fwd[0] == "a" && fwd[2] == "c");
  lp.for_range(0, 3, true, [&](double, std::string_view m) { rev.emplace_back(m); });
  assert(rev[0] == "c" && rev[2] == "a");
  fwd.clear();
  lp.for_range(1, 2, false, [&](double, std::string_view m) { fwd.emplace_back(m); });
  assert(fwd.size() == 2 && fwd[0] == "b" && fwd[1] == "c");

  assert(!lp.add(1.0, "a", kMax).changed);  // same score -> no-op
  assert(lp.add(5.0, "a", kMax).changed);   // move a to the end
  assert(lp.rank("a") == 2U && lp.score("a") == 5.0);

  CompactListpack inplace;
  (void)inplace.add(1.0, "a", kMax);
  (void)inplace.add(2.0, "b", kMax);
  (void)inplace.add(3.0, "c", kMax);
  assert(inplace.add(2.5, "b", kMax).changed);  // score update, same rank slot
  assert(inplace.score("b") == 2.5 && inplace.rank("b") == 1U);
  assert(lp.remove("b") && lp.size() == 2 && !lp.score("b").has_value());

  // width promotion incl. a direct i16 -> f64 jump on a fraction
  CompactListpack w;
  (void)w.add(10.0, "x", kMax);
  assert(w.score_width() == ScoreWidth::I16);
  (void)w.add(100000.0, "big", kMax);
  assert(w.score_width() == ScoreWidth::I32 && w.score("big") == 100000.0);
  (void)w.add(0.5, "frac", kMax);
  assert(w.score_width() == ScoreWidth::F64 && w.score("frac") == 0.5 &&
         w.score("x") == 10.0);

  CompactListpack big;  // 2-byte member length encoding
  const std::string longm(200, 'q');
  assert(big.add(7.0, longm, kMax).changed && big.score(longm) == 7.0);

  CompactListpack limit;  // promotion signals
  for (int i = 0; i < 4; ++i) (void)limit.add(i, "m" + std::to_string(i), 4);
  assert(limit.add(9.0, "over", 4).needs_full);  // count would exceed max
  assert(limit.add(9.0, "", kMax).needs_full);   // empty member has no encoding

  CompactListpack demo;  // OPTIMIZE demotes the width
  for (int i = 0; i < 5; ++i) (void)demo.add(i, "d" + std::to_string(i), kMax);
  (void)demo.add(0.5, "f", kMax);
  assert(demo.score_width() == ScoreWidth::F64);
  demo.remove("f");
  demo.optimize();
  assert(demo.score_width() == ScoreWidth::I16 && demo.score("d3") == 3.0);

  CompactListpack mv = std::move(demo);  // move transfers ownership
  assert(mv.size() == 5 && mv.score("d3") == 3.0);
}

// The keyspace-key arena: keys packed + length-prefixed, resolved by uint64
// offset, with a swiss table keyed by offset but looked up by string_view.
void test_key_arena() {
  using goblin::core::KeyArena;
  using goblin::core::KeyArenaEqual;
  using goblin::core::KeyArenaHash;

  KeyArena arena;
  const auto o1 = arena.append("user:1");
  const auto o2 = arena.append("user:22");
  const std::string long_key(200, 'k');  // 2-byte LEB128 length
  const auto o3 = arena.append(long_key);
  assert(arena.bytes(o1) == "user:1");
  assert(arena.bytes(o2) == "user:22");
  assert(arena.bytes(o3) == long_key);
  assert(arena.live_count() == 3);

  // Swiss table keyed by arena offset, looked up heterogeneously by string_view.
  goblin::core::SwissTable<std::uint64_t, int, KeyArenaHash, KeyArenaEqual> table(
      KeyArenaHash{&arena}, KeyArenaEqual{&arena});
  (void)table.try_emplace(o1, 1);
  (void)table.try_emplace(o2, 2);
  (void)table.try_emplace(o3, 3);
  assert(*table.find(std::string_view("user:1")) == 1);
  assert(*table.find(std::string_view("user:22")) == 2);
  assert(*table.find(std::string_view(long_key)) == 3);
  assert(table.find(std::string_view("user:2")) == nullptr);
  assert(table.find(std::string_view("nope")) == nullptr);

  const auto dead_before = arena.dead_bytes();
  arena.mark_dead(o2);
  assert(arena.live_count() == 2 && arena.dead_bytes() > dead_before);
}

// A ZSet with the listpack enabled dispatches every op to the blob while small,
// then transparently promotes to the full structure once it outgrows the limit --
// results are identical throughout, and it save/loads through the canonical format.
void test_zset_listpack_mode() {
  using goblin::core::ZSet;
  using goblin::core::ZSetOptions;

  // Options must outlive the zsets that point at them; one stable object here.
  const ZSetOptions opts8{.listpack_max_entries = 8};
  ZSet zset(&opts8);
  assert(zset.add(3.0, "c", &opts8) == 1);
  assert(zset.add(1.0, "a", &opts8) == 1);
  assert(zset.add(2.0, "b", &opts8) == 1);
  assert(zset.size() == 3);
  assert(zset.score("a") == 1.0 && zset.score("c") == 3.0 &&
         !zset.score("z").has_value());
  assert(zset.rank("a") == 0U && zset.rank("c") == 2U);
  assert(zset.reverse_rank("c") == 0U);

  auto range = zset.range(0, -1);
  assert(range.size() == 3 && range[0].member == "a" && range[2].member == "c");
  auto rev = zset.reverse_range(0, -1);
  assert(rev.size() == 3 && rev[0].member == "c" && rev[2].member == "a");

  assert(zset.remove("b") && zset.size() == 2);
  assert(zset.add(1.0, "a", &opts8) == 0);  // same score -> no-op

  // Grow past the limit -> promotes to full; every op stays correct.
  for (int i = 0; i < 20; ++i) {
    (void)zset.add(static_cast<double>(100 + i), "big-" + std::to_string(i),
                   &opts8);
  }
  assert(zset.size() == 22);
  assert(zset.score("a") == 1.0 && zset.score("big-10") == 110.0);
  assert(zset.rank("a") == 0U);  // still the minimum
  assert(zset.check_invariants());

  // An empty member has no listpack length encoding -> it promotes to full and
  // every member (including the empty one) stays correct.
  ZSet emptymem(&opts8);
  assert(emptymem.add(1.0, "a", &opts8) == 1);
  assert(emptymem.add(2.0, "", &opts8) == 1);  // empty member -> promote to full
  assert(emptymem.size() == 2 && emptymem.score("") == 2.0 &&
         emptymem.score("a") == 1.0);

  // save/load round-trips a small (listpack) zset through the canonical format.
  ZSet small(&opts8);
  assert(small.add(5.0, "x", &opts8) == 1 && small.add(-2.0, "y", &opts8) == 1);
  assert(small.member_index_capacity() == 0);  // listpack has no swiss index
  std::string buffer;
  goblin::core::snapshot::Writer writer(buffer);
  small.save(writer, /*with_accelerator=*/false, &opts8);
  goblin::core::snapshot::Reader reader(buffer);
  ZSet loaded = ZSet::load(reader, /*use_accelerator=*/false, &opts8);
  assert(loaded.member_index_capacity() == 0);
  assert(loaded.size() == 2 && loaded.score("x") == 5.0 &&
         loaded.score("y") == -2.0);

  // Promote past the limit, then shrink back -- demotes to listpack again.
  ZSet shrink(&opts8);
  for (int i = 0; i < 12; ++i) {
    (void)shrink.add(static_cast<double>(i), "m" + std::to_string(i), &opts8);
  }
  assert(shrink.member_index_capacity() > 0 && shrink.size() == 12);
  for (int i = 0; i < 4; ++i) {
    assert(shrink.remove("m" + std::to_string(i)));
  }
  assert(shrink.member_index_capacity() == 0 && shrink.size() == 8);
  assert(shrink.score("m11") == 11.0);
}

void test_block_hint_rank_cache_lazy_offset_repair() {
  goblin::core::ZSetMemberStorage storage;
  goblin::core::ZSetScoreIndex index(
      &storage, goblin::core::RankCacheMode::BlockHint);

  for (std::uint32_t i = 0; i < 512; ++i) {
    add_score_index_member(storage, index, i);
  }

  const auto before_rank_bytes = index.location_cache_allocated_bytes();
  const auto target =
      goblin::core::ZSetScoreEntry{.score = 256.0, .member_id = 256};
  assert(index.rank(target) == 256U);
  assert(index.location_cache_allocated_bytes() > before_rank_bytes);
  assert(index.rank(target) == 256U);

  assert(index.erase_one(goblin::core::ZSetScoreEntry{.score = 0.0, .member_id = 0}));
  assert(index.rank(target) == 255U);
  assert(index.validate());
}

void test_block_hint_rank_cache_promotes_to_wide_storage() {
  goblin::core::ZSetMemberStorage forced_storage;
  // Pin the load factor so the 600->1200 member growth crosses the 2-block
  // narrow-hint threshold regardless of the (tunable) default load factor.
  goblin::core::ZSetScoreIndex forced_index(
      &forced_storage,
      goblin::core::RankCacheMode::BlockHint,
      2,
      256);

  for (std::uint32_t i = 0; i < 600; ++i) {
    add_score_index_member(forced_storage, forced_index, i);
  }
  assert(forced_index.block_count() <= 2);
  assert(forced_index.rank(goblin::core::ZSetScoreEntry{.score = 0.0,
                                                        .member_id = 0}) == 0U);
  assert(forced_index.rank(goblin::core::ZSetScoreEntry{.score = 599.0,
                                                        .member_id = 599}) == 599U);
  const auto before_promotion_bytes = forced_index.location_cache_allocated_bytes();

  for (std::uint32_t i = 600; i < 1200; ++i) {
    add_score_index_member(forced_storage, forced_index, i);
  }
  assert(forced_index.block_count() > 2);
  assert(forced_index.rank(goblin::core::ZSetScoreEntry{.score = 0.0,
                                                        .member_id = 0}) == 0U);
  assert(forced_index.rank(goblin::core::ZSetScoreEntry{.score = 599.0,
                                                        .member_id = 599}) == 599U);
  assert(forced_index.rank(goblin::core::ZSetScoreEntry{.score = 1199.0,
                                                        .member_id = 1199}) == 1199U);

  goblin::core::ZSetMemberStorage narrow_storage;
  goblin::core::ZSetScoreIndex narrow_index(
      &narrow_storage,
      goblin::core::RankCacheMode::BlockHint);
  for (std::uint32_t i = 0; i < 1200; ++i) {
    add_score_index_member(narrow_storage, narrow_index, i);
  }

  assert(forced_index.location_cache_allocated_bytes() > before_promotion_bytes);
  assert(forced_index.location_cache_allocated_bytes() >
         narrow_index.location_cache_allocated_bytes());
}

void test_goblin_optimize_reclaims_slack() {
  goblin::core::Store store;
  constexpr int kMembers = 6000;
  // Mid-block inserts (shuffled scores) build score-index blocks with capacity
  // slack, and the ref vector over-allocates geometrically.
  for (int i = 0; i < kMembers; ++i) {
    const long long score =
        static_cast<long long>((static_cast<std::uint64_t>(i) * 2654435761ULL) % 100000ULL);
    execute_fields(store, {"ZADD", "k", std::to_string(score), "m" + std::to_string(i)});
  }

  const auto before = store.zset_memory_stats("k");
  assert(before.has_value());
  // The score index carries block slack before optimizing.
  assert(before->score_block_capacity_sum > before->score_entry_count);

  const auto reply = execute_fields(store, {"GOBLIN.OPTIMIZE", "k"});
  assert(reply.starts_with(":"));
  assert(reply != ":0\r\n");  // reclaimed a positive number of bytes

  const auto after = store.zset_memory_stats("k");
  assert(after.has_value());
  assert(after->total_allocated_bytes < before->total_allocated_bytes);
  // Compaction packs blocks: no more than the final partial block is slack.
  assert(after->score_block_capacity_sum - after->score_entry_count <
         goblin::core::ZSetScoreIndex::kDefaultLoad);

  // Data is intact after compaction.
  assert(execute_fields(store, {"ZCARD", "k"}) == ":6000\r\n");
  assert(execute_fields(store, {"ZSCORE", "k", "m0"}) == "$1\r\n0\r\n");
  assert(store.zrank("k", "m0").has_value());

  // Unknown key optimizes to nil.
  assert(execute_fields(store, {"GOBLIN.OPTIMIZE", "missing"}) == "$-1\r\n");
}

void test_goblin_optimize_density_and_growth() {
  goblin::core::Store store;
  constexpr int kMembers = 4000;  // a multiple of the group width
  for (int i = 0; i < kMembers; ++i) {
    execute_fields(store, {"ZADD", "k", std::to_string(i), "m" + std::to_string(i)});
  }
  auto capacity = [&store] {
    return store.zset_memory_stats("k")->member_index_capacity;
  };

  // density 1.0 packs the member index to ~100% load (capacity ~ member count).
  execute_fields(store, {"GOBLIN.OPTIMIZE", "k", "1.0"});
  assert(capacity() >= static_cast<std::size_t>(kMembers) &&
         capacity() < static_cast<std::size_t>(kMembers) + 16);
  assert(execute_fields(store, {"ZSCORE", "k", "m0"}) == "$1\r\n0\r\n");
  assert(execute_fields(store, {"ZCARD", "k"}) == ":4000\r\n");

  // density 0.5 leaves capacity ~ 2x member count.
  execute_fields(store, {"GOBLIN.OPTIMIZE", "k", "0.5"});
  assert(capacity() >= static_cast<std::size_t>(2 * kMembers) &&
         capacity() < static_cast<std::size_t>(2 * kMembers) + 16);
  assert(store.zrank("k", "m0").has_value());

  // Out-of-range densities are rejected.
  assert(execute_fields(store, {"GOBLIN.OPTIMIZE", "k", "0"}).starts_with("-ERR"));
  assert(execute_fields(store, {"GOBLIN.OPTIMIZE", "k", "1.5"}).starts_with("-ERR"));

  // A tight growth factor keeps the incremental (never-optimized) load factor
  // well above the ~50% power-of-two worst case.
  goblin::core::Store tight(
      goblin::core::StoreOptions{.member_index_growth = 1.25});
  for (int i = 0; i < 5000; ++i) {
    (void)tight.zadd("g", static_cast<double>(i), "m" + std::to_string(i));
  }
  const auto stats = tight.zset_memory_stats("g");
  assert(stats.has_value() && stats->member_count == 5000);
  assert(static_cast<double>(stats->member_count) /
             static_cast<double>(stats->member_index_capacity) >
         0.6);
}

void test_snapshot_round_trip() {
  using goblin::core::Store;

  // Standard CRC32C (CRC-32/ISCSI) check value, so the hardware and software
  // paths and every architecture agree on the format's checksum.
  assert(goblin::core::snapshot::checksum("123456789") == 0xE3069283u);

  // Build a store with several keys, duplicate/negative scores, and some
  // removals (to exercise dense-id maintenance and swiss tombstones).
  Store store;
  for (int i = 0; i < 600; ++i) {
    // Many members share a score, so tie-ordering by member bytes is exercised.
    (void)store.zadd("scores", static_cast<double>((i * 13) % 40) - 15.0,
                     "m" + std::to_string(i));
  }
  for (int i = 0; i < 600; i += 7) {
    execute_fields(store, {"ZREM", "scores", "m" + std::to_string(i)});
  }
  (void)store.zadd("pair", 3.5, "pi");
  (void)store.zadd("pair", 3.5, "e");   // equal score, different bytes
  (void)store.zadd("solo", -0.0, "only");

  std::ostringstream out(std::ios::binary);
  store.save(out);
  const std::string bytes = out.str();
  assert(!bytes.empty());

  auto verify = [&store](Store& loaded) {
    for (std::string_view key : {"scores", "pair", "solo"}) {
      assert(loaded.zcard(key) == store.zcard(key));
    }
    for (int i = 1; i < 600; i += 3) {
      const auto member = "m" + std::to_string(i);
      assert(loaded.zscore("scores", member) == store.zscore("scores", member));
      assert(loaded.zrank("scores", member) == store.zrank("scores", member));
    }
    // Tie order (equal scores) must match.
    assert(loaded.zrank("pair", "e") == store.zrank("pair", "e"));
    assert(loaded.zrank("pair", "pi") == store.zrank("pair", "pi"));
    assert(loaded.zscore("solo", "only") == store.zscore("solo", "only"));
  };

  // Fast path: accelerator version matches, so indexes are memcpy-restored.
  {
    Store loaded;
    std::istringstream in(bytes, std::ios::binary);
    const auto stats = loaded.load(in);
    assert(stats.used_accelerator);
    assert(stats.keys == 3);
    verify(loaded);
  }

  // Slow path: bump the accelerator version byte in the header so the
  // accelerator is discarded and indexes rebuild from the canonical layer.
  // The result must be identical (including tie order).
  {
    std::string slow = bytes;
    // Header(16) + section_type(4); section_version low byte is at offset 20.
    slow[20] = static_cast<char>(slow[20] + 1);
    Store loaded;
    std::istringstream in(slow, std::ios::binary);
    const auto stats = loaded.load(in);
    assert(!stats.used_accelerator);
    assert(stats.keys == 3);
    verify(loaded);
  }

  // Hash-identity mismatch (a snapshot written by a different std::hash, i.e.
  // the cross-standard-library / cross-architecture case) also falls back to a
  // canonical rebuild instead of trusting the swiss dump.
  {
    std::string other = bytes;
    other[24] = static_cast<char>(other[24] + 1);  // hash_identity low byte
    Store loaded;
    std::istringstream in(other, std::ios::binary);
    const auto stats = loaded.load(in);
    assert(!stats.used_accelerator);
    assert(stats.keys == 3);
    verify(loaded);
  }

  // Corruption: flipping a payload byte must fail the checksum and leave the
  // store empty (not partially loaded).
  {
    std::string corrupt = bytes;
    corrupt[50] = static_cast<char>(corrupt[50] ^ 0xFF);  // inside the first OP_ZSET operands
    Store loaded;
    (void)loaded.zadd("stale", 1.0, "x");
    std::istringstream in(corrupt, std::ios::binary);
    bool threw = false;
    try {
      (void)loaded.load(in);
    } catch (const goblin::core::snapshot::snapshot_error&) {
      threw = true;
    }
    assert(threw);
    assert(loaded.zcard("stale") == 0);
    assert(loaded.zcard("scores") == 0);
  }

  // A non-snapshot stream is rejected.
  {
    Store loaded;
    std::istringstream in(std::string("not a snapshot at all, really"),
                          std::ios::binary);
    bool threw = false;
    try {
      (void)loaded.load(in);
    } catch (const goblin::core::snapshot::snapshot_error&) {
      threw = true;
    }
    assert(threw);
  }

  // Rank-cache modes are saved per-zset and their location maps are rebuilt on
  // load (via assign_sorted / insert), so ZRANK still works.
  for (auto mode : {goblin::core::RankCacheMode::Exact,
                    goblin::core::RankCacheMode::BlockHint}) {
    Store source(goblin::core::StoreOptions{.rank_cache_mode = mode});
    for (int i = 0; i < 300; ++i) {
      (void)source.zadd("z", static_cast<double>(i % 20), "k" + std::to_string(i));
    }
    std::ostringstream saved(std::ios::binary);
    source.save(saved);

    Store loaded;
    std::istringstream in(saved.str(), std::ios::binary);
    (void)loaded.load(in);
    for (int i = 0; i < 300; i += 5) {
      const auto member = "k" + std::to_string(i);
      assert(loaded.zrank("z", member) == source.zrank("z", member));
      assert(loaded.zscore("z", member) == source.zscore("z", member));
    }
  }
}

// The full persistence cycle on one store: save, clear, reload, same data back.
// An integer-scored (i16) zset serializes its scores at 2 bytes, not 8, so its
// snapshot is markedly smaller than the same members with fractional scores.
void test_snapshot_score_width_shrinks() {
  using goblin::core::Store;

  Store chess;
  Store frac;
  for (int i = 0; i < 500; ++i) {
    const auto member = "m" + std::to_string(i);
    (void)chess.zadd("z", static_cast<double>(i - 250), member);        // i16
    (void)frac.zadd("z", static_cast<double>(i - 250) + 0.5, member);   // f64
  }

  std::ostringstream chess_out(std::ios::binary);
  std::ostringstream frac_out(std::ios::binary);
  chess.save(chess_out);
  frac.save(frac_out);
  const auto chess_bytes = chess_out.str();
  const auto frac_bytes = frac_out.str();
  // 6 bytes/member less (i16 vs f64) across 500 members; be conservative.
  assert(chess_bytes.size() + 500 * 5 < frac_bytes.size());

  Store loaded;
  std::istringstream in(chess_bytes, std::ios::binary);
  (void)loaded.load(in);
  for (int i = 0; i < 500; i += 7) {
    const auto member = "m" + std::to_string(i);
    assert(loaded.zscore("z", member) == static_cast<double>(i - 250));
  }
}

void test_snapshot_save_clear_reload() {
  using goblin::core::Store;
  Store store;

  // Varied data: duplicate, negative, and zero scores; an empty member; several
  // keys; and removals (dense-id and tombstone paths).
  const std::vector<std::string> keys = {"alpha", "beta", "gamma"};
  for (int i = 0; i < 400; ++i) {
    (void)store.zadd("alpha", static_cast<double>((i * 11) % 30) - 10.0,
                     "m" + std::to_string(i));
  }
  for (int i = 0; i < 400; i += 9) {
    execute_fields(store, {"ZREM", "alpha", "m" + std::to_string(i)});
  }
  (void)store.zadd("beta", 0.0, "");    // empty member
  (void)store.zadd("beta", 1.5, "x");
  (void)store.zadd("beta", 1.5, "y");   // equal score -> ordered by member bytes
  (void)store.zadd("gamma", -3.25, "only");

  // Record the full observable state of each key (count + every member and
  // score, in rank order).
  auto full_range = [&store](const std::string& key) {
    return execute_fields(store, {"ZRANGE", key, "0", "-1", "WITHSCORES"});
  };
  std::vector<long long> cards;
  std::vector<std::string> ranges;
  for (const auto& key : keys) {
    cards.push_back(store.zcard(key));
    ranges.push_back(full_range(key));
  }

  // Save, then clear the same store in place.
  std::ostringstream out(std::ios::binary);
  store.save(out);
  store.clear();
  for (const auto& key : keys) {
    assert(store.zcard(key) == 0);  // genuinely emptied
  }

  // Reload into the same store; every key must come back identical.
  std::istringstream in(out.str(), std::ios::binary);
  const auto stats = store.load(in);
  assert(stats.keys == keys.size());
  assert(stats.used_accelerator);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    assert(store.zcard(keys[i]) == cards[i]);
    assert(full_range(keys[i]) == ranges[i]);
  }

  // Canonical-only save (accelerator dropped): smaller file, reports no
  // accelerator on load, still round-trips exactly.
  std::ostringstream lean(std::ios::binary);
  store.save(lean, /*with_accelerator=*/false);
  assert(lean.str().size() < out.str().size());
  store.clear();
  std::istringstream lean_in(lean.str(), std::ios::binary);
  const auto lean_stats = store.load(lean_in);
  assert(!lean_stats.used_accelerator);
  assert(lean_stats.keys == keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    assert(store.zcard(keys[i]) == cards[i]);
    assert(full_range(keys[i]) == ranges[i]);
  }
}

// The hash type: field->value via the shared swiss index over HashStorage.
void test_hash_basic() {
  goblin::core::Hash h;
  assert(h.set("name", "alice") == 1);
  assert(h.set("age", "30") == 1);
  assert(h.set("name", "bob") == 0);        // update an existing field
  assert(h.size() == 2);
  assert(h.get("name") == "bob");
  assert(h.get("age") == "30");
  assert(!h.get("missing").has_value());
  assert(h.contains("name") && !h.contains("missing"));
  assert(h.set("name", "al") == 0);         // shorter value -> in-place
  assert(h.get("name") == "al");
  assert(h.set("name", "alexander") == 0);  // longer value -> re-append
  assert(h.get("name") == "alexander");
  assert(h.erase("age"));                    // swap-remove
  assert(!h.contains("age") && h.size() == 1);
  assert(h.get("name") == "alexander");      // survivor intact after the swap
  int count = 0;
  h.for_each([&](std::string_view f, std::string_view v) {
    ++count;
    assert(f == "name" && v == "alexander");
  });
  assert(count == 1);
  h.compact();
  assert(h.size() == 1 && h.get("name") == "alexander");
  assert(h.memory_stats().field_count == 1);
}

// Snapshot round-trip via both the accelerator (fast) path and the canonical
// rebuild path.
void test_hash_snapshot_roundtrip() {
  goblin::core::Hash h;
  h.set("a", "1");
  h.set("b", "22");
  h.set("c", "333");
  assert(h.erase("b"));  // swap-remove leaves c in b's former id slot

  std::string buf;
  goblin::core::snapshot::Writer w(buf);
  h.save(w, /*with_accelerator=*/true);

  goblin::core::snapshot::Reader r(buf);
  auto fast = goblin::core::Hash::load(r, /*use_accelerator=*/true);
  assert(fast.size() == 2);
  assert(fast.get("a") == "1" && fast.get("c") == "333");
  assert(!fast.get("b").has_value());

  goblin::core::snapshot::Reader r2(buf);
  auto rebuilt = goblin::core::Hash::load(r2, /*use_accelerator=*/false);
  assert(rebuilt.size() == 2);
  assert(rebuilt.get("a") == "1" && rebuilt.get("c") == "333");
  assert(!rebuilt.get("b").has_value());
}

// The arena chunk size is configurable per type; smaller chunks still store and
// retrieve correctly (spanning several chunks), and an invalid size is rejected.
void test_configurable_chunk_size() {
  goblin::core::Hash h(
      goblin::core::HashOptions{.chunk_bytes = std::size_t{1} << 17});  // 128 KiB
  for (int i = 0; i < 5000; ++i) {
    h.set("field-" + std::to_string(i), "value-" + std::to_string(i));
  }
  assert(h.size() == 5000);
  assert(h.get("field-1234") == "value-1234");
  assert(h.get("field-4999") == "value-4999");

  const goblin::core::ZSetOptions zopts{.member_chunk_bytes = std::size_t{1} << 16};  // 64 KiB
  goblin::core::ZSet z(&zopts);
  for (int i = 0; i < 5000; ++i) {
    assert(z.add(static_cast<double>(i), "member-" + std::to_string(i),
                 &zopts) == 1);
  }
  assert(z.size() == 5000);
  assert(z.score("member-1234") == 1234.0);

  // Non-power-of-two / too-small sizes fall back to the default.
  goblin::core::HashStorage bad_hash(1000);
  assert(bad_hash.chunk_bytes() == goblin::core::HashStorage::kDefaultChunkBytes);
  goblin::core::ZSetMemberStorage bad_zset(false, std::size_t{1} << 10);
  assert(bad_zset.chunk_bytes() ==
         goblin::core::ZSetMemberStorage::kDefaultChunkBytes);
}

// ZREM orphans member bytes in the arena; once dead exceeds live past the floor,
// the same trigger the store runs after a ZREM batch reclaims it.
void test_zset_arena_auto_compacts_on_removal() {
  goblin::core::ZSet z;
  const std::string pad(100, 'x');  // ~105 bytes/member
  for (int i = 0; i < 20000; ++i) {
    assert(z.add(static_cast<double>(i), pad + std::to_string(i)) == 1);
  }
  const auto before = z.memory_stats().total_allocated_bytes;
  int removed = 0;
  for (int i = 0; i < 15000; ++i) {
    removed += z.remove(pad + std::to_string(i)) ? 1 : 0;
  }
  assert(removed == 15000);
  // ~1.5 MiB dead vs ~0.5 MiB live -> the store's post-batch trigger compacts.
  const bool compacted = z.compact_after_removal_if_needed(removed);
  assert(compacted);
  assert(z.memory_stats().total_allocated_bytes < before);
  assert(z.size() == 5000);
  assert(z.score(pad + std::to_string(19999)) == 19999.0);  // survivor intact
}

// The hash command surface over RESP (id-order iteration is deterministic).
void test_hash_commands() {
  goblin::core::Store store;
  assert(execute_fields(store, {"HSET", "h", "f1", "v1", "f2", "v2"}) == ":2\r\n");
  assert(execute_fields(store, {"HSET", "h", "f1", "V1"}) == ":0\r\n");  // update
  assert(execute_fields(store, {"HGET", "h", "f1"}) == "$2\r\nV1\r\n");
  assert(execute_fields(store, {"HGET", "h", "nope"}) == "$-1\r\n");
  assert(execute_fields(store, {"HLEN", "h"}) == ":2\r\n");
  assert(execute_fields(store, {"HEXISTS", "h", "f2"}) == ":1\r\n");
  assert(execute_fields(store, {"HEXISTS", "h", "nope"}) == ":0\r\n");
  assert(execute_fields(store, {"HSTRLEN", "h", "f2"}) == ":2\r\n");
  assert(execute_fields(store, {"HSETNX", "h", "f1", "x"}) == ":0\r\n");
  assert(execute_fields(store, {"HSETNX", "h", "f3", "v3"}) == ":1\r\n");
  assert(execute_fields(store, {"HGETALL", "h"}) ==
         "*6\r\n$2\r\nf1\r\n$2\r\nV1\r\n$2\r\nf2\r\n$2\r\nv2\r\n$2\r\nf3\r\n$2\r\nv3\r\n");
  assert(execute_fields(store, {"HKEYS", "h"}) ==
         "*3\r\n$2\r\nf1\r\n$2\r\nf2\r\n$2\r\nf3\r\n");
  assert(execute_fields(store, {"HVALS", "h"}) ==
         "*3\r\n$2\r\nV1\r\n$2\r\nv2\r\n$2\r\nv3\r\n");
  assert(execute_fields(store, {"HMGET", "h", "f1", "nope", "f3"}) ==
         "*3\r\n$2\r\nV1\r\n$-1\r\n$2\r\nv3\r\n");
  assert(execute_fields(store, {"HSET", "h", "n", "10"}) == ":1\r\n");
  assert(execute_fields(store, {"HINCRBY", "h", "n", "5"}) == ":15\r\n");
  assert(execute_fields(store, {"HINCRBY", "h", "fresh", "3"}) == ":3\r\n");
  assert(execute_fields(store, {"HINCRBY", "h", "f1", "1"}).find("ERR") !=
         std::string::npos);  // "V1" is not an integer
  assert(execute_fields(store, {"HDEL", "h", "f2", "nope"}) == ":1\r\n");
  assert(execute_fields(store, {"HEXISTS", "h", "f2"}) == ":0\r\n");
}

// A key holds one type; the cross-type command is a WRONGTYPE error.
void test_wrongtype() {
  goblin::core::Store store;
  assert(execute_fields(store, {"ZADD", "z", "1", "m"}) == ":1\r\n");
  assert(execute_fields(store, {"HSET", "z", "f", "v"}).find("WRONGTYPE") !=
         std::string::npos);
  assert(execute_fields(store, {"HGET", "z", "f"}).find("WRONGTYPE") !=
         std::string::npos);
  assert(execute_fields(store, {"HSET", "h", "f", "v"}) == ":1\r\n");
  assert(execute_fields(store, {"ZADD", "h", "1", "m"}).find("WRONGTYPE") !=
         std::string::npos);
  assert(execute_fields(store, {"ZSCORE", "h", "m"}).find("WRONGTYPE") !=
         std::string::npos);
  // GOBLIN.MEMORY and GOBLIN.OPTIMIZE resolve either type.
  assert(execute_fields(store, {"GOBLIN.MEMORY", "z"}).find("member_count") !=
         std::string::npos);
  assert(execute_fields(store, {"GOBLIN.MEMORY", "h"}).find("field_count") !=
         std::string::npos);
  assert(execute_fields(store, {"GOBLIN.OPTIMIZE", "h"}) != "$-1\r\n");
}

// A snapshot carries both the ZSET and HASH sections; each round-trips through
// the accelerator (fast) path and the canonical rebuild path.
void test_hash_snapshot_persistence() {
  goblin::core::Store store;
  assert(store.zadd("z", 1.0, "m1") == 1);
  assert(store.zadd("z", 2.0, "m2") == 1);
  assert(store.hset("h", "f1", "v1") == 1);
  assert(store.hset("h", "f2", "v2") == 1);
  assert(store.hset("h2", "x", "y") == 1);

  for (const bool with_accelerator : {true, false}) {
    std::stringstream buf;
    store.save(buf, with_accelerator);

    goblin::core::Store loaded;
    const auto stats = loaded.load(buf);
    assert(stats.keys == 3);  // z, h, h2
    assert(loaded.zscore("z", "m1") == 1.0);
    assert(loaded.zscore("z", "m2") == 2.0);
    assert(loaded.zcard("z") == 2);
    assert(loaded.hget("h", "f1") == "v1");
    assert(loaded.hget("h", "f2") == "v2");
    assert(loaded.hlen("h") == 2);
    assert(loaded.hget("h2", "x") == "y");
    assert(!loaded.hget("h", "missing").has_value());
  }
}

// Erasing from the score index rebalances underflowing blocks by merging with a
// neighbor (merge_with_next). Scattered scores make blocks vary in fullness, so
// a block that underflows (< kLoad/2) next to a near-full one merges into more
// than a single block's maximum (2*kLoad+1) before splitting back -- that append
// used to overflow. Bulk-add with scattered scores, then bulk-remove.
void test_score_index_merge_after_erase() {
  using goblin::core::Store;
  Store store;
  std::uint64_t rng = 88172645463325252ULL;
  auto next = [&] {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  const int n = 40000;
  std::vector<std::string> names;
  names.reserve(n);
  for (int i = 0; i < n; ++i) {
    char m[16];
    std::snprintf(m, sizeof(m), "m%06d", i);
    names.emplace_back(m);
    (void)store.zadd("s", static_cast<double>(next() % 60000),
                     std::string_view(names.back()));
  }
  const auto card0 = store.zcard("s");
  std::vector<std::string_view> to_remove;
  to_remove.reserve(30000);
  for (int i = 0; i < 30000; ++i) to_remove.push_back(names[i]);
  const auto removed = store.zrem("s", to_remove);
  assert(removed == 30000);
  assert(store.zcard("s") == card0 - removed);
  for (int i : {30000, 35000, 39999}) {
    assert(store.zscore("s", std::string_view(names[i])).has_value());
  }
}

// The member-bytes arena is built from 1 MiB chunks. Filling one exactly
// (8-byte members: 2^20 / 8 = 131072 of them) once left next_offset_ on a fresh
// chunk boundary whose chunk was not yet allocated, so the next append indexed
// an out-of-bounds chunk and crashed. Cross the boundary and verify the data.
void test_member_arena_chunk_boundary() {
  using goblin::core::Store;
  Store store;
  constexpr int kFill = (1 << 20) / 8;  // 8-byte members that fill one chunk
  const int n = kFill + 200;
  for (int i = 0; i < n; ++i) {
    char m[9];
    std::snprintf(m, sizeof(m), "%08d", i);
    assert(store.zadd("arena", static_cast<double>(i), std::string_view(m, 8)) == 1);
  }
  assert(store.zcard("arena") == n);
  for (int i : {0, kFill - 1, kFill, kFill + 1, n - 1}) {
    char m[9];
    std::snprintf(m, sizeof(m), "%08d", i);
    assert(store.zscore("arena", std::string_view(m, 8)) == static_cast<double>(i));
  }
}

// Background (fork/COW) save: the child writes the snapshot from a frozen copy
// of the store while the parent could keep serving; it must reload identically.
void test_background_save() {
  using goblin::core::Store;
  const std::string path = "/tmp/goblin_core_bgsave_test.gcsn";
  std::remove(path.c_str());

  Store store;
  for (int i = 0; i < 1000; ++i) {
    (void)store.zadd("bg", static_cast<double>(i % 50) - 10.0,
                     "m" + std::to_string(i));
  }
  (void)store.zadd("bg2", 3.5, "x");
  const auto expected = execute_fields(store, {"ZRANGE", "bg", "0", "-1", "WITHSCORES"});
  const auto card = store.zcard("bg");

  const auto started = store.start_background_save(path, /*with_accelerator=*/true);
  assert(started == Store::SaveStart::Started);
  assert(store.background_save_in_progress());

  std::optional<Store::SaveOutcome> outcome;
  for (int i = 0; i < 5000 && !outcome; ++i) {
    outcome = store.reap_background_save();
    if (!outcome) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(outcome.has_value());
  assert(outcome->ok);
  assert(outcome->path == path);
  assert(!store.background_save_in_progress());

  // The parent's data is unchanged, and the file (written from the fork) reloads
  // identically.
  assert(store.zcard("bg") == card);
  Store loaded;
  std::ifstream in(path, std::ios::binary);
  assert(in.good());
  const auto stats = loaded.load(in);
  assert(stats.keys == 2);
  assert(loaded.zcard("bg") == card);
  assert(execute_fields(loaded, {"ZRANGE", "bg", "0", "-1", "WITHSCORES"}) == expected);
  assert(loaded.zscore("bg2", "x") == 3.5);
  std::remove(path.c_str());
}

// Hand-crafted RDB v11 bytes (no redis dependency). Real-encoding and CRC64
// validation against a period-correct redis is a separate fixture step.
void test_rdb_import() {
  using goblin::core::Store;

  // CRC64 must match Redis's published vector (validates the checksum path
  // independently of any real dump).
  assert(goblin::core::rdb::crc64("123456789") == 0xe9c6d914c4b8d9caULL);
  auto u8 = [](std::string& s, unsigned b) { s.push_back(static_cast<char>(b)); };
  auto str = [&u8](std::string& s, std::string_view v) {  // 6-bit length (len < 64)
    u8(s, v.size());
    s.append(v);
  };
  auto dbl = [](std::string& s, double d) {
    const auto bits = std::bit_cast<std::uint64_t>(d);
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
  };
  auto crc0 = [&u8](std::string& s) { for (int i = 0; i < 8; ++i) u8(s, 0); };

  // A ZSET_2 (type 5) with negative and +inf scores; CRC disabled (all zero).
  {
    std::string rdb = "REDIS0011";
    u8(rdb, 0xFE); u8(rdb, 0x00);   // SELECTDB 0
    u8(rdb, 0x05);                  // ZSET_2
    str(rdb, "zk");
    u8(rdb, 0x03);                  // 3 members
    str(rdb, "a"); dbl(rdb, 1.5);
    str(rdb, "bb"); dbl(rdb, -2.0);
    str(rdb, "inf"); dbl(rdb, std::numeric_limits<double>::infinity());
    u8(rdb, 0xFF);
    crc0(rdb);

    Store store;
    std::istringstream in(rdb, std::ios::binary);
    const auto stats = store.load(in);
    assert(stats.keys == 1 && stats.members == 3);
    assert(store.zscore("zk", "a") == 1.5);
    assert(store.zscore("zk", "bb") == -2.0);
    assert(store.zscore("zk", "inf") == std::numeric_limits<double>::max());  // clamped
    assert(store.zcard("zk") == 3);
  }

  auto throws = [](const std::string& rdb) {
    Store store;
    (void)store.zadd("pre", 1.0, "x");
    std::istringstream in(rdb, std::ios::binary);
    try {
      (void)store.load(in);
    } catch (const goblin::core::rdb::rdb_error&) {
      return store.zcard("pre") == 0;  // left empty on failure
    }
    return false;
  };

  // Version newer than v11 is rejected.
  {
    std::string rdb = "REDIS0012";
    u8(rdb, 0xFF); crc0(rdb);
    assert(throws(rdb));
  }
  // A stream type aborts.
  {
    std::string rdb = "REDIS0011";
    u8(rdb, 0x0F);  // STREAM_LISTPACKS
    str(rdb, "s");
    assert(throws(rdb));
  }
  // A non-zero but wrong CRC is rejected.
  {
    std::string rdb = "REDIS0011";
    u8(rdb, 0x05); str(rdb, "z"); u8(rdb, 0x01); str(rdb, "m"); dbl(rdb, 1.0);
    u8(rdb, 0xFF);
    for (int i = 0; i < 8; ++i) u8(rdb, 0xAB);  // bogus checksum
    assert(throws(rdb));
  }
}

void test_keyspace_storage_and_string_value() {
  using goblin::core::KeyspaceStorage;
  using goblin::core::StringValue;

  static_assert(sizeof(StringValue) == 16);

  // A tiny value lives entirely inline; block/offset are unused.
  {
    StringValue v{};
    const std::string_view small = "42";
    v.length = static_cast<std::uint16_t>(small.size());
    assert(v.is_inline());
    assert(v.tail_length() == 0);
    std::memcpy(v.head_data(), small.data(), small.size());
    assert(v.head() == small);
  }
  // A 14-byte value is the largest fully-inline value.
  {
    StringValue v{};
    const std::string_view full = "0123456789abcd";  // 14 bytes
    v.length = static_cast<std::uint16_t>(full.size());
    assert(v.is_inline());
    std::memcpy(v.head_data(), full.data(), full.size());
    assert(v.head() == full);
  }
  // A larger value keeps a 6-byte prefix inline and addresses its tail.
  {
    StringValue v{};
    v.length = 40;
    assert(!v.is_inline());
    assert(v.tail_length() == 40 - StringValue::kPrefixCap);
    v.set_tail(7, 123);
    assert(v.tail_block() == 7);
    assert(v.tail_offset() == 123);
    std::memcpy(v.head_data(), "prefix", StringValue::kPrefixCap);
    assert(v.head() == std::string_view("prefix", StringValue::kPrefixCap));
  }

  // Key table: push/view, an empty key, value tails sharing the arena, and a
  // swap-remove sliding the last id into a hole.
  {
    KeyspaceStorage storage;
    const auto a = storage.push_back_key("alpha");
    const auto b = storage.push_back_key("bravo");
    const auto c = storage.push_back_key("");  // empty keys are valid
    assert(a == 0 && b == 1 && c == 2);
    assert(storage.size() == 3);
    assert(storage.view(a) == "alpha");
    assert(storage.view(b) == "bravo");
    assert(storage.view(c).empty());

    const auto loc = storage.append_tail("a-long-tail-of-bytes");
    assert(storage.tail_view(loc, 20) == "a-long-tail-of-bytes");

    storage.mark_key_dead(a);
    storage.move_key_slot(a, c);
    storage.pop_back_key();
    assert(storage.size() == 2);
    assert(storage.view(a).empty());  // c's (empty) bytes now sit at id a
    assert(storage.view(b) == "bravo");
    assert(storage.dead_bytes() == 5);  // "alpha"
  }

  // Cross a chunk boundary so the 32/32 {block, offset} split addresses more
  // than one block; every key must still resolve.
  {
    KeyspaceStorage storage(KeyspaceStorage::kMinChunkBytes);  // 128 KiB chunks
    std::vector<std::uint32_t> ids;
    std::vector<std::string> keys;
    const std::string filler(1024, 'x');
    for (int i = 0; i < 400; ++i) {  // ~400 KiB of keys spans several chunks
      std::string key = "k" + std::to_string(i) + ":" + filler;
      ids.push_back(storage.push_back_key(key));
      keys.push_back(std::move(key));
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
      assert(storage.view(ids[i]) == keys[i]);
    }
  }
}

}  // namespace

// --- Scripting (EVAL / EVALSHA / SCRIPT) -----------------------------------

std::string run_script(goblin::core::Store& store,
                       goblin::core::ScriptEngine& engine,
                       std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  std::string out;
  goblin::core::execute_command_into(
      store, *parsed.command, out,
      goblin::core::CommandExecutionOptions{.script_engine = &engine});
  return out;
}

void test_eval_type_conversions() {
  goblin::core::Store store;
  goblin::core::ScriptEngine engine(store);

  assert(run_script(store, engine, {"EVAL", "return 1", "0"}) == ":1\r\n");
  assert(run_script(store, engine, {"EVAL", "return 'hello'", "0"}) ==
         "$5\r\nhello\r\n");
  assert(run_script(store, engine, {"EVAL", "return 3.99", "0"}) == ":3\r\n");
  assert(run_script(store, engine, {"EVAL", "return true", "0"}) == ":1\r\n");
  assert(run_script(store, engine, {"EVAL", "return false", "0"}) == "$-1\r\n");
  assert(run_script(store, engine, {"EVAL", "return", "0"}) == "$-1\r\n");
  assert(run_script(store, engine, {"EVAL", "return {1,2,3}", "0"}) ==
         "*3\r\n:1\r\n:2\r\n:3\r\n");
  // An array stops at the first nil hole.
  assert(run_script(store, engine, {"EVAL", "return {1,2,nil,4}", "0"}) ==
         "*2\r\n:1\r\n:2\r\n");
  // KEYS / ARGV are 1-based and carry the split from numkeys.
  assert(run_script(store, engine,
                    {"EVAL", "return {KEYS[1], ARGV[1]}", "1", "k1", "a1"}) ==
         "*2\r\n$2\r\nk1\r\n$2\r\na1\r\n");
  // status_reply / error_reply shape the RESP framing.
  assert(run_script(store, engine,
                    {"EVAL", "return redis.status_reply('GOOD')", "0"}) ==
         "+GOOD\r\n");
  assert(run_script(store, engine,
                    {"EVAL", "return redis.error_reply('My Error')", "0"}) ==
         "-My Error\r\n");
  // sha1hex matches the published empty-string digest.
  assert(run_script(store, engine, {"EVAL", "return redis.sha1hex('')", "0"}) ==
         "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");
}

void test_eval_redis_call() {
  goblin::core::Store store;
  goblin::core::ScriptEngine engine(store);

  assert(run_script(store, engine, {"EVAL", "return redis.call('ping')", "0"}) ==
         "+PONG\r\n");
  // A write through redis.call mutates the real store...
  assert(run_script(store, engine,
                    {"EVAL", "return redis.call('zadd', KEYS[1], 1, 'a')", "1",
                     "z"}) == ":1\r\n");
  // ...and is visible both to a later script and to a plain command.
  assert(run_script(store, engine,
                    {"EVAL", "return redis.call('zscore', KEYS[1], 'a')", "1",
                     "z"}) == "$1\r\n1\r\n");
  assert(execute_fields(store, {"ZSCORE", "z", "a"}) == "$1\r\n1\r\n");
  // A multi-bulk reply becomes a Lua table and can round-trip back out.
  assert(run_script(store, engine,
                    {"EVAL", "return redis.call('zrange', KEYS[1], 0, -1)", "1",
                     "z"}) == "*1\r\n$1\r\na\r\n");
}

void test_eval_error_paths() {
  goblin::core::Store store;
  goblin::core::ScriptEngine engine(store);

  // An uncaught redis.call error propagates as a RESP error reply...
  const auto raised =
      run_script(store, engine, {"EVAL", "return redis.call('zscore')", "0"});
  assert(!raised.empty() && raised.front() == '-');
  // ...but redis.pcall hands the script the { err = ... } table instead.
  assert(run_script(store, engine,
                    {"EVAL",
                     "local r = redis.pcall('zscore'); if r.err then return "
                     "'caught' end",
                     "0"}) == "$6\r\ncaught\r\n");
  // Compile failures are reported, not crashed on.
  const auto compile = run_script(store, engine, {"EVAL", "this is not lua", "0"});
  assert(!compile.empty() && compile.front() == '-');
  // The sandbox forbids creating globals.
  const auto global = run_script(store, engine, {"EVAL", "x = 1", "0"});
  assert(!global.empty() && global.front() == '-' &&
         global.find("global") != std::string::npos);
  // A script cannot re-enter EVAL through redis.call.
  const auto nested = run_script(
      store, engine, {"EVAL", "return redis.call('eval', 'return 1', '0')", "0"});
  assert(!nested.empty() && nested.front() == '-');
  // Bad numkeys is a clean error, not a crash.
  const auto badkeys =
      run_script(store, engine, {"EVAL", "return 1", "-1"});
  assert(!badkeys.empty() && badkeys.front() == '-');
}

void test_script_load_and_evalsha() {
  goblin::core::Store store;
  goblin::core::ScriptEngine engine(store);

  const auto load = run_script(store, engine, {"SCRIPT", "LOAD", "return 42"});
  // "$40\r\n<40 hex>\r\n"
  assert(load.size() == 5 + 40 + 2);
  const std::string sha = load.substr(5, 40);

  assert(run_script(store, engine, {"EVALSHA", sha, "0"}) == ":42\r\n");
  assert(run_script(store, engine, {"SCRIPT", "EXISTS", sha}) == "*1\r\n:1\r\n");
  // An unknown digest is NOSCRIPT, not a crash.
  assert(run_script(store, engine,
                    {"EVALSHA", "ffffffffffffffffffffffffffffffffffffffff", "0"})
             .rfind("-NOSCRIPT", 0) == 0);

  // EVAL also populates the cache, so the same body is immediately EVALSHA-able.
  (void)run_script(store, engine, {"EVAL", "return 7", "0"});
  const auto digest =
      run_script(store, engine, {"EVAL", "return redis.sha1hex('return 7')", "0"});
  const std::string sha7 = digest.substr(5, 40);  // "$40\r\n<hex>\r\n"
  assert(run_script(store, engine, {"EVALSHA", sha7, "0"}) == ":7\r\n");

  // FLUSH clears the cache.
  assert(run_script(store, engine, {"SCRIPT", "FLUSH"}) == "+OK\r\n");
  assert(run_script(store, engine, {"SCRIPT", "EXISTS", sha}) == "*1\r\n:0\r\n");
  assert(run_script(store, engine, {"EVALSHA", sha, "0"}).rfind("-NOSCRIPT", 0) ==
         0);
}

void test_eval_helper_libraries() {
  goblin::core::Store store;
  goblin::core::ScriptEngine engine(store);

  assert(run_script(store, engine,
                    {"EVAL", "return cjson.encode({1,2,3})", "0"}) ==
         "$7\r\n[1,2,3]\r\n");
  assert(run_script(store, engine,
                    {"EVAL", "return cjson.decode('[10,20,30]')[2]", "0"}) ==
         ":20\r\n");
  assert(run_script(store, engine,
                    {"EVAL", "return cmsgpack.unpack(cmsgpack.pack('hi'))", "0"}) ==
         "$2\r\nhi\r\n");
  assert(run_script(store, engine, {"EVAL", "return bit.band(6,3)", "0"}) ==
         ":2\r\n");
  assert(run_script(store, engine,
                    {"EVAL",
                     "return struct.unpack('>I2', struct.pack('>I2', 258))",
                     "0"}) == ":258\r\n");
}

void test_eval_without_engine_is_unavailable() {
  // A caller that does not wire up a ScriptEngine gets a clean error, never a
  // crash -- execute_command's default options carry no engine.
  goblin::core::Store store;
  const auto reply = execute_fields(store, {"EVAL", "return 1", "0"});
  assert(!reply.empty() && reply.front() == '-');
}

// --- Luau scripting (LUAU.EVAL / LUAU.EVALSHA / LUAU.SCRIPT) -----------------

std::string run_luau(goblin::core::Store& store,
                     goblin::core::LuauEngine& engine,
                     std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  std::string out;
  goblin::core::execute_command_into(
      store, *parsed.command, out,
      goblin::core::CommandExecutionOptions{.luau_engine = &engine});
  return out;
}

void test_luau_type_conversions() {
  goblin::core::Store store;
  goblin::core::LuauEngine engine(store);

  assert(run_luau(store, engine, {"LUAU.EVAL", "return 1", "0"}) == ":1\r\n");
  assert(run_luau(store, engine, {"LUAU.EVAL", "return 'hi'", "0"}) == "$2\r\nhi\r\n");
  assert(run_luau(store, engine, {"LUAU.EVAL", "return 3.9", "0"}) == ":3\r\n");
  assert(run_luau(store, engine, {"LUAU.EVAL", "return true", "0"}) == ":1\r\n");
  assert(run_luau(store, engine, {"LUAU.EVAL", "return false", "0"}) == "$-1\r\n");
  assert(run_luau(store, engine, {"LUAU.EVAL", "return {1,2,3}", "0"}) ==
         "*3\r\n:1\r\n:2\r\n:3\r\n");
  assert(run_luau(store, engine,
                  {"LUAU.EVAL", "return {KEYS[1], ARGV[1]}", "1", "k1", "a1"}) ==
         "*2\r\n$2\r\nk1\r\n$2\r\na1\r\n");
  assert(run_luau(store, engine,
                  {"LUAU.EVAL", "return redis.status_reply('GOOD')", "0"}) ==
         "+GOOD\r\n");
  assert(run_luau(store, engine,
                  {"LUAU.EVAL", "return redis.error_reply('My Error')", "0"}) ==
         "-My Error\r\n");
  assert(run_luau(store, engine, {"LUAU.EVAL", "return redis.sha1hex('')", "0"}) ==
         "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");
}

void test_luau_redis_call() {
  goblin::core::Store store;
  goblin::core::LuauEngine engine(store);

  assert(run_luau(store, engine, {"LUAU.EVAL", "return redis.call('ping')", "0"}) ==
         "+PONG\r\n");
  assert(run_luau(store, engine,
                  {"LUAU.EVAL", "return redis.call('zadd', KEYS[1], 1, 'a')", "1",
                   "z"}) == ":1\r\n");
  // A Luau script's write is visible to a plain command (one shared store).
  assert(execute_fields(store, {"ZSCORE", "z", "a"}) == "$1\r\n1\r\n");
  // An uncaught redis.call error propagates; redis.pcall hands back the table.
  const auto raised =
      run_luau(store, engine, {"LUAU.EVAL", "return redis.call('zscore')", "0"});
  assert(!raised.empty() && raised.front() == '-');
  assert(run_luau(store, engine,
                  {"LUAU.EVAL",
                   "local r = redis.pcall('zscore') if r.err then return 'caught' end",
                   "0"}) == "$6\r\ncaught\r\n");
  // A Luau script cannot re-enter either interpreter.
  const auto nested =
      run_luau(store, engine,
               {"LUAU.EVAL", "return redis.call('luau.eval', 'return 1', '0')", "0"});
  assert(!nested.empty() && nested.front() == '-');
}

void test_luau_script_load_and_evalsha() {
  goblin::core::Store store;
  goblin::core::LuauEngine engine(store);

  const auto load = run_luau(store, engine, {"LUAU.SCRIPT", "LOAD", "return 42"});
  assert(load.size() == 5 + 40 + 2);
  const std::string sha = load.substr(5, 40);
  assert(run_luau(store, engine, {"LUAU.EVALSHA", sha, "0"}) == ":42\r\n");
  assert(run_luau(store, engine, {"LUAU.SCRIPT", "EXISTS", sha}) == "*1\r\n:1\r\n");
  assert(run_luau(store, engine,
                  {"LUAU.EVALSHA", "ffffffffffffffffffffffffffffffffffffffff", "0"})
             .rfind("-NOSCRIPT", 0) == 0);
  assert(run_luau(store, engine, {"LUAU.SCRIPT", "FLUSH"}) == "+OK\r\n");
  assert(run_luau(store, engine, {"LUAU.SCRIPT", "EXISTS", sha}) == "*1\r\n:0\r\n");
}

void test_luau_is_a_distinct_interpreter() {
  // The two engines share the store but nothing else -- different language
  // dialect, different standard library, and independent script caches.
  goblin::core::Store store;
  goblin::core::ScriptEngine puc(store);
  goblin::core::LuauEngine luau(store);

  // Luau type annotations parse under Luau but are a syntax error under PUC 5.1.
  assert(run_luau(store, luau, {"LUAU.EVAL", "local x: number = 7 return x", "0"}) ==
         ":7\r\n");
  const auto puc_typed =
      run_script(store, puc, {"EVAL", "local x: number = 7 return x", "0"});
  assert(!puc_typed.empty() && puc_typed.front() == '-');

  // Luau ships bit32; the PUC engine ships bit. Each is absent in the other.
  assert(run_luau(store, luau, {"LUAU.EVAL", "return bit32.band(6,3)", "0"}) ==
         ":2\r\n");
  assert(run_script(store, puc, {"EVAL", "return bit.band(6,3)", "0"}) == ":2\r\n");
  assert(run_luau(store, luau, {"LUAU.EVAL", "return bit.band(6,3)", "0"}).front() ==
         '-');
  assert(run_script(store, puc, {"EVAL", "return bit32.band(6,3)", "0"}).front() ==
         '-');

  // Caches are independent: a script loaded into one is unknown to the other.
  const auto load = run_luau(store, luau, {"LUAU.SCRIPT", "LOAD", "return 5"});
  const std::string sha = load.substr(5, 40);
  assert(run_luau(store, luau, {"LUAU.EVALSHA", sha, "0"}) == ":5\r\n");
  assert(run_script(store, puc, {"EVALSHA", sha, "0"}).rfind("-NOSCRIPT", 0) == 0);
}

void test_luau_without_engine_is_unavailable() {
  goblin::core::Store store;
  const auto reply = execute_fields(store, {"LUAU.EVAL", "return 1", "0"});
  assert(!reply.empty() && reply.front() == '-');
}

// --- Wren scripting (WREN.EVAL / WREN.EVALSHA / WREN.SCRIPT) -----------------

std::string run_wren(goblin::core::Store& store,
                     goblin::core::WrenEngine& engine,
                     std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  std::string out;
  goblin::core::execute_command_into(
      store, *parsed.command, out,
      goblin::core::CommandExecutionOptions{.wren_engine = &engine});
  return out;
}

void test_wren_type_conversions() {
  goblin::core::Store store;
  goblin::core::WrenEngine engine(store);

  assert(run_wren(store, engine, {"WREN.EVAL", "return 1", "0"}) == ":1\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "return \"hi\"", "0"}) == "$2\r\nhi\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "return 3.9", "0"}) == ":3\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "var x = 5", "0"}) == "$-1\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "return true", "0"}) == ":1\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "return false", "0"}) == "$-1\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "return [1,2,3]", "0"}) ==
         "*3\r\n:1\r\n:2\r\n:3\r\n");
  // KEYS/ARGV are 0-based Lists (Wren indexing).
  assert(run_wren(store, engine,
                  {"WREN.EVAL", "return [KEYS[0], ARGV[0]]", "1", "k1", "a1"}) ==
         "*2\r\n$2\r\nk1\r\n$2\r\na1\r\n");
  assert(run_wren(store, engine,
                  {"WREN.EVAL", "return Redis.status(\"GOOD\")", "0"}) == "+GOOD\r\n");
  assert(run_wren(store, engine,
                  {"WREN.EVAL", "return Redis.error(\"My Error\")", "0"}) ==
         "-My Error\r\n");
  assert(run_wren(store, engine, {"WREN.EVAL", "return Redis.sha1hex(\"\")", "0"}) ==
         "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");
}

void test_wren_redis_call() {
  goblin::core::Store store;
  goblin::core::WrenEngine engine(store);

  assert(run_wren(store, engine, {"WREN.EVAL", "return Redis.call([\"ping\"])", "0"}) ==
         "+PONG\r\n");
  assert(run_wren(store, engine,
                  {"WREN.EVAL", "return Redis.call([\"zadd\", KEYS[0], 1, \"a\"])",
                   "1", "z"}) == ":1\r\n");
  assert(execute_fields(store, {"ZSCORE", "z", "a"}) == "$1\r\n1\r\n");
  const auto raised =
      run_wren(store, engine, {"WREN.EVAL", "return Redis.call([\"zscore\"])", "0"});
  assert(!raised.empty() && raised.front() == '-');
  assert(run_wren(store, engine,
                  {"WREN.EVAL",
                   "var r = Redis.pcall([\"zscore\"])\nif (r[\"err\"] != null) return "
                   "\"caught\"",
                   "0"}) == "$6\r\ncaught\r\n");
  const auto nested = run_wren(
      store, engine,
      {"WREN.EVAL", "return Redis.call([\"wren.eval\", \"return 1\", \"0\"])", "0"});
  assert(!nested.empty() && nested.front() == '-');
}

void test_wren_script_load_and_evalsha() {
  goblin::core::Store store;
  goblin::core::WrenEngine engine(store);

  const auto load = run_wren(store, engine, {"WREN.SCRIPT", "LOAD", "return 42"});
  assert(load.size() == 5 + 40 + 2);
  const std::string sha = load.substr(5, 40);
  assert(run_wren(store, engine, {"WREN.EVALSHA", sha, "0"}) == ":42\r\n");
  assert(run_wren(store, engine, {"WREN.SCRIPT", "EXISTS", sha}) == "*1\r\n:1\r\n");
  assert(run_wren(store, engine,
                  {"WREN.EVALSHA", "ffffffffffffffffffffffffffffffffffffffff", "0"})
             .rfind("-NOSCRIPT", 0) == 0);
  assert(run_wren(store, engine, {"WREN.SCRIPT", "FLUSH"}) == "+OK\r\n");
  assert(run_wren(store, engine, {"WREN.SCRIPT", "EXISTS", sha}) == "*1\r\n:0\r\n");
}

void test_wren_is_a_distinct_interpreter() {
  // Wren shares the store but is neither PUC-Lua nor Luau: a Wren list-filter
  // chain runs under WREN.EVAL but is a syntax error under either Lua engine, and
  // the caches are independent.
  goblin::core::Store store;
  goblin::core::ScriptEngine puc(store);
  goblin::core::WrenEngine wren(store);

  assert(run_wren(store, wren,
                  {"WREN.EVAL", "return [1,2,3,4].where{|x| x % 2 == 0}.toList", "0"}) ==
         "*2\r\n:2\r\n:4\r\n");
  const auto puc_wren =
      run_script(store, puc, {"EVAL", "return [1,2,3,4].where{|x| x % 2 == 0}.toList", "0"});
  assert(!puc_wren.empty() && puc_wren.front() == '-');

  const auto load = run_wren(store, wren, {"WREN.SCRIPT", "LOAD", "return 5"});
  const std::string sha = load.substr(5, 40);
  assert(run_wren(store, wren, {"WREN.EVALSHA", sha, "0"}) == ":5\r\n");
  assert(run_script(store, puc, {"EVALSHA", sha, "0"}).rfind("-NOSCRIPT", 0) == 0);
}

void test_wren_without_engine_is_unavailable() {
  goblin::core::Store store;
  const auto reply = execute_fields(store, {"WREN.EVAL", "return 1", "0"});
  assert(!reply.empty() && reply.front() == '-');
}

// --- Tcl scripting (TCL.EVAL / TCL.EVALSHA / TCL.SCRIPT) ---------------------

std::string run_tcl(goblin::core::Store& store,
                    goblin::core::TclEngine& engine,
                    std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  std::string out;
  goblin::core::execute_command_into(
      store, *parsed.command, out,
      goblin::core::CommandExecutionOptions{.tcl_engine = &engine});
  return out;
}

void test_tcl_type_conversions() {
  goblin::core::Store store;
  goblin::core::TclEngine engine(store);

  // A canonical integer replies as an integer; anything else as a bulk string.
  assert(run_tcl(store, engine, {"TCL.EVAL", "expr {1 + 2}", "0"}) == ":3\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "return hello", "0"}) == "$5\r\nhello\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "return 5.5", "0"}) == "$3\r\n5.5\r\n");
  // KEYS / ARGV are Tcl lists (indexed with lindex, 0-based).
  assert(run_tcl(store, engine, {"TCL.EVAL", "lindex $KEYS 0", "1", "k1"}) ==
         "$2\r\nk1\r\n");
  assert(run_tcl(store, engine,
                 {"TCL.EVAL", "set s 0; foreach n $ARGV { incr s $n }; return $s", "0",
                  "10", "20", "12"}) == ":42\r\n");
  // The redis reply builders.
  assert(run_tcl(store, engine, {"TCL.EVAL", "redis status GOOD", "0"}) == "+GOOD\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "redis error {My Error}", "0"}) ==
         "-My Error\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "redis integer 42", "0"}) == ":42\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "redis nil", "0"}) == "$-1\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "redis array {a b c}", "0"}) ==
         "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
  assert(run_tcl(store, engine, {"TCL.EVAL", "redis sha1hex {}", "0"}) ==
         "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");
}

void test_tcl_redis_call() {
  goblin::core::Store store;
  goblin::core::TclEngine engine(store);

  assert(run_tcl(store, engine,
                 {"TCL.EVAL", "redis call zadd [lindex $KEYS 0] 1 a", "1", "z"}) ==
         ":1\r\n");
  assert(execute_fields(store, {"ZSCORE", "z", "a"}) == "$1\r\n1\r\n");
  // An uncaught command error becomes a RESP error; catch recovers it.
  const auto raised =
      run_tcl(store, engine, {"TCL.EVAL", "redis call zscore", "0"});
  assert(!raised.empty() && raised.front() == '-');
  assert(run_tcl(store, engine,
                 {"TCL.EVAL", "if {[catch {redis call zscore} e]} {return caught}",
                  "0"}) == "$6\r\ncaught\r\n");
  // A script cannot re-enter any interpreter.
  const auto nested = run_tcl(
      store, engine, {"TCL.EVAL", "redis call tcl.eval {return 1} 0", "0"});
  assert(!nested.empty() && nested.front() == '-');
  // The sandbox removes process/host commands.
  assert(run_tcl(store, engine, {"TCL.EVAL", "exit", "0"}).front() == '-');
  assert(run_tcl(store, engine, {"TCL.EVAL", "source /etc/passwd", "0"}).front() == '-');
}

void test_tcl_script_load_and_evalsha() {
  goblin::core::Store store;
  goblin::core::TclEngine engine(store);

  const auto load = run_tcl(store, engine, {"TCL.SCRIPT", "LOAD", "return 42"});
  assert(load.size() == 5 + 40 + 2);
  const std::string sha = load.substr(5, 40);
  assert(run_tcl(store, engine, {"TCL.EVALSHA", sha, "0"}) == ":42\r\n");
  assert(run_tcl(store, engine, {"TCL.SCRIPT", "EXISTS", sha}) == "*1\r\n:1\r\n");
  assert(run_tcl(store, engine,
                 {"TCL.EVALSHA", "ffffffffffffffffffffffffffffffffffffffff", "0"})
             .rfind("-NOSCRIPT", 0) == 0);
  assert(run_tcl(store, engine, {"TCL.SCRIPT", "FLUSH"}) == "+OK\r\n");
  assert(run_tcl(store, engine, {"TCL.SCRIPT", "EXISTS", sha}) == "*1\r\n:0\r\n");
  // LOAD rejects an unbalanced script (via `info complete`).
  assert(run_tcl(store, engine, {"TCL.SCRIPT", "LOAD", "set x {"}).front() == '-');
}

void test_tcl_is_a_distinct_interpreter() {
  goblin::core::Store store;
  goblin::core::ScriptEngine puc(store);
  goblin::core::TclEngine tcl(store);

  // A Tcl one-liner runs under TCL.EVAL but is a syntax error under PUC-Lua.
  assert(run_tcl(store, tcl, {"TCL.EVAL", "return [string toupper hello]", "0"}) ==
         "$5\r\nHELLO\r\n");
  const auto puc_tcl =
      run_script(store, puc, {"EVAL", "return [string toupper hello]", "0"});
  assert(!puc_tcl.empty() && puc_tcl.front() == '-');

  // Independent caches.
  const auto load = run_tcl(store, tcl, {"TCL.SCRIPT", "LOAD", "return 5"});
  const std::string sha = load.substr(5, 40);
  assert(run_tcl(store, tcl, {"TCL.EVALSHA", sha, "0"}) == ":5\r\n");
  assert(run_script(store, puc, {"EVALSHA", sha, "0"}).rfind("-NOSCRIPT", 0) == 0);
}

void test_tcl_without_engine_is_unavailable() {
  goblin::core::Store store;
  const auto reply = execute_fields(store, {"TCL.EVAL", "return 1", "0"});
  assert(!reply.empty() && reply.front() == '-');
}

// --- MicroPython scripting (UPYTHON.EVAL / UPYTHON.EVALSHA / UPYTHON.SCRIPT) --

std::string run_upython(goblin::core::Store& store,
                        goblin::core::UPythonEngine& engine,
                        std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  std::string out;
  goblin::core::execute_command_into(
      store, *parsed.command, out,
      goblin::core::CommandExecutionOptions{.upython_engine = &engine});
  return out;
}

// MicroPython keeps a single global VM, so all Python assertions share one engine
// (and one Store) rather than constructing/destructing the VM per case.
void test_upython_scripting() {
  goblin::core::Store store;
  goblin::core::UPythonEngine engine(store);
  auto& e = engine;

  // A script produces its reply by assigning the `reply` global.
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = 1 + 2", "0"}) == ":3\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = 'hello'", "0"}) ==
         "$5\r\nhello\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "x = 5", "0"}) == "$-1\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = True", "0"}) == ":1\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = False", "0"}) == "$-1\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = [1, 2, 3]", "0"}) ==
         "*3\r\n:1\r\n:2\r\n:3\r\n");
  assert(run_upython(store, e,
                     {"UPYTHON.EVAL", "reply = [x * x for x in range(4)]", "0"}) ==
         "*4\r\n:0\r\n:1\r\n:4\r\n:9\r\n");
  // KEYS / ARGV are 0-based lists.
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = KEYS[0]", "1", "k1"}) ==
         "$2\r\nk1\r\n");
  assert(run_upython(store, e,
                     {"UPYTHON.EVAL", "reply = sum(int(x) for x in ARGV)", "0", "10",
                      "20", "12"}) == ":42\r\n");
  // The redis helpers.
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = redis.status('GOOD')", "0"}) ==
         "+GOOD\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = redis.error('My Error')", "0"}) ==
         "-My Error\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = redis.sha1hex('')", "0"}) ==
         "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");

  // redis.call, and the write is visible to a plain command.
  assert(run_upython(store, e,
                     {"UPYTHON.EVAL", "reply = redis.call('zadd', KEYS[0], 1, 'a')", "1",
                      "z"}) == ":1\r\n");
  assert(execute_fields(store, {"ZSCORE", "z", "a"}) == "$1\r\n1\r\n");
  // try/except recovers a command error; an uncaught one is a RESP error.
  assert(run_upython(store, e,
                     {"UPYTHON.EVAL",
                      "try:\n redis.call('zscore')\nexcept Exception:\n reply = 'caught'",
                      "0"}) == "$6\r\ncaught\r\n");
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = redis.call('zscore')", "0"})
             .front() == '-');
  assert(run_upython(store, e, {"UPYTHON.EVAL", "reply = 1 // 0", "0"}).front() == '-');
  assert(run_upython(store, e,
                     {"UPYTHON.EVAL", "reply = redis.call('upython.eval', 'reply=1', '0')",
                      "0"})
             .front() == '-');

  // SCRIPT LOAD / EVALSHA / EXISTS / FLUSH.
  const auto load = run_upython(store, e, {"UPYTHON.SCRIPT", "LOAD", "reply = 42"});
  assert(load.size() == 5 + 40 + 2);
  const std::string sha = load.substr(5, 40);
  assert(run_upython(store, e, {"UPYTHON.EVALSHA", sha, "0"}) == ":42\r\n");
  assert(run_upython(store, e, {"UPYTHON.SCRIPT", "EXISTS", sha}) == "*1\r\n:1\r\n");
  assert(run_upython(store, e,
                     {"UPYTHON.EVALSHA", "ffffffffffffffffffffffffffffffffffffffff", "0"})
             .rfind("-NOSCRIPT", 0) == 0);
  assert(run_upython(store, e, {"UPYTHON.SCRIPT", "FLUSH"}) == "+OK\r\n");
  assert(run_upython(store, e, {"UPYTHON.SCRIPT", "EXISTS", sha}) == "*1\r\n:0\r\n");
  // LOAD rejects a syntax error without running.
  assert(run_upython(store, e, {"UPYTHON.SCRIPT", "LOAD", "def f("}).front() == '-');

  // Distinct interpreter: Python syntax is a syntax error under PUC-Lua.
  goblin::core::ScriptEngine puc(store);
  assert(run_script(store, puc, {"EVAL", "reply = [x for x in range(3)]", "0"}).front() ==
         '-');
}

void test_upython_without_engine_is_unavailable() {
  goblin::core::Store store;
  const auto reply = execute_fields(store, {"UPYTHON.EVAL", "reply = 1", "0"});
  assert(!reply.empty() && reply.front() == '-');
}

// --- QuickJS scripting (QUICKJS.EVAL / QUICKJS.EVALSHA / QUICKJS.SCRIPT) ------

std::string run_quickjs(goblin::core::Store& store,
                        goblin::core::QuickJsEngine& engine,
                        std::initializer_list<std::string_view> fields) {
  std::vector<std::string_view> views(fields);
  auto parsed = goblin::core::parse_command(views);
  assert(parsed.ok());
  std::string out;
  goblin::core::execute_command_into(
      store, *parsed.command, out,
      goblin::core::CommandExecutionOptions{.quickjs_engine = &engine});
  return out;
}

// QuickJS keeps one runtime/context, so all JavaScript assertions share a single
// engine (and one Store); a script body runs inside a function, so `return`
// produces the reply and declarations stay script-local.
void test_quickjs_scripting() {
  goblin::core::Store store;
  goblin::core::QuickJsEngine engine(store);
  auto& e = engine;

  // A script produces its reply with `return`.
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return 1 + 2", "0"}) == ":3\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return 'hello'", "0"}) ==
         "$5\r\nhello\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "var x = 5;", "0"}) == "$-1\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return true", "0"}) == ":1\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return false", "0"}) == "$-1\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return 3.5", "0"}) == "$3\r\n3.5\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return [1, 2, 3]", "0"}) ==
         "*3\r\n:1\r\n:2\r\n:3\r\n");
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVAL", "return [0, 1, 2, 3].map(x => x * x)", "0"}) ==
         "*4\r\n:0\r\n:1\r\n:4\r\n:9\r\n");
  // Declarations stay script-local (the body runs inside a function).
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVAL",
                      "var leaked = 7; return typeof globalThis.leaked", "0"}) ==
         "$9\r\nundefined\r\n");
  // KEYS / ARGV are 0-based arrays.
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return KEYS[0]", "1", "k1"}) ==
         "$2\r\nk1\r\n");
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVAL", "return ARGV.reduce((a, b) => a + parseInt(b), 0)",
                      "0", "10", "20", "12"}) == ":42\r\n");
  // The redis helpers.
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return redis.status('GOOD')", "0"}) ==
         "+GOOD\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return redis.error('My Error')", "0"}) ==
         "-My Error\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return redis.sha1hex('')", "0"}) ==
         "$40\r\nda39a3ee5e6b4b0d3255bfef95601890afd80709\r\n");

  // redis.call, and the write is visible to a plain command.
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVAL", "return redis.call('zadd', KEYS[0], 1, 'a')", "1",
                      "z"}) == ":1\r\n");
  assert(execute_fields(store, {"ZSCORE", "z", "a"}) == "$1\r\n1\r\n");
  // try/catch recovers a command error; an uncaught one is a RESP error.
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVAL",
                      "try { redis.call('zscore') } catch (e) { return 'caught' }",
                      "0"}) == "$6\r\ncaught\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return redis.call('zscore')", "0"})
             .front() == '-');
  assert(run_quickjs(store, e, {"QUICKJS.EVAL", "return undefinedThing()", "0"})
             .front() == '-');
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVAL",
                      "return redis.call('quickjs.eval', 'return 1', '0')", "0"})
             .front() == '-');

  // SCRIPT LOAD / EVALSHA / EXISTS / FLUSH.
  const auto load = run_quickjs(store, e, {"QUICKJS.SCRIPT", "LOAD", "return 42"});
  assert(load.size() == 5 + 40 + 2);
  const std::string sha = load.substr(5, 40);
  assert(run_quickjs(store, e, {"QUICKJS.EVALSHA", sha, "0"}) == ":42\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.SCRIPT", "EXISTS", sha}) == "*1\r\n:1\r\n");
  assert(run_quickjs(store, e,
                     {"QUICKJS.EVALSHA", "ffffffffffffffffffffffffffffffffffffffff", "0"})
             .rfind("-NOSCRIPT", 0) == 0);
  assert(run_quickjs(store, e, {"QUICKJS.SCRIPT", "FLUSH"}) == "+OK\r\n");
  assert(run_quickjs(store, e, {"QUICKJS.SCRIPT", "EXISTS", sha}) == "*1\r\n:0\r\n");
  // LOAD rejects a syntax error without running.
  assert(run_quickjs(store, e, {"QUICKJS.SCRIPT", "LOAD", "return 1 +"}).front() == '-');

  // Distinct interpreter: JavaScript is a syntax error under PUC-Lua.
  goblin::core::ScriptEngine puc(store);
  assert(run_script(store, puc, {"EVAL", "return [0, 1, 2].map(x => x * x)", "0"})
             .front() == '-');
}

void test_quickjs_without_engine_is_unavailable() {
  goblin::core::Store store;
  const auto reply = execute_fields(store, {"QUICKJS.EVAL", "return 1", "0"});
  assert(!reply.empty() && reply.front() == '-');
}

// The compare-and-delete idiom (the Redlock unlock script) written in every
// embedded language, each asserted to be a faithful drop-in for the native
// GOBLIN.CAD: a matching token deletes the key and replies 1, a re-run replies
// 0, and a mismatched token leaves the key in place and replies 0. Each engine
// lives in its own scope so only one VM is alive at a time (MicroPython balances
// mp_init/mp_deinit per instance).
void test_compare_and_delete_idiom() {
  using goblin::core::Store;

  // Run one engine's idiom through the three cases against a shared store.
  auto check = [](auto run, Store& store, auto& engine, std::string_view cmd,
                  std::string_view src) {
    store.set("lk", "secret");
    assert(run(store, engine, {cmd, src, "1", "lk", "secret"}) == ":1\r\n");  // match -> del
    assert(run(store, engine, {cmd, src, "1", "lk", "secret"}) == ":0\r\n");  // gone
    store.set("lk", "secret");
    assert(run(store, engine, {cmd, src, "1", "lk", "nope"}) == ":0\r\n");    // mismatch
    assert(store.get("lk").has_value());  // ... left the key untouched
    (void)store.del("lk");
  };

  Store store;
  {  // PUC-Lua and Luau: 1-based KEYS/ARGV, varargs redis.call, top-level return.
    goblin::core::ScriptEngine e(store);
    check(run_script, store, e, "EVAL",
R"(if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("del", KEYS[1])
end
return 0)");
  }
  {
    goblin::core::LuauEngine e(store);
    check(run_luau, store, e, "LUAU.EVAL",
R"(if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("del", KEYS[1])
end
return 0)");
  }
  {  // Wren: 0-based Lists, Redis.call takes a List, return inside the wrapper.
    goblin::core::WrenEngine e(store);
    check(run_wren, store, e, "WREN.EVAL",
R"(if (Redis.call(["get", KEYS[0]]) == ARGV[0]) {
  return Redis.call(["del", KEYS[0]])
}
return 0)");
  }
  {  // Tcl: lists read with lindex (0-based), string compare with eq.
    goblin::core::TclEngine e(store);
    check(run_tcl, store, e, "TCL.EVAL",
R"(if {[redis call get [lindex $KEYS 0]] eq [lindex $ARGV 0]} {
  return [redis call del [lindex $KEYS 0]]
}
return 0)");
  }
  {  // MicroPython: 0-based lists, the reply comes from the `reply` global.
    goblin::core::UPythonEngine e(store);
    check(run_upython, store, e, "UPYTHON.EVAL",
R"(if redis.call("get", KEYS[0]) == ARGV[0]:
    reply = redis.call("del", KEYS[0])
else:
    reply = 0)");
  }
  {  // JavaScript: 0-based arrays, strict ===, return inside the wrapper.
    goblin::core::QuickJsEngine e(store);
    check(run_quickjs, store, e, "QUICKJS.EVAL",
R"(if (redis.call("get", KEYS[0]) === ARGV[0]) {
  return redis.call("del", KEYS[0])
}
return 0)");
  }

  // The native GOBLIN.CAD is the same drop-in on the same store.
  store.set("lk", "secret");
  assert(execute_fields(store, {"GOBLIN.CAD", "lk", "nope"}) == ":0\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "lk", "secret"}) == ":1\r\n");
  assert(execute_fields(store, {"GOBLIN.CAD", "lk", "secret"}) == ":0\r\n");
}

// SCRIPT LOAD / EVALSHA / SCRIPT FLUSH on every engine, showing the cached
// (precompiled) script runs by digest, is repeatable, and reports NOSCRIPT for an
// unknown or flushed digest. This exercises each engine's precompile path: LOAD
// compiles once, EVALSHA executes the cached artifact with no recompilation.
void test_evalsha_runs_cached_script() {
  using goblin::core::Store;
  const std::string_view kMissing = "ffffffffffffffffffffffffffffffffffffffff";

  auto check = [kMissing](auto run, Store& store, auto& engine,
                          const std::string& prefix, std::string_view script) {
    const std::string script_cmd = prefix + "SCRIPT";
    const std::string evalsha_cmd = prefix + "EVALSHA";

    // SCRIPT LOAD returns the 40-hex digest.
    const std::string load = run(store, engine, {script_cmd, "LOAD", script});
    assert(load.size() == 5 + 40 + 2 && load.compare(0, 5, "$40\r\n") == 0);
    const std::string sha = load.substr(5, 40);

    // EVALSHA runs the cached compiled script -- and is repeatable (the cached
    // artifact is reused, not consumed).
    assert(run(store, engine, {evalsha_cmd, sha, "0"}) == ":42\r\n");
    assert(run(store, engine, {evalsha_cmd, sha, "0"}) == ":42\r\n");

    // An unknown digest is NOSCRIPT.
    assert(run(store, engine, {evalsha_cmd, kMissing, "0"}).rfind("-NOSCRIPT", 0) == 0);

    // After FLUSH the digest is gone.
    assert(run(store, engine, {script_cmd, "FLUSH"}) == "+OK\r\n");
    assert(run(store, engine, {evalsha_cmd, sha, "0"}).rfind("-NOSCRIPT", 0) == 0);
  };

  Store store;
  { goblin::core::ScriptEngine e(store);  check(run_script, store, e, "", "return 42"); }
  { goblin::core::LuauEngine e(store);    check(run_luau, store, e, "LUAU.", "return 42"); }
  { goblin::core::WrenEngine e(store);    check(run_wren, store, e, "WREN.", "return 42"); }
  { goblin::core::TclEngine e(store);     check(run_tcl, store, e, "TCL.", "return 42"); }
  { goblin::core::UPythonEngine e(store); check(run_upython, store, e, "UPYTHON.", "reply = 42"); }
  { goblin::core::QuickJsEngine e(store); check(run_quickjs, store, e, "QUICKJS.", "return 42"); }
}

int main() {
  test_swiss_table_string_view_lookup();
  test_swiss_table_insert_find_update();
  test_swiss_table_collision_probe_and_growth();
  test_swiss_table_erase_reuses_tombstones();
  test_chunked_sorted_list_splits_and_ranges();
  test_chunked_sorted_list_erase_rebalances();
  test_zset_uses_chunked_ordering();
  test_zset_score_rank_and_remove();
  test_zset_equal_score_lex_order_and_id_reuse();
  test_zset_compact_rebuilds_indexes_and_storage();
  test_zset_auto_compacts_after_large_removal_batch();
  test_zset_dense_delete_moves_last_member();
  test_zset_dense_delete_same_score_multi_block();
  test_zset_member_index_cleanup_removes_tombstones();
  test_zset_member_index_cleanup_rehashes_many_tombstones();
  test_zset_member_index_cleanup_shrinks_sparse_table();
  test_zset_skips_auto_compaction_for_small_removals();
  test_store_zset_methods();
  test_store_multiple_zsets();
  test_store_shared_member_layer();
  test_store_shared_layer_structural_append_cow();
  test_zset_small_collection_footprint();
  test_zset_arena_growth_spans_blocks();
  test_store_shared_member_layer_skips_unrelated_keys();
  test_store_many_zsets_coexist();
  test_store_strings();
  test_store_string_arena_compaction();
  test_string_commands();
  test_goblin_cad();
  test_goblin_caexpire();
  test_goblin_cas();
  test_goblin_td_leaderboard_rescore();
  test_memory_report();
  test_goblin_increx();
  test_zremrangebyscore();
  test_goblin_zwindow();
  test_string_range_commands();
  test_string_snapshot_roundtrip();
  test_ttl_set();
  test_ttl_commands();
  test_ttl_snapshot_roundtrip();
  test_ttl_load_drops_expired();
  test_expire_and_set_options();
  test_store_rank_location_cache();
  test_block_hint_rank_cache_uses_narrow_storage();
  test_zset_score_width_promotes();
  test_zset_optimize_demotes_score_width();
  test_zset_score_index_width_ordering();
  test_zset_arena_id_stable_reclaim();
  test_compact_listpack();
  test_key_arena();
  test_zset_listpack_mode();
  test_block_hint_rank_cache_lazy_offset_repair();
  test_block_hint_rank_cache_promotes_to_wide_storage();
  test_resp_parser_incremental();
  test_inline_parser();
  test_protocol_error();
  test_resp_parser_pipeline_and_pop_into();
  test_command_perfect_hash();
  test_resp_bulk_writer_small_header_table();
  test_resp_array_writer_small_header_table();
  test_resp_bulk_wire_size_matches_output();
  test_zrange_withscores_batch_matches_streaming_append();
  test_zrange_withscores_fused_matches_legacy_append();
  test_command_dispatch();
  test_echo_info();
  test_goblin_optimize_reclaims_slack();
  test_goblin_optimize_density_and_growth();
  test_snapshot_round_trip();
  test_snapshot_score_width_shrinks();
  test_snapshot_save_clear_reload();
  test_hash_basic();
  test_hash_snapshot_roundtrip();
  test_configurable_chunk_size();
  test_zset_arena_auto_compacts_on_removal();
  test_hash_commands();
  test_wrongtype();
  test_hash_snapshot_persistence();
  test_score_index_merge_after_erase();
  test_member_arena_chunk_boundary();
  test_background_save();
  test_rdb_import();
  test_range_command_parses_indexes();
  test_zadd_updates_existing_members();
  test_score_string_cache_updates_with_scores();
  test_eval_type_conversions();
  test_eval_redis_call();
  test_eval_error_paths();
  test_script_load_and_evalsha();
  test_eval_helper_libraries();
  test_eval_without_engine_is_unavailable();
  test_luau_type_conversions();
  test_luau_redis_call();
  test_luau_script_load_and_evalsha();
  test_luau_is_a_distinct_interpreter();
  test_luau_without_engine_is_unavailable();
  test_wren_type_conversions();
  test_wren_redis_call();
  test_wren_script_load_and_evalsha();
  test_wren_is_a_distinct_interpreter();
  test_wren_without_engine_is_unavailable();
  test_tcl_type_conversions();
  test_tcl_redis_call();
  test_tcl_script_load_and_evalsha();
  test_tcl_is_a_distinct_interpreter();
  test_tcl_without_engine_is_unavailable();
  test_upython_scripting();
  test_upython_without_engine_is_unavailable();
  test_quickjs_scripting();
  test_quickjs_without_engine_is_unavailable();
  test_compare_and_delete_idiom();
  test_evalsha_runs_cached_script();

  test_keyspace_storage_and_string_value();
  return 0;
}
