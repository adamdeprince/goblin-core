#include "goblin/core/command.hpp"
#include "goblin/core/packed_zset.hpp"
#include "goblin/core/replication.hpp"
#include "goblin/core/store.hpp"

#undef NDEBUG
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using goblin::core::PackedZSet;
using goblin::core::PackedZSetAddItem;
using goblin::core::PackedZSetKind;
using goblin::core::Store;
using goblin::core::StoreOptions;
using goblin::core::ZSetImplementation;

std::string run(Store& store, std::vector<std::string_view> fields) {
  return goblin::core::handle_command(store, fields);
}

template <class Integer>
void test_integer_hash_distribution() {
  using Traits = goblin::core::detail::PackedIntegerTraits<Integer>;
  constexpr std::size_t count = 100000;
  for (int shape = 0; shape < 4; ++shape) {
    std::array<std::size_t, 1024> buckets{};
    std::array<std::size_t, 128> fingerprints{};
    for (std::size_t i = 0; i < count; ++i) {
      Integer key;
      if (shape == 0) key = static_cast<Integer>(i);
      else if (shape == 1) key = -static_cast<Integer>(i) - 1;
      else if (shape == 2) {
        key = std::numeric_limits<Integer>::min() + static_cast<Integer>(i);
      } else {
        if constexpr (sizeof(Integer) == 8) {
          key = static_cast<Integer>(i) << 32;
        } else {
          key = std::numeric_limits<Integer>::max() - static_cast<Integer>(i);
        }
      }
      const auto hash = typename Traits::Hash{}(key);
      const auto bucket = static_cast<std::size_t>(
          (static_cast<unsigned __int128>(hash) * buckets.size()) >> 64);
      ++buckets[bucket];
      ++fingerprints[hash & 127];
    }
    // Deliberately loose deterministic bounds: catch identity/low-bit-only
    // hashing without prescribing exact hashes or tuning to page IDs.
    for (auto hits : buckets) assert(hits > 30 && hits < 200);
    for (auto hits : fingerprints) assert(hits > 400 && hits < 1200);
  }
}

template <class Traits, class Score, bool Rle = false>
void test_update_slots_and_allocation_failures() {
  using namespace goblin::core;
  using Key = typename Traits::Key;
  using Index = detail::PackedZSetIndex<Traits, Score, Rle>;
  const auto key_for = [](std::size_t id) {
    if constexpr (std::is_integral_v<Key>) {
      return static_cast<Key>(id);
    } else {
      Key key;
      for (std::size_t b = 0; b < 8; ++b) {
        key.bytes[15 - b] = static_cast<std::uint8_t>(id >> (b * 8));
      }
      return key;
    }
  };
  const auto put = [&](Index& index, std::size_t id, double score,
                       PackedZSetAddOptions options = {}) {
    const auto member = Traits::format(key_for(id));
    const PackedZSetAddItem item{score, member};
    return index.add(std::span(&item, 1), options);
  };
  SwissTable<Key, Score, typename Traits::Hash> sizing;
  sizing.reserve_additional(1);
  const auto full = static_cast<std::size_t>(sizing.capacity() * 0.92);
  Index index(0.5);
  for (std::size_t i = 0; i < full; ++i) assert(put(index, i, i).added == 1);
  index.force_merge();
  const auto allocated = index.allocated_bytes();
  MemoryCeiling deny_growth(1);
  deny_growth.bind(nullptr, [](const void*) noexcept { return std::size_t{1}; });
  {
    MemoryCeilingScope scope(&deny_growth);
    // At the Swiss load ceiling, existing updates must not reserve a new slot.
    assert(put(index, 0, 0.25).changed == 1);
    assert(put(index, 0, 0.25).changed == 0);
    assert(put(index, 0, 50, {.nx = true}).changed == 0);
    assert(put(index, full + 1, 1, {.xx = true}).changed == 0);
    bool rejected = false;
    try { (void)put(index, full, full); }
    catch (const MaxMemoryExceeded&) { rejected = true; }
    assert(rejected);
  }
  assert(index.allocated_bytes() == allocated && index.size() == full);
  assert(*index.find(key_for(0)) == Score{0.25});
  assert(index.find(key_for(full)) == nullptr && index.check_invariants());
  const auto zero = Traits::format(key_for(0));
  const auto one = Traits::format(key_for(1));
  const PackedZSetAddItem batch[] = {{0.5, zero}, {1.5, one}, {0.75, zero}};
  assert(index.add(batch, {}).changed == 3);
  assert(index.allocated_bytes() == allocated && *index.find(key_for(0)) == Score{0.75});

  // A failed tree split must neither publish a new Swiss entry nor overwrite
  // the old score of an existing member moving into a full destination leaf.
  const auto capacity = Index::Order::kLeafCapacity;
  for (const bool existing : {false, true}) {
    Index split(0.5);
    const auto count = existing ? capacity + capacity / 2 : capacity;
    for (std::size_t i = 0; i < count; ++i) assert(put(split, i, i).added == 1);
    split.force_merge();
    bool rejected = false;
    {
      MemoryCeilingScope scope(&deny_growth);
      try { (void)put(split, existing ? 0 : count, count + 100); }
      catch (const MaxMemoryExceeded&) { rejected = true; }
    }
    assert(rejected && split.size() == count && split.check_invariants());
    assert(*split.find(key_for(0)) == Score{0});
    assert(split.find(key_for(count)) == nullptr);
    assert(put(split, existing ? 0 : count, count + 100).changed == 1);
    assert(split.check_invariants());
  }
  // Duplicate new members must reserve one slot, not a whole batch's worth.
  Index duplicates(0.5);
  std::vector<PackedZSetAddItem> repeated(full + 1, {1, zero});
  assert(duplicates.add(repeated, {}).added == 1);
  Index single(0.5);
  assert(put(single, 0, 1).added == 1);
  assert(duplicates.allocated_bytes() == single.allocated_bytes());
}

