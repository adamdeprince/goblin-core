// Direct packed-tree mutation timings, excluding protocol/Swiss-table work.
// Build: c++ -O3 -DNDEBUG -std=c++23 -Iinclude benchmarks/packed_merge.cpp -o /tmp/packed-merge
// Run: packed-merge [operations=1000000] [local|increment|random|churn|rebalance]
#include "goblin/core/packed_bplus_tree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

struct IntegerTraits {
  using Key = std::int32_t;
  static bool less(Key a, Key b) noexcept { return a < b; }
};

int main(int argc, char** argv) {
  using Tree = goblin::core::detail::PackedBPlusTree<IntegerTraits, float>;
  const std::size_t operations = argc > 1 ? std::stoull(argv[1]) : 1000000;
  const std::string shape = argc > 2 ? argv[2] : "local";
  if (shape == "rebalance" && operations != 0) {
    const auto capacity = Tree::kLeafCapacity;
    const auto count = capacity + capacity / 2;
    const auto trials = std::max<std::size_t>(1, operations / capacity);
    double micros = 0.0;
    std::uint64_t checksum = 0;
    for (std::size_t trial = 0; trial < trials; ++trial) {
      Tree tree(0.5);
      const bool reverse = trial % 2 != 0;
      for (std::size_t i = 0; i < count; ++i) {
        const auto id = static_cast<std::int32_t>(reverse ? count - i - 1 : i);
        tree.insert({static_cast<float>(id), id});
      }
      tree.force_merge();
      for (std::size_t i = 0; i < capacity / 4; ++i) {
        const auto id = static_cast<std::int32_t>(reverse ? count - i - 1 : i);
        tree.erase({static_cast<float>(id), id});
      }
      const auto id = static_cast<std::int32_t>(reverse ? count - capacity / 4 - 1
                                                       : capacity / 4);
      const auto begin = std::chrono::steady_clock::now();
      tree.erase({static_cast<float>(id), id});
      const auto end = std::chrono::steady_clock::now();
      micros += std::chrono::duration<double, std::micro>(end - begin).count();
      if (tree.leaf_count() != 2 || !tree.check_invariants() ||
          tree.size() != count - capacity / 4 - 1) {
        throw std::logic_error("invalid redistribution");
      }
      checksum += tree.size();
    }
    std::printf("shape\tmembers\toperations\tus_per_op\tchecksum\n");
    std::printf("rebalance\t%zu\t%zu\t%.6f\t%llu\n", count, trials,
                micros / trials, static_cast<unsigned long long>(checksum));
    return 0;
  }
  if (operations == 0 || (shape != "local" && shape != "increment" &&
                          shape != "random" && shape != "churn")) {
    throw std::invalid_argument("invalid operations or shape");
  }
  const std::size_t count = shape == "local" ? 384 : 20000;
  Tree tree(0.5);
  std::vector<float> scores(count);
  for (std::size_t id = 0; id < count; ++id) {
    scores[id] = shape == "increment" ? 1.0f : static_cast<float>(id);
    tree.insert({scores[id], static_cast<std::int32_t>(id)});
  }
  tree.force_merge();

  std::uint64_t rng = 0x1234'5678'abcd'0123ULL;
  const auto next = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  struct Change { std::int32_t key; float score; };
  std::vector<Change> changes;
  changes.reserve(operations);
  for (std::size_t op = 0; op < operations; ++op) {
    const auto id = static_cast<std::int32_t>(next() % count);
    changes.push_back({id, static_cast<float>(next() % (count * 2)) -
                              static_cast<float>(count)});
  }

  const auto begin = std::chrono::steady_clock::now();
  for (const auto change : changes) {
    const auto id = change.key;
    const auto candidate = shape == "increment" ? scores[id] + 1.0f
                                                 : change.score;
    if (shape == "churn") {
      tree.erase({scores[id], id});
      tree.insert({candidate, id});
    } else {
      tree.replace({scores[id], id}, {candidate, id});
    }
    scores[id] = candidate;
  }
  const auto end = std::chrono::steady_clock::now();

  // Full mapping/order validation is outside the timed region.
  if (!tree.check_invariants() || tree.size() != count) {
    throw std::logic_error("invalid final tree");
  }
  const auto entries = tree.range_by_rank(0, -1, false);
  std::vector<bool> seen(count);
  std::uint64_t checksum = 0;
  for (const auto& entry : entries) {
    if (entry.key < 0 || static_cast<std::size_t>(entry.key) >= count ||
        seen[entry.key] || entry.score != scores[entry.key]) {
      throw std::logic_error("incorrect final mapping");
    }
    seen[entry.key] = true;
    checksum = checksum * 131 + static_cast<std::uint32_t>(entry.key);
  }
  const auto micros =
      std::chrono::duration<double, std::micro>(end - begin).count();
  std::printf("shape\tmembers\toperations\tus_per_op\tleaves\tallocated_bytes\tchecksum\n");
  std::printf("%s\t%zu\t%zu\t%.6f\t%zu\t%zu\t%llu\n", shape.c_str(),
              count, operations, micros / operations, tree.leaf_count(),
              tree.allocated_bytes(), static_cast<unsigned long long>(checksum));
}
