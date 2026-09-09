// Full packed index updates (Swiss + tree + numeric member parsing), no protocol.
// Build with each stage's frozen headers. Setup and validation are not timed.
#include "goblin/core/packed_zset.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  using namespace goblin::core;
  const std::size_t operations = argc > 1 ? std::stoull(argv[1]) : 200000;
  const std::string shape = argc > 2 ? argv[2] : "increment";
  if (operations == 0 || (shape != "local" && shape != "increment" &&
                         shape != "random" && shape != "insert" &&
                         shape != "churn" && shape != "erase" &&
                         shape != "noop")) {
    throw std::invalid_argument(
        "expected operations and local|increment|random|insert|churn|erase|noop");
  }
  const std::size_t count = shape == "local" ? 384 :
      (shape == "insert" || shape == "erase") ? operations : 20000;
  detail::PackedInt32Float32 index(0.5);
  std::vector<std::string> members;
  std::vector<float> scores(count);
  members.reserve(count);
  for (std::size_t id = 0; id < count; ++id) {
    members.push_back(std::to_string(id));
    scores[id] = shape == "increment" ? 1.0f : static_cast<float>(id);
    if (shape != "insert") {
      const PackedZSetAddItem item{scores[id], members.back()};
      if (index.add(std::span(&item, 1), {}).added != 1) {
        throw std::logic_error("setup insertion failed");
      }
    }
  }
  index.force_merge();
  std::uint64_t rng = 0x1234'5678'abcd'0123ULL;
  const auto next = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  struct Change { std::size_t id; float input; };
  std::vector<Change> changes;
  changes.reserve(operations);
  for (std::size_t op = 0; op < operations; ++op) {
    const auto id = (shape == "insert" || shape == "erase") ? op : next() % count;
    const auto input = shape == "noop" ? scores[id] : shape == "increment" ? 1.0f :
        static_cast<float>(next() % (count * 2)) - static_cast<float>(count);
    changes.push_back({id, input});
    if (shape == "increment") scores[id] += input;
    else scores[id] = input;
  }
  const PackedZSetAddOptions options{.increment = shape == "increment"};
  const auto begin = std::chrono::steady_clock::now();
  for (const auto& change : changes) {
    if (shape == "churn" || shape == "erase") {
      const std::string_view member = members[change.id];
      if (index.remove(std::span(&member, 1)).removed != 1) {
        throw std::logic_error("removal failed");
      }
      if (shape == "erase") continue;
    }
    const PackedZSetAddItem item{change.input, members[change.id]};
    const auto result = index.add(std::span(&item, 1), options);
    if (result.invalid_member || result.invalid_score) {
      throw std::logic_error("update failed");
    }
  }
  const auto end = std::chrono::steady_clock::now();
  const auto expected_size = shape == "erase" ? 0 : count;
  if (index.size() != expected_size || !index.check_invariants()) {
    throw std::logic_error("invalid index");
  }
  const auto ordered = index.range_by_rank(0, -1, false);
  std::vector<bool> seen(count);
  std::uint64_t checksum = 0;
  for (const auto& entry : ordered) {
    if (entry.key < 0 || static_cast<std::size_t>(entry.key) >= count ||
        seen[entry.key] || entry.score != scores[entry.key]) {
      throw std::logic_error("incorrect final mapping");
    }
    seen[entry.key] = true;
    checksum = checksum * 131 + static_cast<std::uint32_t>(entry.key);
  }
  const auto ns = std::chrono::duration<double, std::nano>(end - begin).count();
  std::printf("shape\tmembers\toperations\tns_per_op\tallocated_bytes\tchecksum\n");
  // One churn operation is a remove followed by an add of the same member.
  std::printf("%s\t%zu\t%zu\t%.3f\t%zu\t%llu\n", shape.c_str(), expected_size,
              operations, ns / operations, index.allocated_bytes(),
              static_cast<unsigned long long>(checksum));
}
