#undef NDEBUG
#include "goblin/core/packed_zset.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

namespace {

template <class Traits>
typename Traits::Key key_for(std::size_t id) {
  if constexpr (std::is_integral_v<typename Traits::Key>) {
    return static_cast<typename Traits::Key>(id) - 1000;
  } else {
    typename Traits::Key key;
    key.bytes[0] = static_cast<std::uint8_t>(id % 3);
    for (std::size_t byte = 0; byte < 4; ++byte) {
      key.bytes[15 - byte] = static_cast<std::uint8_t>(id >> (byte * 8));
    }
    return key;
  }
}

template <class Traits, class Score, bool Rle = false>
void redistribute_full_neighbor() {
  using Tree = goblin::core::detail::PackedBPlusTree<Traits, Score, Rle>;
  const auto capacity = Tree::kLeafCapacity;
  const auto count = capacity + capacity / 2;
  for (const bool reverse : {false, true}) {
    Tree tree(0.5);
    for (std::size_t i = 0; i < count; ++i) {
      const auto id = reverse ? count - i - 1 : i;
      tree.insert({static_cast<Score>(id), key_for<Traits>(id)});
    }
    tree.force_merge();
    assert(tree.leaf_count() == 2);
    const auto allocated = tree.allocated_bytes();
    for (std::size_t i = 0; i <= capacity / 4; ++i) {
      const auto id = reverse ? count - i - 1 : i;
      tree.erase({static_cast<Score>(id), key_for<Traits>(id)});
    }
    // An underfull leaf plus a full neighbor cannot coalesce: both directions
    // must redistribute. The raw layout does so without another allocation;
    // RLE may grow a score stream when the neighbor has more diverse scores.
    assert(tree.leaf_count() == 2 && tree.check_invariants());
    if constexpr (!Rle) assert(tree.allocated_bytes() == allocated);
    const auto entries = tree.range_by_rank(0, -1, false);
    assert(entries.size() == count - capacity / 4 - 1);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto id = reverse ? i : i + capacity / 4 + 1;
      assert(entries[i].score == static_cast<Score>(id));
      assert(entries[i].key == key_for<Traits>(id));
    }
  }
}

