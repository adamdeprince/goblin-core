// Isolate score-index scaling from protocol and replication overhead.
// Build: c++ -O3 -DNDEBUG -std=c++23 -Iinclude benchmarks/zset_tie_rescore.cpp -o /tmp/zset-tie-rescore
// Run:   /tmp/zset-tie-rescore [members=1000000] [operations=50000] [tied|bucketed|unique]
// To compare a preserved header, define GOBLIN_ZSET_SCORE_INDEX_HEADER to its path.
#ifndef GOBLIN_ZSET_SCORE_INDEX_HEADER
#define GOBLIN_ZSET_SCORE_INDEX_HEADER "goblin/core/zset_score_index.hpp"
#endif
#include GOBLIN_ZSET_SCORE_INDEX_HEADER

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  using namespace goblin::core;
  const std::size_t count = argc > 1 ? std::stoull(argv[1]) : 1000000;
  const std::size_t operations = argc > 2 ? std::stoull(argv[2]) : 50000;
  const std::string shape = argc > 3 ? argv[3] : "tied";
  if (count == 0 || count > 100000000 || operations == 0 ||
      (shape != "tied" && shape != "bucketed" && shape != "unique")) {
    throw std::invalid_argument("invalid size, operation count, or score shape");
  }
  ZSetMemberStorage members;
  std::vector<ZSetScoreEntry> ordered;
  members.reserve(count);
  ordered.reserve(count);
  for (std::uint32_t id = 0; id < count; ++id) {
    const double score = shape == "tied" ? 2 :
                         shape == "bucketed" ? id % 2048 : id + 0.25;
    const auto name = std::to_string(id);
    const auto stored_id = members.push_back(name, score);
    ordered.push_back({score, stored_id, zset_member_prefix(name)});
  }
  std::sort(ordered.begin(), ordered.end(), [&](auto a, auto b) {
    return a.score == b.score ? members.view(a.member_id) < members.view(b.member_id)
                              : a.score < b.score;
  });
  ZSetScoreIndex index(&members);
  index.assign_sorted(ordered);
  std::mt19937 rng(90210);
  std::vector<std::uint32_t> targets(operations);
  for (auto& id : targets) id = static_cast<std::uint32_t>(rng() % count);
  std::uint64_t checksum = 0;
  const auto begin = std::chrono::steady_clock::now();
  for (const auto id : targets) {
    const auto found = index.find_entry_location({members.score(id), id});
    if (!found) throw std::logic_error("lookup lost a member");
    checksum += found->first + found->second;
  }
  const auto located = std::chrono::steady_clock::now();
  for (const auto id : targets) {
    const auto old = members.score(id);
    const auto next = old + 1;
    if (!index.rescore({old, id}, {next, id})) {
      throw std::logic_error("rescore lost a member");
    }
    members.set_score(id, next);
  }
  const auto rescored = std::chrono::steady_clock::now();
  // Check the final logical mapping independently of the implementation's
  // layout validator. Report layout validity separately so preserved builds
  // with excessive capacity rounding can still be compared transparently.
  if (index.size() != count) throw std::logic_error("wrong final cardinality");
  const auto final = index.range(0, index.size());
  std::vector<bool> seen(count);
  for (std::size_t i = 0; i < final.size(); ++i) {
    const auto entry = final[i];
    if (entry.member_id >= count || seen[entry.member_id] ||
        entry.score != members.score(entry.member_id)) {
      throw std::logic_error("wrong final member/score mapping");
    }
    if (i > 0 && (final[i - 1].score > entry.score ||
        (final[i - 1].score == entry.score &&
         members.view(final[i - 1].member_id) >= members.view(entry.member_id)))) {
      throw std::logic_error("wrong final lexicographic score order");
    }
    seen[entry.member_id] = true;
  }
  const auto lookup_us = std::chrono::duration<double, std::micro>(located - begin).count();
  const auto rescore_us = std::chrono::duration<double, std::micro>(rescored - located).count();
  std::printf("members\toperations\tshape\tlookup_us_per_op\trescore_us_per_op\tblocks\tchecksum\tlayout_valid\n");
  std::printf("%zu\t%zu\t%s\t%.6f\t%.6f\t%zu\t%llu\t%d\n", count, operations,
              shape.c_str(), lookup_us / operations, rescore_us / operations,
              index.block_count(), static_cast<unsigned long long>(checksum),
              static_cast<int>(index.validate()));
}
