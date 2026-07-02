#include "goblin/core/command.hpp"
#include "goblin/core/chunked_sorted_list.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/store.hpp"
#include "goblin/core/swiss_table.hpp"

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
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

}  // namespace

int main() {
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
  test_resp_parser_incremental();
  test_inline_parser();
  test_protocol_error();
  test_resp_bulk_writer_small_header_table();
  test_resp_array_writer_small_header_table();
  test_command_dispatch();
  test_range_command_parses_indexes();
  test_zadd_updates_existing_members();
  test_score_string_cache_updates_with_scores();
  return 0;
}