template <class Traits, class Score, bool Rle = false>
void exercise(double exponent) {
  using Tree = goblin::core::detail::PackedBPlusTree<Traits, Score, Rle>;
  using Entry = typename Tree::Entry;
  Tree tree(exponent);
  const auto capacity = Tree::kLeafCapacity;
  const auto count = capacity * 5 + 37;
  std::vector<std::optional<Score>> reference(count);
  const auto put = [&](std::size_t id, Score score) {
    const auto key = key_for<Traits>(id);
    if (reference[id]) tree.replace({*reference[id], key}, {score, key});
    else tree.insert({score, key});
    reference[id] = score;
  };
  const auto erase = [&](std::size_t id) {
    if (!reference[id]) return;
    tree.erase({*reference[id], key_for<Traits>(id)});
    reference[id].reset();
  };
  const auto verify = [&]() {
    const auto dirty = tree.dirty_size();
    assert(tree.check_invariants());
    std::vector<Entry> expected;
    for (std::size_t id = 0; id < count; ++id) {
      if (reference[id]) expected.push_back({*reference[id], key_for<Traits>(id)});
    }
    std::sort(expected.begin(), expected.end(), [](const Entry& a, const Entry& b) {
      return a.score < b.score ||
             (a.score == b.score && Traits::less(a.key, b.key));
    });
    const auto actual = tree.range_by_rank(0, -1, false);
    assert(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
      assert(actual[i].key == expected[i].key);
      assert(actual[i].score == expected[i].score);
    }
    if (!expected.empty()) {
      const auto middle = expected.size() / 2;
      assert(tree.rank(expected[middle], false) == middle);
      assert(tree.rank(expected[middle], true) == expected.size() - middle - 1);
      const auto score = static_cast<double>(expected[middle].score);
      const auto window = tree.range_by_score(score, false, score, false, false,
                                             0, std::nullopt);
      const auto matches = std::count_if(expected.begin(), expected.end(),
          [&](const Entry& entry) { return entry.score == expected[middle].score; });
      assert(window.size() == static_cast<std::size_t>(matches));
    }
    assert(tree.dirty_size() == dirty);  // Reads never force a merge.
  };

  for (std::size_t id = 0; id < capacity; ++id) put(id, static_cast<Score>(id));
  tree.force_merge();
  // First/last slots and both sides of a bitmap word, including partial words
  // in UUID layouts. Repeated rescores must invalidate the original slot once.
  for (const auto id : {std::size_t{0}, std::size_t{63}, std::size_t{64},
                        std::size_t{127}, capacity - 1}) {
    put(id, Score{-200});
    put(id, Score{200});
    put(id, static_cast<Score>(id));
    verify();
    erase(id);
    verify();
    put(id, static_cast<Score>(id));
    verify();
  }
  tree.force_merge();
  verify();

  for (std::size_t id = capacity; id < count; ++id) {
    put(id, static_cast<Score>(id % 17));
  }
  tree.force_merge();
  for (std::size_t id = 0; id < 32; ++id) {
    put(id, Score{-10000});
    put(id, Score{10000});
    put(id, static_cast<Score>(id % 17));
  }
  verify();
  // Copying a dirty tree must copy invalidation state as well as its tuples.
  auto copy = tree;
  copy.force_merge();
  assert(copy.check_invariants() && copy.size() == tree.size());

  std::uint64_t rng = 0x7812'8731'9102'3412ULL;
  const auto next = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  for (std::size_t op = 0; op < 6000; ++op) {
    const auto id = next() % count;
    if (next() % 5 == 0) erase(id);
    else put(id, static_cast<Score>(static_cast<int>(next() % 129) - 64));
    if (op % 127 == 0) verify();
    if (op % 997 == 0) tree.force_merge();
  }
  verify();

  // Drain from both directions to exercise redistribution, coalescing, root
  // collapse and reused leaf slots. Use exact ties and infinities on refill.
  for (std::size_t id = 0; id < count; ++id) {
    erase(id);
    if (id % 127 == 0) verify();
  }
  verify();
  for (std::size_t id = 0; id < count; ++id) {
    const auto score = id % 3 == 0 ? -std::numeric_limits<Score>::infinity() :
                       id % 3 == 1 ? Score{-0.0} :
                                     std::numeric_limits<Score>::infinity();
    put(id, score);
  }
  verify();
  // Equal live tuples do no work, including signed-zero and infinite scores.
  const auto dirty_before_noops = tree.dirty_size();
  for (std::size_t id = 0; id < count; ++id) {
    const auto score = *reference[id];
    put(id, score == Score{0} ? Score{0} : score);
  }
  assert(tree.dirty_size() == dirty_before_noops);
  verify();
  for (std::size_t id = count; id != 0;) {
    erase(--id);
    if (id % 127 == 0) verify();
  }
  verify();
}

template <class Traits, class Score, bool Rle = false>
void deep_rescore_routing() {
  using Tree = goblin::core::detail::PackedBPlusTree<Traits, Score, Rle>;
  using Entry = typename Tree::Entry;
  const auto capacity = Tree::kLeafCapacity;
  const auto count = capacity * 40 + 11;
  Tree tree(0.5);
  std::vector<Score> scores(count);
  for (std::size_t id = 0; id < count; ++id) {
    scores[id] = static_cast<Score>(id / 8);  // ties span leaf boundaries
    tree.insert({scores[id], key_for<Traits>(id)});
  }
  tree.force_merge();
  assert(tree.tree_height() >= 3);
  const auto put = [&](std::size_t id, Score score) {
    tree.replace({scores[id], key_for<Traits>(id)}, {score, key_for<Traits>(id)});
    scores[id] = score;
  };
  const auto verify = [&]() {
    assert(tree.check_invariants());
    std::vector<Entry> expected;
    for (std::size_t id = 0; id < count; ++id) {
      expected.push_back({scores[id], key_for<Traits>(id)});
    }
    std::sort(expected.begin(), expected.end(), [](const Entry& a, const Entry& b) {
      return a.score < b.score ||
             (a.score == b.score && Traits::less(a.key, b.key));
    });
    const auto actual = tree.range_by_rank(0, -1, false);
    assert(actual.size() == count);
    for (std::size_t i = 0; i < count; ++i) {
      assert(actual[i].score == expected[i].score && actual[i].key == expected[i].key);
    }
    for (const auto rank : {std::size_t{0}, capacity / 2 - 1, count / 4,
                            count / 2, count - 1}) {
      assert(tree.rank(expected[rank], false) == rank);
      assert(tree.rank(expected[rank], true) == count - rank - 1);
    }
  };
  // Retire boundary tuples without shrinking conservative routing fences,
  // then return members into those gaps and across either neighboring fence.
  for (std::size_t part = 1; part < 64; ++part) {
    const auto id = part * (capacity / 2) - 1;
    put(id, -static_cast<Score>(id + 1));
  }
  verify();
  for (std::size_t part = 1; part < 64; ++part) {
    const auto id = part * (capacity / 2) - 1;
    put(id, static_cast<Score>(id / 8) + Score{0.125});
  }
  verify();
  std::uint64_t rng = 0xe137'4890'abcd'0123ULL;
  for (std::size_t round = 0; round < 1600; ++round) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    const auto id = rng % count;
    Score score;
    switch (round % 6) {
      case 0: score = scores[id] + Score{0.125}; break;
      case 1: score = -static_cast<Score>(count + id); break;
      case 2: score = static_cast<Score>(count + id); break;
      case 3: score = Score{-0.0}; break;
      case 4: score = static_cast<Score>(id / 8); break;
      default: score = (id % 2 ? -1 : 1) * std::numeric_limits<Score>::infinity();
    }
    put(id, score);
    if (round % 199 == 0) verify();
  }
  verify();
  tree.force_merge();
  verify();
}

