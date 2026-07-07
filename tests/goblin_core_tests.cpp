#include "goblin/core/command.hpp"
#include "goblin/core/chunked_sorted_list.hpp"
#include "goblin/core/hash.hpp"
#include "goblin/core/rdb.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"
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
  assert(memory.starts_with("*36\r\n"));
  assert(memory.find("member_index_allocated_bytes") != std::string::npos);
  assert(memory.find("rank_cache_mode") != std::string::npos);
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

  for (int i = 1023; i >= 0; --i) {
    assert(zset.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
  }

  assert(zset.size() == 1024);
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

void test_store_inline_and_overflow_zsets() {
  goblin::core::Store store;

  assert(store.zadd("one", 1.0, "a") == 1);
  auto stats = store.memory_stats();
  assert(stats.inline_zset_count == 1);
  assert(stats.overflow_zset_count == 0);

  assert(store.zadd("two", 2.0, "b") == 1);
  stats = store.memory_stats();
  assert(stats.inline_zset_count == 1);
  assert(stats.overflow_zset_count == 1);

  assert(store.zscore("one", "a") == 1.0);
  assert(store.zscore("two", "b") == 2.0);

  const std::vector<std::string_view> remove_a{"a"};
  assert(store.zrem("one", remove_a) == 1);
  assert(store.zcard("one") == 0);
  assert(store.zscore("two", "b") == 2.0);

  stats = store.memory_stats();
  assert(stats.inline_zset_count == 0);
  assert(stats.overflow_zset_count == 1);

  assert(store.zadd("three", 3.0, "c") == 1);
  assert(store.zscore("two", "b") == 2.0);
  assert(store.zscore("three", "c") == 3.0);
  stats = store.memory_stats();
  assert(stats.inline_zset_count == 0);
  assert(stats.overflow_zset_count == 2);
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
  goblin::core::ZSetScoreIndex forced_index(
      &forced_storage,
      goblin::core::RankCacheMode::BlockHint,
      2);

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
         goblin::core::ZSetScoreIndex::kLoad);

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

  goblin::core::ZSet z(goblin::core::ZSetOptions{
      .member_chunk_bytes = std::size_t{1} << 16});  // 64 KiB
  for (int i = 0; i < 5000; ++i) {
    assert(z.add(static_cast<double>(i), "member-" + std::to_string(i)) == 1);
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

}  // namespace

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
  test_store_inline_and_overflow_zsets();
  test_store_rank_location_cache();
  test_block_hint_rank_cache_uses_narrow_storage();
  test_block_hint_rank_cache_lazy_offset_repair();
  test_block_hint_rank_cache_promotes_to_wide_storage();
  test_resp_parser_incremental();
  test_inline_parser();
  test_protocol_error();
  test_resp_parser_pipeline_and_pop_into();
  test_resp_bulk_writer_small_header_table();
  test_resp_array_writer_small_header_table();
  test_resp_bulk_wire_size_matches_output();
  test_zrange_withscores_fused_matches_legacy_append();
  test_command_dispatch();
  test_goblin_optimize_reclaims_slack();
  test_goblin_optimize_density_and_growth();
  test_snapshot_round_trip();
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
  return 0;
}
