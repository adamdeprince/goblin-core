#include "goblin/core/zset_score_index.hpp"

#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace goblin::core;

struct Fixture {
  ZSetMemberStorage members;
  ZSetScoreIndex index;
  std::vector<double> scores;
  std::vector<bool> present;

  explicit Fixture(RankCacheMode mode)
      : index(&members, mode, 3, 64) {}

  void add(std::string name, double score) {
    const auto id = members.push_back(name, score);
    scores.push_back(score);
    present.push_back(true);
    index.insert({score, id});
  }

  void rescore(std::uint32_t id, double score) {
    const auto old = scores[id];
    members.set_score(id, score);
    try {
      assert(index.rescore({old, id}, {score, id}));
    } catch (...) {
      members.set_score(id, old);
      throw;
    }
    scores[id] = score;
  }

  void verify() {
    std::vector<ZSetScoreEntry> expected;
    for (std::uint32_t id = 0; id < scores.size(); ++id) {
      if (present[id]) expected.push_back({scores[id], id});
    }
    std::sort(expected.begin(), expected.end(), [&](auto a, auto b) {
      return a.score == b.score ? members.view(a.member_id) < members.view(b.member_id)
                                : a.score < b.score;
    });
    assert(index.validate());
    assert(index.size() == expected.size());
    const auto actual = index.range(0, index.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
      assert(actual[i].score == expected[i].score);
      assert(actual[i].member_id == expected[i].member_id);
      assert(index.rank(expected[i]) == i);
    }
  }
};

void test_tied_churn(RankCacheMode mode) {
  Fixture f(mode);
  std::mt19937 rng(98761);
  std::vector<unsigned> insertion(4096);
  std::iota(insertion.begin(), insertion.end(), 0);
  std::shuffle(insertion.begin(), insertion.end(), rng);
  for (auto n : insertion) {
    // Decimal text must stay lexicographic; shared prefixes and embedded NULs
    // exercise full member comparisons, including across block boundaries.
    auto name = n % 3 == 0 ? std::string("same\0", 5) + std::to_string(n)
                           : std::to_string(n);
    f.add(std::move(name), 2.0);
  }
  f.add("", 2.0);
  f.add(std::string(4, '\0'), 2.0);
  f.verify();

  for (unsigned step = 0; step < 24000; ++step) {
    const auto id = static_cast<std::uint32_t>(rng() % f.scores.size());
    if (step % 11 == 0) {
      if (f.present[id]) {
        assert(f.index.erase_one({f.scores[id], id}));
        f.present[id] = false;
      } else {
        f.index.insert({f.scores[id], id});
        f.present[id] = true;
      }
    } else if (f.present[id]) {
      const double candidates[] = {2, 3, 2, 3, -0.5, 100000,
                                    -std::numeric_limits<double>::infinity(),
                                    std::numeric_limits<double>::infinity()};
      f.rescore(id, candidates[rng() % 8]);
    }
    if (step % 2000 == 0) f.verify();
  }
  f.verify();
}

void test_source_draining(RankCacheMode mode) {
  Fixture f(mode);
  for (unsigned i = 0; i < 512; ++i) f.add(std::to_string(i), 2);
  const auto original_blocks = f.index.block_count();
  // Move the entire population in both directions through dense ties, forcing
  // destination splits, source merges, empty-block removal, and cache repairs.
  for (double score : {3.0, 1.0, 4.0, 2.0}) {
    for (std::uint32_t id = 0; id < f.scores.size(); ++id) {
      f.rescore(id, score);
      if (id % 64 == 0) f.verify();
    }
    f.verify();
    assert(f.index.block_count() <= original_blocks * 2);
  }
}

void test_rejected_rescore(RankCacheMode mode) {
  unsigned rejected = 0;
  unsigned accepted = 0;
  // Reject allocations at several points: destination growth, split metadata,
  // and the block-hint width transition. Every rejection must preserve order.
  for (const std::size_t allowance : {1U, 64U, 512U, 1024U, 2048U, 8192U}) {
    Fixture f(mode);
    for (unsigned i = 0; i < 192; ++i) f.add(std::to_string(i), i < 64 ? 1 : 2);
    MemoryCeiling ceiling(allowance);
    for (std::uint32_t id = 0; id < 64; ++id) {
      try {
        MemoryCeilingScope scope(&ceiling);
        f.rescore(id, 2);
        ++accepted;
      } catch (const MaxMemoryExceeded&) {
        ++rejected;
        assert(f.scores[id] == 1);
        assert(f.members.score(id) == 1);
      }
      f.verify();
    }
  }
  assert(rejected > 0 && accepted > 0);
}

void test_capacity_rounding_at_default_load() {
  ZSetMemberStorage members;
  ZSetScoreIndex index(&members);
  for (unsigned id = 0; id < 2 * ZSetScoreIndex::kDefaultLoad; ++id) {
    const auto stored = members.push_back(std::to_string(id), id);
    index.insert({static_cast<double>(id), stored});
    assert(index.validate());
  }
}
}  // namespace

int main() {
  test_capacity_rounding_at_default_load();
  for (auto mode : {RankCacheMode::Off, RankCacheMode::Exact,
                    RankCacheMode::BlockHint}) {
    test_tied_churn(mode);
    test_source_draining(mode);
    test_rejected_rescore(mode);
  }
}