template <class Score>
void score_run_boundaries() {
  using Traits = goblin::core::detail::PackedIntegerTraits<std::int32_t>;
  using Tree = goblin::core::detail::PackedBPlusTree<Traits, Score, true>;
  using Raw = goblin::core::detail::PackedBPlusTree<Traits, Score>;
  Tree tree(0.0);
  for (int i = 0; i < 3; ++i) tree.insert({Score{7}, i});
  assert(tree.sorted_score_bytes() == 3 * sizeof(Score));
  assert(tree.compressed_leaf_count() == 0);
  tree.insert({Score{7}, 3});
  assert(tree.sorted_score_bytes() == 3 * sizeof(Score));
  assert(tree.compressed_leaf_count() == 1);
  tree.erase({Score{7}, 1});
  assert(tree.sorted_score_bytes() == 3 * sizeof(Score));
  assert(tree.compressed_leaf_count() == 0 && tree.check_invariants());

  Tree tied(0.5);
  Raw raw(0.5);
  const auto count = Tree::kLeafCapacity * 3;
  for (std::size_t i = 0; i < count; ++i) {
    tied.insert({Score{1}, static_cast<std::int32_t>(i)});
    raw.insert({Score{1}, static_cast<std::int32_t>(i)});
  }
  tied.force_merge();
  raw.force_merge();
  assert(tied.check_invariants());
  assert(tied.compressed_leaf_count() == tied.leaf_count());
  assert(tied.sorted_score_bytes() == tied.leaf_count() * 3 * sizeof(Score));
  assert(tied.allocated_bytes() < raw.allocated_bytes());
  // Mutate a copy, including dirty slots and score-stream growth.
  auto copy = tied;
  for (std::size_t i = 0; i < count; ++i) {
    copy.replace({Score{1}, static_cast<std::int32_t>(i)},
                 {static_cast<Score>(i + 2), static_cast<std::int32_t>(i)});
  }
  copy.force_merge();
  assert(copy.check_invariants() && tied.check_invariants());
  assert(copy.compressed_leaf_count() == 0);
  assert(copy.sorted_score_bytes() == count * sizeof(Score));
}