void test_leaf_dirty_dedup_and_in_place_merge() {
  for (const auto kind : {PackedZSetKind::Int32Float32,
                          PackedZSetKind::Int32Float64,
                          PackedZSetKind::Int64Float32,
                          PackedZSetKind::Int64Float64}) {
    PackedZSet zset(kind);
    std::vector<std::string> members;
    std::vector<PackedZSetAddItem> items;
    members.reserve(64);
    items.reserve(64);
    for (int index = 0; index < 64; ++index) {
      members.push_back(std::to_string(index));
      items.push_back({static_cast<double>(index), members.back()});
    }
    const auto inserted = zset.add(items);
    assert(inserted.added == 64);
    assert(zset.check_invariants());
    zset.force_merge();

    // A -> B -> A in one leaf keeps one distinct dirty record. The old sorted
    // A remains shadowed and no duplicate may leak into a read.
    PackedZSetAddItem update{50.0, "7"};
    assert(zset.add(std::span(&update, 1)).changed == 1);
    update.score = 55.0;
    assert(zset.add(std::span(&update, 1)).changed == 1);
    update.score = 7.0;
    assert(zset.add(std::span(&update, 1)).changed == 1);
    assert(zset.unsorted_size() == 1);
    const auto score_window = zset.range_by_score(7.0, false, 7.0, false);
    assert(score_window.size() == 1 && score_window.front().member == "7");

    const std::string_view removed[] = {"7", "8"};
    assert(zset.remove(removed).removed == 2);
    assert(zset.range_by_rank(0, -1).size() == 62);
    assert(zset.check_invariants());

    zset.force_merge();
    assert(zset.unsorted_size() == 0);
    assert(zset.sorted_entry_count() == zset.size());
    assert(zset.check_invariants());
  }
}

void test_randomized_append_log_against_reference(bool rle = false) {
  constexpr std::array cases{
      std::pair{PackedZSetKind::Int32Float32, 0.0},
      std::pair{PackedZSetKind::Int32Float32, 0.5},
      std::pair{PackedZSetKind::Int32Float32, 1.0},
      std::pair{PackedZSetKind::Int32Float64, 0.0},
      std::pair{PackedZSetKind::Int32Float64, 0.5},
      std::pair{PackedZSetKind::Int32Float64, 1.0},
  };
  for (const auto [kind, merge_exponent] : cases) {
    PackedZSet zset(kind, merge_exponent, rle);
    std::map<int, double> reference;
    std::uint64_t random = 0x8c3c'010c'cb47'563dULL;
    const auto next = [&random] {
      random ^= random << 13;
      random ^= random >> 7;
      random ^= random << 17;
      return random;
    };

    for (int step = 0; step < 10'000; ++step) {
      const int key = static_cast<int>(next() % 257) - 128;
      const auto member = std::to_string(key);
      if (next() % 5 != 0) {
        const double input =
            static_cast<double>(static_cast<int>(next() % 4001) - 2000) /
            8.0;
        const double stored = kind == PackedZSetKind::Int32Float32
                                  ? static_cast<double>(static_cast<float>(input))
                                  : input;
        const PackedZSetAddItem item{input, member};
        const auto result = zset.add(std::span(&item, 1));
        assert(!result.invalid_member && !result.invalid_score);
        reference[key] = stored;
      } else {
        const std::string_view view = member;
        const auto result = zset.remove(std::span(&view, 1));
        assert(!result.invalid_member);
        reference.erase(key);
      }
      assert(zset.entry_count() >= zset.size());

      if (step % 37 == 0) zset.force_merge();
      if (step % 19 != 0) continue;
      std::vector<std::pair<int, double>> expected(reference.begin(),
                                                   reference.end());
      std::sort(expected.begin(), expected.end(), [](const auto& lhs,
                                                     const auto& rhs) {
        return lhs.second < rhs.second ||
               (lhs.second == rhs.second && lhs.first < rhs.first);
      });
      const auto actual = zset.range_by_rank(0, -1);
      assert(actual.size() == expected.size());
      for (std::size_t index = 0; index < actual.size(); ++index) {
        assert(actual[index].member == std::to_string(expected[index].first));
        assert(actual[index].score == expected[index].second);
      }
      assert(zset.check_invariants());
    }
  }
}