template <class Score>
void score_run_allocation_failures() {
  using namespace goblin::core;
  using Traits = detail::PackedIntegerTraits<std::int32_t>;
  using Tree = detail::PackedBPlusTree<Traits, Score, true>;
  const auto capacity = Tree::kLeafCapacity;
  MemoryCeiling deny_growth(1);
  deny_growth.bind(nullptr, [](const void*) noexcept { return std::size_t{1}; });

  // Break long runs into distinct scores until compaction needs more storage.
  // Exercise both a single leaf and moves between separate source/dest leaves.
  for (const bool cross_leaf : {false, true}) {
    Tree tree(0.5);
    const auto count = cross_leaf ? capacity * 2 + capacity / 4 : capacity / 2;
    for (std::size_t i = 0; i < count; ++i) {
      tree.insert({Score{1}, static_cast<std::int32_t>(i)});
    }
    tree.force_merge();
    std::size_t changed = 0;
    bool rejected = false;
    for (; changed < capacity / 2; ++changed) {
      MemoryCeilingScope scope(&deny_growth);
      try {
        tree.replace({Score{1}, static_cast<std::int32_t>(changed)},
                     {static_cast<Score>(changed + 2),
                      static_cast<std::int32_t>(changed)});
      } catch (const MaxMemoryExceeded&) {
        rejected = true;
        break;
      }
    }
    assert(rejected && changed != 0 && tree.check_invariants());
    assert(tree.size() == count);
    for (const auto& entry : tree.range_by_rank(0, -1, false)) {
      const auto id = static_cast<std::size_t>(entry.key);
      assert(entry.score == (id < changed ? static_cast<Score>(id + 2) : Score{1}));
    }
    // A refused write remains retryable with the original old tuple.
    tree.replace({Score{1}, static_cast<std::int32_t>(changed)},
                 {static_cast<Score>(changed + 2), static_cast<std::int32_t>(changed)});
    tree.force_merge();
    assert(tree.check_invariants());
  }

  // Redistribution from a diverse neighbor would expand a compressed leaf.
  // Denying that optional growth must not reject the already committed erase.
  Tree tree(0.5);
  const auto count = capacity + capacity / 2;
  for (std::size_t i = 0; i < count; ++i) {
    tree.insert({i < capacity / 2 ? Score{1} : static_cast<Score>(i + 2),
                 static_cast<std::int32_t>(i)});
  }
  tree.force_merge();
  assert(tree.leaf_count() == 2);
  {
    MemoryCeilingScope scope(&deny_growth);
    for (std::size_t i = 0; i <= capacity / 4; ++i) {
      tree.erase({Score{1}, static_cast<std::int32_t>(i)});
    }
  }
  assert(tree.size() == count - capacity / 4 - 1 && tree.check_invariants());
  tree.erase({Score{1}, static_cast<std::int32_t>(capacity / 4 + 1)});
  assert(tree.check_invariants());
}

}  // namespace

int main() {
  using namespace goblin::core::detail;
  score_run_boundaries<float>();
  score_run_boundaries<double>();
  score_run_allocation_failures<float>();
  score_run_allocation_failures<double>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int32_t>, float>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int32_t>, float, true>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int32_t>, double>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int32_t>, double, true>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int64_t>, float>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int64_t>, float, true>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int64_t>, double>();
  redistribute_full_neighbor<PackedIntegerTraits<std::int64_t>, double, true>();
  redistribute_full_neighbor<PackedUuidTraits, float>();
  redistribute_full_neighbor<PackedUuidTraits, float, true>();
  redistribute_full_neighbor<PackedUuidTraits, double>();
  redistribute_full_neighbor<PackedUuidTraits, double, true>();
  deep_rescore_routing<PackedIntegerTraits<std::int32_t>, float>();
  deep_rescore_routing<PackedIntegerTraits<std::int32_t>, float, true>();
  deep_rescore_routing<PackedIntegerTraits<std::int32_t>, double>();
  deep_rescore_routing<PackedIntegerTraits<std::int32_t>, double, true>();
  deep_rescore_routing<PackedIntegerTraits<std::int64_t>, float>();
  deep_rescore_routing<PackedIntegerTraits<std::int64_t>, float, true>();
  deep_rescore_routing<PackedIntegerTraits<std::int64_t>, double>();
  deep_rescore_routing<PackedIntegerTraits<std::int64_t>, double, true>();
  deep_rescore_routing<PackedUuidTraits, float>();
  deep_rescore_routing<PackedUuidTraits, float, true>();
  deep_rescore_routing<PackedUuidTraits, double>();
  deep_rescore_routing<PackedUuidTraits, double, true>();
  for (const auto exponent : {0.0, 0.5, 1.0}) {
    exercise<PackedIntegerTraits<std::int32_t>, float>(exponent);
    exercise<PackedIntegerTraits<std::int32_t>, float, true>(exponent);
    exercise<PackedIntegerTraits<std::int32_t>, double>(exponent);
    exercise<PackedIntegerTraits<std::int32_t>, double, true>(exponent);
    exercise<PackedIntegerTraits<std::int64_t>, float>(exponent);
    exercise<PackedIntegerTraits<std::int64_t>, float, true>(exponent);
    exercise<PackedIntegerTraits<std::int64_t>, double>(exponent);
    exercise<PackedIntegerTraits<std::int64_t>, double, true>(exponent);
    exercise<PackedUuidTraits, float>(exponent);
    exercise<PackedUuidTraits, float, true>(exponent);
    exercise<PackedUuidTraits, double>(exponent);
    exercise<PackedUuidTraits, double, true>(exponent);
  }
}