void test_multilevel_tree_and_cross_leaf_churn() {
  constexpr int kMembers = 20'000;
  PackedZSet zset(PackedZSetKind::Int32Float64, 0.5);
  std::map<int, double> reference;
  std::vector<std::string> members;
  std::vector<PackedZSetAddItem> items;
  members.reserve(kMembers);
  items.reserve(kMembers);
  for (int key = 0; key < kMembers; ++key) {
    members.push_back(std::to_string(key));
    const auto score = static_cast<double>((key * 37) % 997);
    items.push_back({score, members.back()});
    reference[key] = score;
  }
  assert(zset.add(items).added == kMembers);
  assert(zset.check_invariants());

  std::uint64_t random = 0xd1b5'4a32'd192'ed03ULL;
  const auto next = [&random] {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    return random;
  };
  for (int step = 0; step < 30'000; ++step) {
    const auto key = static_cast<int>(next() % kMembers);
    const auto member = std::to_string(key);
    if (next() % 7 == 0) {
      const std::string_view view = member;
      const auto removed = zset.remove(std::span(&view, 1));
      const auto expected = reference.erase(key);
      assert(removed.removed == expected);
    } else {
      const auto score = static_cast<double>(static_cast<int>(next() % 4001) -
                                             2000) /
                         4.0;
      const PackedZSetAddItem item{score, member};
      const auto changed = zset.add(std::span(&item, 1));
      assert(!changed.invalid_member && !changed.invalid_score);
      reference[key] = score;
    }

    if (step % 2'000 != 0) continue;
    assert(zset.check_invariants());
    std::vector<std::pair<int, double>> expected(reference.begin(),
                                                 reference.end());
    std::sort(expected.begin(), expected.end(), [](const auto& lhs,
                                                   const auto& rhs) {
      return lhs.second < rhs.second ||
             (lhs.second == rhs.second && lhs.first < rhs.first);
    });
    const auto actual = zset.range_by_rank(0, -1);
    assert(actual.size() == expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
      assert(actual[index].member == std::to_string(expected[index].first));
      assert(actual[index].score == expected[index].second);
    }
    if (!expected.empty()) {
      const auto probe = static_cast<std::size_t>(next() % expected.size());
      const auto rank = zset.rank(std::to_string(expected[probe].first));
      assert(rank.valid_member && rank.rank == probe);
    }
  }
  assert(zset.check_invariants());
}

void test_configurable_merge_exponent() {
  std::vector<std::string> members;
  std::vector<PackedZSetAddItem> items;
  members.reserve(64);
  items.reserve(64);
  for (int index = 0; index < 64; ++index) {
    members.push_back(std::to_string(index));
    items.push_back({static_cast<double>(index), members.back()});
  }

  PackedZSet eager(PackedZSetKind::Int32Float64, 0.0);
  assert(eager.add(items).added == 64);
  assert(eager.merge_exponent() == 0.0);
  assert(eager.merge_threshold() == 1);
  assert(eager.unsorted_size() == 0);

  PackedZSet balanced(PackedZSetKind::Int32Float64, 0.5);
  assert(balanced.add(items).added == 64);
  assert(balanced.merge_threshold() == 16);
  assert(balanced.unsorted_size() == 0);

  PackedZSet fractional(PackedZSetKind::Int32Float64, 0.75);
  assert(fractional.add(items).added == 64);
  assert(fractional.merge_threshold() == 64);
  assert(fractional.unsorted_size() == 0);

  PackedZSet write_heavy(PackedZSetKind::Int32Float64, 1.0);
  assert(write_heavy.add(items).added == 64);
  assert(write_heavy.merge_exponent() == 1.0);
  assert(write_heavy.merge_threshold() == 256);
  assert(write_heavy.unsorted_size() == 64);
  assert(write_heavy.check_invariants());

  bool rejected = false;
  try {
    PackedZSet invalid(PackedZSetKind::Int32Float64, 1.01);
    (void)invalid;
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);

  StoreOptions invalid_options;
  invalid_options.packed_zset_merge_exponent = -0.01;
  rejected = false;
  try {
    Store invalid_store(invalid_options);
    (void)invalid_store;
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  assert(rejected);

  StoreOptions options;
  options.packed_zset_merge_exponent = 1.0;
  Store store(options);
  assert(store.packed_zadd("configured", PackedZSetKind::Int32Float64, items)
             .added == 64);
  const auto configured = store.packed_zset_memory_stats("configured");
  assert(configured && configured->merge_exponent == 1.0);
  assert(configured->merge_threshold == 256);
  assert(configured->unsorted_entries == 64);
  assert(run(store, {"GOBLIN.MEMORY", "configured"})
             .find("merge_exponent") != std::string::npos);

  assert(run(store, {"COPY", "configured", "configured-copy"}) == ":1\r\n");
  const auto copied = store.packed_zset_memory_stats("configured-copy");
  assert(copied && copied->merge_exponent == 1.0);

  std::stringstream snapshot;
  store.save(snapshot, false);
  StoreOptions restored_options;
  restored_options.packed_zset_merge_exponent = 0.0;
  Store restored(restored_options);
  assert(restored.load(snapshot).keys == 2);
  const auto loaded = restored.packed_zset_memory_stats("configured");
  assert(loaded && loaded->merge_exponent == 0.0);
  assert(loaded->merge_threshold == 1);
  assert(loaded->unsorted_entries == 0);
}

void test_score_rle_store_policy() {
  StoreOptions options;
  // Exercise the default through creation, copy, aggregate stores, and load.
  options.zset_implementation = goblin::core::ZSetImplementation::PackedInt32Float32;
  Store store(options);
  std::vector<std::string> members;
  std::vector<PackedZSetAddItem> items;
  members.reserve(1200);
  items.reserve(1200);
  for (int i = 0; i < 1200; ++i) {
    members.push_back(std::to_string(i));
    items.push_back({static_cast<double>(i / 100), members.back()});
  }
  assert(store.packed_zadd("runs", PackedZSetKind::Int32Float32, items).added == 1200);
  assert(run(store, {"COPY", "runs", "copy"}) == ":1\r\n");
  assert(run(store, {"ZUNIONSTORE", "union", "2", "runs", "copy"}) == ":1200\r\n");
  assert(run(store, {"ZINTERSTORE", "intersection", "2", "runs", "copy"}) == ":1200\r\n");
  assert(run(store, {"ZADD", "ordinary", "1", "1", "1", "2", "1", "3", "1", "4"}) == ":4\r\n");
  for (const auto key : {"runs", "copy", "union", "intersection", "ordinary"}) {
    assert(run(store, {"GOBLIN.OPTIMIZE", key}).front() == ':');
    const auto stats = store.packed_zset_memory_stats(key);
    assert(stats && stats->score_rle && stats->compressed_leaf_count != 0);
    assert(stats->sorted_score_bytes < stats->sorted_entries * sizeof(float));
    const auto reply = run(store, {"GOBLIN.MEMORY", key});
    assert(reply.find("score_rle") != std::string::npos);
    assert(reply.find("compressed_leaf_count") != std::string::npos);
    assert(reply.find("sorted_score_bytes") != std::string::npos);
  }
  const auto expected = run(store, {"ZRANGE", "runs", "0", "-1", "WITHSCORES"});
  assert(run(store, {"ZRANGE", "copy", "0", "-1", "WITHSCORES"}) == expected);
  std::stringstream snapshot;
  store.save(snapshot, false);
  for (const bool enabled : {false, true}) {
    StoreOptions receiver;
    receiver.packed_zset_score_rle = enabled;
    Store restored(receiver);
    snapshot.clear();
    snapshot.seekg(0);
    assert(restored.load(snapshot).keys == 5);
    const auto stats = restored.packed_zset_memory_stats("runs");
    assert(stats && stats->score_rle == enabled);
    assert(run(restored, {"ZRANGE", "runs", "0", "-1", "WITHSCORES"}) == expected);
    // Also cover a snapshot emitted by the ordinary layout loading into RLE.
    std::stringstream again;
    restored.save(again, false);
    Store compressed(options);
    assert(compressed.load(again).keys == 5);
    assert(compressed.packed_zset_memory_stats("runs")->score_rle);
    assert(run(compressed, {"ZRANGE", "runs", "0", "-1", "WITHSCORES"}) == expected);
  }
}

void test_all_command_prefixes() {
  const std::vector<std::string> prefixes{
      "GOBLIN.PACKED_INT32_FLOAT32",
      "GOBLIN.PACKED_INT32_FLOAT64",
      "GOBLIN.PACKED_INT64_FLOAT32",
      "GOBLIN.PACKED_INT64_FLOAT64",
  };
  for (const auto& prefix : prefixes) {
    Store store;
    const auto zadd = prefix + ".ZADD";
    const auto zrange = prefix + ".ZRANGE";
    const auto zscore = prefix + ".ZSCORE";
    const auto zrank = prefix + ".ZRANK";
    const auto zrem = prefix + ".ZREM";
    const auto zcard = prefix + ".ZCARD";

    auto parsed = goblin::core::parse_command(
        std::vector<std::string_view>{zadd, "numbers", "1", "10"});
    assert(parsed.ok() && parsed.command->packed_zset_kind != 0);
    assert(goblin::core::lookup_command_type(zadd) ==
           goblin::core::CommandType::zadd);

    assert(run(store, {zadd, "numbers", "1", "10", "1", "2", "0", "-3"}) ==
           ":3\r\n");
    // Equal-score integer members use their binary numeric order, not the
    // lexical order of their input strings.
    assert(run(store, {zrange, "numbers", "0", "-1"}) ==
           "*3\r\n$2\r\n-3\r\n$1\r\n2\r\n$2\r\n10\r\n");
    assert(run(store, {zscore, "numbers", "10"}) == "$1\r\n1\r\n");
    assert(run(store, {zrank, "numbers", "10"}) == ":2\r\n");
    assert(run(store, {zadd, "numbers", "CH", "2", "10"}) == ":1\r\n");
    assert(run(store, {zrem, "numbers", "2"}) == ":1\r\n");
    assert(run(store, {zcard, "numbers"}) == ":2\r\n");
    assert(run(store, {zscore, "numbers", "not-an-integer"}).starts_with(
        "-ERR member is not a valid"));
    if (prefix.find("INT32") != std::string::npos) {
      assert(run(store, {zscore, "numbers", "2147483648"}).starts_with(
          "-ERR member is not a valid INT32"));
    }
  }

  Store store;
  const auto command_info = run(
      store, {"COMMAND", "INFO", "GOBLIN.PACKED_INT32_FLOAT32.ZADD"});
  assert(command_info.find("goblin.packed_int32_float32.zadd") !=
         std::string::npos);
  const auto command_catalog = run(store, {"COMMAND"});
  assert(command_catalog.find("goblin.packed_uuid_float64.zscan") !=
         std::string::npos);
}

void test_uuid_commands_and_representation_gate() {
  Store store;
  constexpr std::string_view prefix = "GOBLIN.PACKED_UUID_FLOAT64";
  const std::string zadd = std::string(prefix) + ".ZADD";
  const std::string zrange = std::string(prefix) + ".ZRANGE";
  constexpr std::string_view first =
      "00112233-4455-6677-8899-aabbccddeeff";
  constexpr std::string_view second =
      "00112233-4455-6677-8899-AABBCCDDEF00";

  assert(run(store, {zadd, "uuids", "1", second, "1", first}) == ":2\r\n");
  assert(run(store, {zrange, "uuids", "0", "-1"}) ==
         "*2\r\n$36\r\n00112233-4455-6677-8899-aabbccddeeff\r\n"
         "$36\r\n00112233-4455-6677-8899-aabbccddef00\r\n");
  assert(run(store, {std::string(prefix) + ".ZSCORE", "uuids",
                     "00112233445566778899AABBCCDDEEFF"}) ==
         "$1\r\n1\r\n");

  // Once a packed key exists, ordinary Z* commands resolve through its pinned
  // representation even when the server default is the standard zset.
  assert(run(store, {"ZADD", "uuids", "2", "plain"}).starts_with(
      "-ERR member is not a valid UUID"));
  assert(run(store,
             {"GOBLIN.PACKED_UUID_FLOAT32.ZCARD", "uuids"})
             .starts_with("-WRONGTYPE"));
  assert(run(store,
             {"GOBLIN.PACKED_UUID_FLOAT32.ZADD", "uuids32", "0.5", first}) ==
         ":1\r\n");
  assert(run(store,
             {"GOBLIN.PACKED_UUID_FLOAT32.ZSCORE", "uuids32", first}) ==
         "$3\r\n0.5\r\n");
  assert(run(store, {"TYPE", "uuids"}) == "+zset\r\n");
  assert(run(store, {"RENAME", "uuids32", "moved-uuids"}) == "+OK\r\n");
  assert(run(store,
             {"GOBLIN.PACKED_UUID_FLOAT32.ZSCORE", "moved-uuids", first}) ==
         "$3\r\n0.5\r\n");
  assert(run(store, {"SET", "uuids", "replacement"}) == "+OK\r\n");
  assert(run(store, {"GET", "uuids"}) == "$11\r\nreplacement\r\n");
}

void test_float32_scores_and_full_surface() {
  Store store;
  constexpr std::string_view p = "GOBLIN.PACKED_INT32_FLOAT32";
  const std::string zadd = std::string(p) + ".ZADD";
  assert(run(store, {zadd, "overflow", "3.5e38", "1"}).starts_with(
      "-ERR resulting score is not representable"));
  assert(run(store, {std::string(p) + ".ZCARD", "overflow"}) == ":0\r\n");
  assert(run(store, {zadd, "f", "0.1", "1", "2", "2", "3", "3"}) ==
         ":3\r\n");
  const auto score = store.packed_zscore(
      "f", PackedZSetKind::Int32Float32, "1");
  assert(score.score && *score.score == static_cast<double>(0.1F));

  assert(run(store, {std::string(p) + ".ZCOUNT", "f", "(1", "+inf"}) ==
         ":2\r\n");
  assert(run(store,
             {std::string(p) + ".ZRANGEBYSCORE", "f", "-inf", "+inf",
              "WITHSCORES", "LIMIT", "1", "1"}) ==
         "*2\r\n$1\r\n2\r\n$1\r\n2\r\n");
  assert(run(store, {std::string(p) + ".ZINCRBY", "f", "4", "2"}) ==
         "$1\r\n6\r\n");
  assert(run(store, {std::string(p) + ".ZMSCORE", "f", "1", "99"}) ==
         "*2\r\n$19\r\n0.10000000149011612\r\n$-1\r\n");
  assert(run(store, {std::string(p) + ".ZPOPMAX", "f"}) ==
         "*2\r\n$1\r\n2\r\n$1\r\n6\r\n");
  assert(run(store,
             {std::string(p) + ".ZREMRANGEBYSCORE", "f", "-inf", "1"}) ==
         ":1\r\n");
  assert(run(store,
             {std::string(p) + ".ZREMRANGEBYRANK", "f", "0", "-1"}) ==
         ":1\r\n");
  assert(run(store, {std::string(p) + ".ZCARD", "f"}) == ":0\r\n");
}

void test_union_and_intersection() {
  Store store;
  constexpr std::string_view p = "GOBLIN.PACKED_INT64_FLOAT64";
  assert(run(store, {std::string(p) + ".ZADD", "left", "1", "1", "2",
                     "2", "3", "3"}) == ":3\r\n");
  assert(run(store, {std::string(p) + ".ZADD", "right", "4", "2", "5",
                     "3", "6", "4"}) == ":3\r\n");
  assert(run(store,
             {std::string(p) + ".ZUNIONSTORE", "union", "2", "left",
              "right", "WEIGHTS", "2", "3", "AGGREGATE", "SUM"}) ==
         ":4\r\n");
  assert(run(store,
             {std::string(p) + ".ZRANGE", "union", "0", "-1",
              "WITHSCORES"}) ==
         "*8\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n2\r\n$2\r\n16\r\n"
         "$1\r\n4\r\n$2\r\n18\r\n$1\r\n3\r\n$2\r\n21\r\n");
  assert(run(store,
             {std::string(p) + ".ZINTERSTORE", "intersection", "2",
              "left", "right", "AGGREGATE", "MAX"}) == ":2\r\n");
  assert(run(store,
             {std::string(p) + ".ZRANGE", "intersection", "0", "-1",
              "WITHSCORES"}) ==
         "*4\r\n$1\r\n2\r\n$1\r\n4\r\n$1\r\n3\r\n$1\r\n5\r\n");

  assert(run(store, {"SET", "overwrite", "value"}) == "+OK\r\n");
  assert(run(store,
             {std::string(p) + ".ZUNIONSTORE", "overwrite", "1", "left"}) ==
         ":3\r\n");
  assert(store.packed_zset_kind("overwrite") ==
         PackedZSetKind::Int64Float64);
  assert(run(store,
             {"GOBLIN.PACKED_INT64_FLOAT32.ZUNIONSTORE", "bad", "1",
              "left"})
             .starts_with("-WRONGTYPE"));

  constexpr std::string_view f32 = "GOBLIN.PACKED_INT32_FLOAT32";
  assert(run(store, {std::string(f32) + ".ZADD", "f32-a", "100000000", "1"}) ==
         ":1\r\n");
  assert(run(store, {std::string(f32) + ".ZADD", "f32-b", "1", "1"}) ==
         ":1\r\n");
  assert(run(store,
             {std::string(f32) + ".ZADD", "f32-c", "-100000000", "1"}) ==
         ":1\r\n");
  assert(run(store,
             {std::string(f32) + ".ZUNIONSTORE", "f32-union", "3", "f32-a",
              "f32-b", "f32-c"}) == ":1\r\n");
  // Aggregate in double and narrow once at the destination boundary.
  assert(run(store,
             {std::string(f32) + ".ZSCORE", "f32-union", "1"}) ==
         "$1\r\n1\r\n");
}

void test_default_implementation_selector() {
  struct Case {
    ZSetImplementation implementation;
    PackedZSetKind kind;
    std::string_view member;
    std::string_view canonical_member;
  };
  constexpr std::array cases{
      Case{ZSetImplementation::PackedInt32Float32,
           PackedZSetKind::Int32Float32, "17", "17"},
      Case{ZSetImplementation::PackedInt32Float64,
           PackedZSetKind::Int32Float64, "17", "17"},
      Case{ZSetImplementation::PackedInt64Float32,
           PackedZSetKind::Int64Float32, "9223372036854775807",
           "9223372036854775807"},
      Case{ZSetImplementation::PackedInt64Float64,
           PackedZSetKind::Int64Float64, "9223372036854775807",
           "9223372036854775807"},
      Case{ZSetImplementation::PackedUuidFloat32,
           PackedZSetKind::UuidFloat32,
           "00112233-4455-6677-8899-AABBCCDDEEFF",
           "00112233-4455-6677-8899-aabbccddeeff"},
      Case{ZSetImplementation::PackedUuidFloat64,
           PackedZSetKind::UuidFloat64,
           "00112233-4455-6677-8899-AABBCCDDEEFF",
           "00112233-4455-6677-8899-aabbccddeeff"},
  };

  for (const auto& test : cases) {
    assert(goblin::core::parse_zset_implementation(
               goblin::core::zset_implementation_name(test.implementation)) ==
           test.implementation);
    StoreOptions options;
    options.zset_implementation = test.implementation;
    Store store(options);
    assert(store.zset_implementation() == test.implementation);
    assert(store.default_packed_zset_kind() == test.kind);
    assert(run(store, {"ZADD", "selected", "0.1", test.member}) ==
           ":1\r\n");
    assert(store.packed_zset_kind("selected") == test.kind);
    assert(store.packed_zset_memory_stats("selected")->score_rle);
    assert(run(store, {"ZCARD", "selected"}) == ":1\r\n");
    const auto range = run(store, {"ZRANGE", "selected", "0", "-1"});
    assert(range.find(test.canonical_member) != std::string::npos);

    const auto score = store.packed_zscore("selected", test.kind, test.member);
    assert(score.score.has_value());
    const bool float32 = test.kind == PackedZSetKind::Int32Float32 ||
                         test.kind == PackedZSetKind::Int64Float32 ||
                         test.kind == PackedZSetKind::UuidFloat32;
    assert(*score.score ==
           (float32 ? static_cast<double>(0.1F) : 0.1));

    // The public C++ wrapper follows the server default, with an explicit
    // opt-out that preserves the same logical data for all six layouts.
    PackedZSet packed(test.kind);
    PackedZSet raw(test.kind, goblin::core::kDefaultPackedZSetMergeExponent,
                   false);
    const PackedZSetAddItem item{0.1, test.member};
    for (auto* zset : {&packed, &raw}) {
      assert(zset->add(std::span(&item, 1), {}).added == 1);
      assert(zset->check_invariants());
    }
    assert(packed.score_rle_enabled());
    assert(!raw.score_rle_enabled());
    const auto packed_entries = packed.range_by_rank(0, -1, false);
    const auto raw_entries = raw.range_by_rank(0, -1, false);
    assert(packed_entries.size() == 1 && raw_entries.size() == 1);
    assert(packed_entries[0].member == raw_entries[0].member);
    assert(packed_entries[0].score == raw_entries[0].score);
  }

  assert(goblin::core::parse_zset_implementation("standard") ==
         ZSetImplementation::Standard);
  assert(goblin::core::parse_zset_implementation(
             "packed-int64-float32") ==
         ZSetImplementation::PackedInt64Float32);
  assert(!goblin::core::parse_zset_implementation("packed-int16-float32"));

  Store standard;
  assert(run(standard, {"ZADD", "ordinary", "1", "not-an-integer"}) ==
         ":1\r\n");
  assert(standard.key_type("ordinary") == goblin::core::KeyType::Zset);

  // The configured representation applies only to newly created destinations.
  // A restored standard key remains standard and is still addressable through
  // ordinary commands.
  std::stringstream snapshot;
  standard.save(snapshot, false);
  StoreOptions selected_options;
  selected_options.zset_implementation =
      ZSetImplementation::PackedInt32Float64;
  Store selected(selected_options);
  assert(selected.load(snapshot).keys == 1);
  assert(run(selected, {"ZSCORE", "ordinary", "not-an-integer"}) ==
         "$1\r\n1\r\n");
  assert(!selected.packed_zset_kind("ordinary"));
  assert(run(selected, {"ZADD", "new", "2", "2"}) == ":1\r\n");
  assert(selected.packed_zset_kind("new") ==
         PackedZSetKind::Int32Float64);

  // Aggregate stores accept either source representation and create their
  // destination in the configured default representation.
  assert(run(selected,
             {"ZUNIONSTORE", "mixed-union", "2", "ordinary", "new"})
             .starts_with("-ERR member is not a valid INT32"));
  assert(run(selected, {"ZADD", "numeric-standard", "3", "3"}) ==
         ":1\r\n");
  // numeric-standard was just created under the packed default. Load a normal
  // numeric source to exercise a genuinely mixed aggregate.
  Store normal_numeric;
  assert(run(normal_numeric, {"ZADD", "normal-numeric", "1", "2"}) ==
         ":1\r\n");
  std::stringstream numeric_snapshot;
  normal_numeric.save(numeric_snapshot, false);
  Store mixed(selected_options);
  assert(mixed.load(numeric_snapshot).keys == 1);
  assert(run(mixed, {"ZADD", "packed-numeric", "3", "2", "4", "4"}) ==
         ":2\r\n");
  assert(run(mixed,
             {"ZUNIONSTORE", "mixed-union", "2", "normal-numeric",
              "packed-numeric"}) == ":2\r\n");
  assert(mixed.packed_zset_kind("mixed-union") ==
         PackedZSetKind::Int32Float64);
  assert(run(mixed,
             {"ZINTERSTORE", "mixed-intersection", "2", "normal-numeric",
              "packed-numeric"}) == ":1\r\n");
  assert(mixed.packed_zset_kind("mixed-intersection") ==
         PackedZSetKind::Int32Float64);
  assert(run(mixed,
             {"ZSCORE", "mixed-intersection", "2"}) == "$1\r\n4\r\n");
}

void test_snapshot_copy_and_optimize() {
  Store store;
  const std::string zadd = "GOBLIN.PACKED_INT64_FLOAT64.ZADD";
  assert(run(store, {zadd, "packed", "3.5", "9223372036854775807",
                     "-1.25", "-9223372036854775808"}) == ":2\r\n");
  assert(run(store, {"COPY", "packed", "packed-copy"}) == ":1\r\n");
  assert(store.packed_zset_kind("packed-copy") ==
         PackedZSetKind::Int64Float64);
  assert(run(store, {"GOBLIN.OPTIMIZE", "packed"}).front() == ':');
  const auto memory = run(store, {"GOBLIN.MEMORY", "packed"});
  assert(memory.find("INT64_FLOAT64") != std::string::npos);
  assert(memory.find("unsorted_entries") != std::string::npos);

  std::stringstream snapshot;
  store.save(snapshot, false);
  Store loaded;
  const auto stats = loaded.load(snapshot);
  assert(stats.keys == 2 && stats.members == 4);
  assert(loaded.packed_zset_kind("packed") ==
         PackedZSetKind::Int64Float64);
  assert(run(loaded,
             {"GOBLIN.PACKED_INT64_FLOAT64.ZRANGE", "packed", "0", "-1",
              "WITHSCORES"}) ==
         "*4\r\n$20\r\n-9223372036854775808\r\n$5\r\n-1.25\r\n"
         "$19\r\n9223372036854775807\r\n$3\r\n3.5\r\n");
}

void test_replication_round_trip(bool rle = false) {
  Store source;
  StoreOptions receiver_options;
  receiver_options.packed_zset_score_rle = rle;
  Store target(receiver_options);
  std::vector<std::string_view> fields{
      "GOBLIN.PACKED_INT32_FLOAT64.ZADD", "replicated", "2.5", "17"};
  auto parsed = goblin::core::parse_command(fields);
  assert(parsed.ok());
  const auto response =
      goblin::core::execute_command(source, *parsed.command);
  assert(response == ":1\r\n");
  const auto mutations = goblin::core::build_replication_mutations(
      source, *parsed.command, response);
  assert(mutations.size() == 1);
  assert(mutations.front().payload.find(
             "GOBLIN.PACKED_INT32_FLOAT64.ZADD") != std::string::npos);

  const auto id = goblin::core::make_replication_id();
  target.set_replication_state(
      {.id = id, .offset = 0, .valid = true});
  const goblin::core::ReplicationBatch batch{
      .id = id, .offset = 1, .mutations = mutations};
  std::string error;
  assert(goblin::core::apply_firehose_batch(target, batch, error));
  assert(target.packed_zset_kind("replicated") ==
         PackedZSetKind::Int32Float64);
  assert(target.packed_zset_memory_stats("replicated")->score_rle == rle);
  assert(run(target,
             {"GOBLIN.PACKED_INT32_FLOAT64.ZSCORE", "replicated", "17"}) ==
         "$3\r\n2.5\r\n");

  StoreOptions default_options;
  default_options.zset_implementation =
      ZSetImplementation::PackedInt64Float32;
  Store default_source(default_options);
  Store default_target(receiver_options);
  std::vector<std::string_view> ordinary_fields{
      "ZADD", "ordinary-replicated", "0.1", "91"};
  auto ordinary = goblin::core::parse_command(ordinary_fields);
  assert(ordinary.ok());
  const auto ordinary_response =
      goblin::core::execute_command(default_source, *ordinary.command);
  assert(ordinary_response == ":1\r\n");
  const auto ordinary_mutations = goblin::core::build_replication_mutations(
      default_source, *ordinary.command, ordinary_response);
  assert(ordinary_mutations.size() == 1);
  assert(ordinary_mutations.front().payload.find(
             "GOBLIN.PACKED_INT64_FLOAT32.ZADD") != std::string::npos);
  const goblin::core::ReplicationBatch ordinary_batch{
      .id = id, .offset = 1, .mutations = ordinary_mutations};
  default_target.set_replication_state(
      {.id = id, .offset = 0, .valid = true});
  error.clear();
  assert(goblin::core::apply_firehose_batch(default_target, ordinary_batch,
                                             error));
  assert(default_target.packed_zset_kind("ordinary-replicated") ==
         PackedZSetKind::Int64Float32);
  assert(default_target.packed_zset_memory_stats("ordinary-replicated")->score_rle == rle);
  assert(run(default_target,
             {"ZSCORE", "ordinary-replicated", "91"}) ==
         "$19\r\n0.10000000149011612\r\n");
}

}  // namespace

int main() {
  test_integer_hash_distribution<std::int32_t>();
  test_integer_hash_distribution<std::int64_t>();
  using namespace goblin::core::detail;
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int32_t>, float>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int32_t>, float, true>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int32_t>, double>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int32_t>, double, true>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int64_t>, float>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int64_t>, float, true>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int64_t>, double>();
  test_update_slots_and_allocation_failures<PackedIntegerTraits<std::int64_t>, double, true>();
  test_update_slots_and_allocation_failures<PackedUuidTraits, float>();
  test_update_slots_and_allocation_failures<PackedUuidTraits, float, true>();
  test_update_slots_and_allocation_failures<PackedUuidTraits, double>();
  test_update_slots_and_allocation_failures<PackedUuidTraits, double, true>();
  test_leaf_dirty_dedup_and_in_place_merge();
  test_randomized_append_log_against_reference();
  test_randomized_append_log_against_reference(true);
  test_multilevel_tree_and_cross_leaf_churn();
  test_configurable_merge_exponent();
  test_score_rle_store_policy();
  test_all_command_prefixes();
  test_uuid_commands_and_representation_gate();
  test_float32_scores_and_full_surface();
  test_union_and_intersection();
  test_default_implementation_selector();
  test_snapshot_copy_and_optimize();
  test_replication_round_trip();
  test_replication_round_trip(true);
}
