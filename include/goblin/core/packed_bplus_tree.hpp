#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "goblin/core/memory_limit.hpp"
#include "goblin/core/packed_score_runs.hpp"

namespace goblin::core::detail {

// A high-fanout, arena-indexed B+ tree for fixed-width zset tuples. Leaves keep
// a sorted base and a small key-sorted dirty tail. Updating the same key in one
// leaf overwrites its dirty record; reaching the local threshold compacts only
// that leaf. Branch and leaf references are 32-bit arena indices, never heap
// pointers.
template <class Traits, class Score, bool ScoreRle = false>
class PackedBPlusTree {
 public:
  using Key = typename Traits::Key;

  struct Entry {
    Score score{};
    Key key{};
  };

  static_assert(std::is_trivially_copyable_v<Entry>);

  static constexpr std::size_t kTargetLeafBytes = 4096;
  static constexpr std::size_t kLeafCapacity =
      std::max<std::size_t>(32, kTargetLeafBytes / sizeof(Entry));
  static constexpr std::size_t kBranchCapacity = 64;

  explicit PackedBPlusTree(double merge_exponent)
      : merge_exponent_(merge_exponent),
        merge_threshold_(threshold_for(merge_exponent)) {}

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t dirty_size() const noexcept {
    return dirty_count_;
  }
  [[nodiscard]] std::size_t entry_count() const noexcept {
    return sorted_count_ + dirty_count_;
  }
  [[nodiscard]] std::size_t sorted_entry_count() const noexcept {
    return sorted_count_;
  }
  [[nodiscard]] std::size_t merge_threshold() const noexcept {
    return merge_threshold_;
  }
  [[nodiscard]] std::size_t leaf_capacity() const noexcept {
    return kLeafCapacity;
  }
  [[nodiscard]] std::size_t leaf_count() const noexcept {
    return active_leaf_count_;
  }
  [[nodiscard]] std::size_t branch_count() const noexcept {
    return active_branch_count_;
  }
  [[nodiscard]] std::size_t tree_height() const noexcept {
    return root_ == kNull ? 0 : height_ + 1;
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    std::size_t bytes = leaves_.capacity() * sizeof(Leaf) +
                        branches_.capacity() * sizeof(Branch);
    for (const auto& leaf : leaves_) {
      if constexpr (ScoreRle) bytes += leaf.entries.allocated_bytes();
      else bytes += leaf.entries.capacity() * sizeof(Entry);
    }
    return bytes;
  }

  [[nodiscard]] static constexpr bool score_rle_enabled() noexcept {
    return ScoreRle;
  }
  [[nodiscard]] std::size_t compressed_leaf_count() const noexcept {
    std::size_t count = 0;
    if constexpr (ScoreRle) {
      for (const auto& leaf : leaves_) {
        count += leaf.active && leaf.entries.encoded;
      }
    }
    return count;
  }
  [[nodiscard]] std::size_t sorted_score_bytes() const noexcept {
    if constexpr (!ScoreRle) return sorted_count_ * sizeof(Score);
    else {
      std::size_t bytes = 0;
      for (const auto& leaf : leaves_) {
        if (leaf.active) bytes += leaf.entries.scores.size() * sizeof(Score);
      }
      return bytes;
    }
  }

  void insert(Entry entry) {
    if (root_ == kNull) {
      const auto leaf_id = allocate_leaf();
      auto& leaf = leaves_[leaf_id];
      leaf.fence = entry;
      leaf.has_fence = true;
      leaf.live_count = 1;
      set_dirty(leaf, entry.key, entry.score);
      root_ = leaf_id;
      first_leaf_ = leaf_id;
      last_leaf_ = leaf_id;
      active_leaf_count_ = 1;
      size_ = 1;
      maybe_compact_leaf(leaf_id);
      return;
    }

    auto path = locate(entry);
    if (leaves_[path.leaf].live_count >= kLeafCapacity) {
      split_leaf(path);
      path = locate(entry);
    }

    auto& leaf = leaves_[path.leaf];
    prepare_dirty(leaf);
    ++leaf.live_count;
    ++size_;
    set_dirty(leaf, entry.key, entry.score);
    expand_fence(path, entry);
    adjust_path_count(path, 1);
    maybe_compact_leaf(path.leaf);
  }

  void replace(Entry old_entry, Entry new_entry) {
    if (equivalent(old_entry, new_entry)) return;
    assert(root_ != kNull);

    auto old_path = locate(old_entry);
    auto new_path = locate(new_entry);
    if (old_path.leaf == new_path.leaf) {
      auto& leaf = leaves_[old_path.leaf];
      prepare_dirty(leaf);
      set_dirty(leaf, new_entry.key, new_entry.score, old_entry);
      expand_fence(old_path, new_entry);
      maybe_compact_leaf(old_path.leaf);
      return;
    }

    if (leaves_[new_path.leaf].live_count >= kLeafCapacity) {
      split_leaf(new_path);
      old_path = locate(old_entry);
      new_path = locate(new_entry);
    }

    auto& old_leaf = leaves_[old_path.leaf];
    auto& new_leaf = leaves_[new_path.leaf];
    prepare_dirty(old_leaf);
    prepare_dirty(new_leaf);
    --old_leaf.live_count;
    ++new_leaf.live_count;
    set_dirty(old_leaf, old_entry.key, std::nullopt, old_entry);
    set_dirty(new_leaf, new_entry.key, new_entry.score);
    adjust_path_count(old_path, -1);
    expand_fence(new_path, new_entry);
    adjust_path_count(new_path, 1);
    maybe_compact_leaf(old_path.leaf);
    maybe_compact_leaf(new_path.leaf);
    maybe_rebalance_leaf(old_path.leaf);
  }

  void erase(Entry old_entry) {
    assert(root_ != kNull && size_ != 0);
    const auto path = locate(old_entry);
    auto& leaf = leaves_[path.leaf];
    prepare_dirty(leaf);
    --leaf.live_count;
    --size_;
    set_dirty(leaf, old_entry.key, std::nullopt, old_entry);
    adjust_path_count(path, -1);

    if (size_ == 0) {
      clear();
      return;
    }
    maybe_compact_leaf(path.leaf);
    maybe_rebalance_leaf(path.leaf);
  }

  [[nodiscard]] std::optional<std::size_t> rank(Entry entry,
                                                 bool reverse) const {
    if (root_ == kNull) return std::nullopt;
    const auto path = locate(entry);
    std::size_t before = 0;
    for (std::size_t depth = 0; depth < path.depth; ++depth) {
      const auto& branch = branches_[path.nodes[depth]];
      for (std::size_t child = 0; child < path.slots[depth]; ++child) {
        before += branch.counts[child];
      }
    }

    std::size_t local = 0;
    bool found = false;
    for_each_leaf_entry(path.leaf, [&](const Entry& candidate) {
      if (candidate.key == entry.key) {
        found = true;
        return false;
      }
      ++local;
      return true;
    });
    if (!found) return std::nullopt;
    const auto forward = before + local;
    return reverse ? size_ - forward - 1 : forward;
  }

  [[nodiscard]] std::vector<Entry> range_by_rank(long long start,
                                                  long long stop,
                                                  bool reverse) const {
    std::vector<Entry> result;
    const auto bounds = normalize_rank_range(size_, start, stop);
    if (!bounds) return result;
    auto [first, count] = *bounds;
    if (reverse) first = size_ - first - count;
    reserve_memory_vector(result, count);

    auto position = locate_rank(first);
    auto leaf_id = position.leaf;
    auto skip = position.offset;
    while (leaf_id != kNull && result.size() < count) {
      for_each_leaf_entry(leaf_id, [&](const Entry& entry) {
        if (skip != 0) {
          --skip;
          return true;
        }
        result.push_back(entry);
        return result.size() < count;
      });
      leaf_id = leaves_[leaf_id].next;
    }
    if (reverse) std::reverse(result.begin(), result.end());
    return result;
  }

  [[nodiscard]] std::vector<Entry> range_by_score(
      double min, bool min_exclusive, double max, bool max_exclusive,
      bool reverse, std::size_t offset,
      std::optional<std::size_t> limit) const {
    std::vector<Entry> result;
    if (root_ == kNull || std::isnan(min) || std::isnan(max) || min > max ||
        (min == max && (min_exclusive || max_exclusive))) {
      return result;
    }
    if (limit && *limit == 0) return result;

    auto leaf_id = first_leaf_for_score(min, min_exclusive);
    std::size_t skipped = 0;
    bool done = false;
    while (leaf_id != kNull && !done) {
      const auto& leaf = leaves_[leaf_id];
      for_each_leaf_entry(leaf_id, [&](const Entry& entry) {
        const auto score = static_cast<double>(entry.score);
        const bool above = min_exclusive ? score > min : score >= min;
        if (!above) return true;
        const bool below = max_exclusive ? score < max : score <= max;
        if (!below) {
          done = true;
          return false;
        }
        if (!reverse && skipped++ < offset) return true;
        reserve_memory_vector_for_push(result, 64);
        result.push_back(entry);
        if (!reverse && limit && result.size() >= *limit) {
          done = true;
          return false;
        }
        return true;
      });
      if (done) break;
      const auto fence_score = static_cast<double>(leaf.fence.score);
      if ((max_exclusive && fence_score >= max) ||
          (!max_exclusive && fence_score > max)) {
        break;
      }
      leaf_id = leaf.next;
    }

    if (reverse) {
      std::reverse(result.begin(), result.end());
      if (offset >= result.size()) {
        result.clear();
        return result;
      }
      if (offset != 0) {
        result.erase(result.begin(),
                     result.begin() + static_cast<std::ptrdiff_t>(offset));
      }
      if (limit && result.size() > *limit) result.resize(*limit);
    }
    return result;
  }

  void force_merge() {
    for (auto leaf = first_leaf_; leaf != kNull; leaf = leaves_[leaf].next) {
      compact_leaf(leaf);
    }
  }

  [[nodiscard]] bool check_invariants() const {
    if (root_ == kNull) {
      return size_ == 0 && active_leaf_count_ == 0 && first_leaf_ == kNull &&
             last_leaf_ == kNull && sorted_count_ == 0 && dirty_count_ == 0;
    }
    if (size_ == 0 || first_leaf_ == kNull || last_leaf_ == kNull) return false;

    std::size_t leaves_seen = 0;
    std::size_t live_seen = 0;
    std::size_t sorted_seen = 0;
    std::size_t dirty_seen = 0;
    std::uint32_t previous = kNull;
    std::optional<Entry> previous_fence;
    std::optional<Entry> previous_entry;
    for (auto leaf_id = first_leaf_; leaf_id != kNull;
         leaf_id = leaves_[leaf_id].next) {
      if (leaf_id >= leaves_.size()) return false;
      const auto& leaf = leaves_[leaf_id];
      if (!leaf.active || leaf.prev != previous || !leaf.has_fence ||
          leaf.sorted_size + tail_size(leaf) > kLeafCapacity + merge_threshold_) {
        return false;
      }
      if constexpr (ScoreRle) {
        if (!leaf.entries.check_invariants(leaf.sorted_size) ||
            tail_size(leaf) >= merge_threshold_) return false;
      } else if (leaf.sorted_size > leaf.entries.size()) return false;
      if (previous_fence && !less(*previous_fence, leaf.fence)) return false;
      previous_fence = leaf.fence;
      for (std::size_t i = 1; i < leaf.sorted_size; ++i) {
        if (less(base_entry(leaf, i), base_entry(leaf, i - 1))) return false;
      }
      for (std::size_t i = 0; i < kLeafCapacity; ++i) {
        const bool shadowed = i < leaf.sorted_size &&
                              tail_contains(leaf, base_entry(leaf, i).key);
        if (base_invalidated(leaf, i) != shadowed) return false;
      }
      const auto dirty_begin = tail_begin(leaf);
      for (std::size_t i = 1; i < tail_size(leaf); ++i) {
        if (!Traits::less(dirty_begin[i - 1].key, dirty_begin[i].key)) {
          return false;
        }
      }
      std::size_t local_live = 0;
      bool local_ok = true;
      for_each_leaf_entry(leaf_id, [&](const Entry& entry) {
        if (previous_entry && !less(*previous_entry, entry)) local_ok = false;
        if (less(leaf.fence, entry)) local_ok = false;
        previous_entry = entry;
        ++local_live;
        return true;
      });
      if (!local_ok || local_live != leaf.live_count) return false;
      live_seen += local_live;
      sorted_seen += leaf.sorted_size;
      dirty_seen += tail_size(leaf);
      previous = leaf_id;
      ++leaves_seen;
    }
    if (previous != last_leaf_ || leaves_seen != active_leaf_count_ ||
        live_seen != size_ || sorted_seen != sorted_count_ ||
        dirty_seen != dirty_count_) {
      return false;
    }

    std::size_t tree_total = 0;
    Entry tree_fence{};
    if (!validate_subtree(root_, height_, tree_total, tree_fence) ||
        tree_total != size_ || !equivalent(tree_fence, leaves_[last_leaf_].fence)) {
      return false;
    }
    return true;
  }

 private:
  static constexpr std::uint32_t kNull =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::size_t kMaxHeight = 16;

  using RleEntries = PackedScoreRuns<Entry, kLeafCapacity>;
  struct Leaf {
    std::conditional_t<ScoreRle, RleEntries, std::vector<Entry>> entries;
    // Base tuples stay unchanged until compaction, so their slots are stable.
    // One bit replaces a dirty-tail key search per base tuple on merges/reads.
    std::array<std::uint64_t, (kLeafCapacity + 63) / 64> invalidated{};
    Entry fence{};
    std::size_t sorted_size{0};
    std::size_t live_count{0};
    std::uint32_t next{kNull};
    std::uint32_t prev{kNull};
    std::uint32_t next_free{kNull};
    bool has_fence{false};
    bool active{false};
  };

  struct Branch {
    std::array<Entry, kBranchCapacity + 1> maxes{};
    std::array<std::uint32_t, kBranchCapacity + 1> children{};
    std::array<std::size_t, kBranchCapacity + 1> counts{};
    std::size_t total{0};
    std::uint32_t next_free{kNull};
    std::uint16_t count{0};
    bool active{false};
  };

  struct Path {
    std::array<std::uint32_t, kMaxHeight> nodes{};
    std::array<std::uint16_t, kMaxHeight> slots{};
    std::size_t depth{0};
    std::uint32_t leaf{kNull};
  };

  struct RankPosition {
    std::uint32_t leaf{kNull};
    std::size_t offset{0};
  };

  // Construct only the live delta slots. A union keeps the unused stack slots
  // inactive even when Entry/Key have default member initializers (UUIDs do).
  // Trivial union copies used by sort preserve the active Entry's lifetime.
  union MergeSlot {
    Entry entry;
    MergeSlot() noexcept {}
  };
  static_assert(std::is_trivially_copyable_v<MergeSlot>);

  [[nodiscard]] static std::size_t threshold_for(double exponent) noexcept {
    if (!(exponent >= 0.0 && exponent <= 1.0)) return 1;
    if (exponent == 0.0) return 1;
    if (exponent == 1.0) return kLeafCapacity;
    const auto threshold = std::ceil(
        exponent == 0.5
            ? std::sqrt(static_cast<double>(kLeafCapacity))
            : std::pow(static_cast<double>(kLeafCapacity), exponent));
    return std::clamp<std::size_t>(static_cast<std::size_t>(threshold), 1,
                                   kLeafCapacity);
  }

  [[nodiscard]] static bool less(const Entry& lhs,
                                 const Entry& rhs) noexcept {
    if (lhs.score < rhs.score) return true;
    if (rhs.score < lhs.score) return false;
    return Traits::less(lhs.key, rhs.key);
  }

  [[nodiscard]] static bool equivalent(const Entry& lhs,
                                       const Entry& rhs) noexcept {
    // Live tuples/fences have no NaN, so direct equality also handles +/-0.
    return lhs.score == rhs.score && lhs.key == rhs.key;
  }

  [[nodiscard]] static bool key_less(const Entry& lhs,
                                     const Key& rhs) noexcept {
    return Traits::less(lhs.key, rhs);
  }

  [[nodiscard]] static bool key_less(const Key& lhs,
                                     const Entry& rhs) noexcept {
    return Traits::less(lhs, rhs.key);
  }

  [[nodiscard]] std::uint32_t allocate_leaf() {
    Leaf prepared;
    if constexpr (ScoreRle) prepared.entries.reserve(merge_threshold_);
    else reserve_memory_vector(prepared.entries,
                               kLeafCapacity + merge_threshold_);
    std::uint32_t id = kNull;
    if (free_leaf_ != kNull) {
      id = free_leaf_;
      free_leaf_ = leaves_[id].next_free;
      leaves_[id] = std::move(prepared);
    } else {
      if (leaves_.size() >= kNull) {
        throw std::length_error("packed zset leaf arena exhausted");
      }
      reserve_memory_vector_for_push(leaves_, 8);
      id = static_cast<std::uint32_t>(leaves_.size());
      leaves_.push_back(std::move(prepared));
    }
    leaves_[id].active = true;
    return id;
  }

  void release_leaf(std::uint32_t id) noexcept {
    auto& leaf = leaves_[id];
    leaf.entries.clear();
    leaf.invalidated.fill(0);
    leaf.sorted_size = 0;
    leaf.live_count = 0;
    leaf.next = kNull;
    leaf.prev = kNull;
    leaf.has_fence = false;
    leaf.active = false;
    leaf.next_free = free_leaf_;
    free_leaf_ = id;
  }

  [[nodiscard]] std::uint32_t allocate_branch() {
    std::uint32_t id = kNull;
    if (free_branch_ != kNull) {
      id = free_branch_;
      free_branch_ = branches_[id].next_free;
      branches_[id] = Branch{};
    } else {
      if (branches_.size() >= kNull) {
        throw std::length_error("packed zset branch arena exhausted");
      }
      reserve_memory_vector_for_push(branches_, 8);
      id = static_cast<std::uint32_t>(branches_.size());
      branches_.push_back(Branch{});
    }
    branches_[id].active = true;
    ++active_branch_count_;
    return id;
  }

  void release_branch(std::uint32_t id) noexcept {
    auto& branch = branches_[id];
    assert(branch.active && active_branch_count_ != 0);
    branch.active = false;
    branch.count = 0;
    branch.total = 0;
    branch.next_free = free_branch_;
    free_branch_ = id;
    --active_branch_count_;
  }

  [[nodiscard]] Path locate(const Entry& entry) const {
    assert(root_ != kNull);
    Path path;
    auto node = root_;
    auto level = height_;
    while (level != 0) {
      assert(path.depth < kMaxHeight);
      const auto& branch = branches_[node];
      auto found = std::lower_bound(
          branch.maxes.begin(), branch.maxes.begin() + branch.count, entry,
          [](const Entry& fence, const Entry& value) {
            return less(fence, value);
          });
      auto slot = static_cast<std::size_t>(found - branch.maxes.begin());
      if (slot == branch.count) slot = branch.count - 1;
      path.nodes[path.depth] = node;
      path.slots[path.depth] = static_cast<std::uint16_t>(slot);
      ++path.depth;
      node = branch.children[slot];
      --level;
    }
    path.leaf = node;
    return path;
  }

  [[nodiscard]] std::uint32_t first_leaf_for_score(
      double score, bool exclusive) const noexcept {
    if (root_ == kNull) return kNull;
    auto node = root_;
    auto level = height_;
    while (level != 0) {
      const auto& branch = branches_[node];
      std::size_t slot = 0;
      while (slot < branch.count) {
        const auto fence_score = static_cast<double>(branch.maxes[slot].score);
        if (exclusive ? fence_score > score : fence_score >= score) break;
        ++slot;
      }
      if (slot == branch.count) return kNull;
      node = branch.children[slot];
      --level;
    }
    return node;
  }

  [[nodiscard]] RankPosition locate_rank(std::size_t rank) const noexcept {
    assert(rank < size_ && root_ != kNull);
    auto node = root_;
    auto level = height_;
    while (level != 0) {
      const auto& branch = branches_[node];
      std::size_t slot = 0;
      while (slot < branch.count && rank >= branch.counts[slot]) {
        rank -= branch.counts[slot++];
      }
      assert(slot < branch.count);
      node = branch.children[slot];
      --level;
    }
    return {.leaf = node, .offset = rank};
  }

  [[nodiscard]] static auto tail_begin(auto& leaf) {
    if constexpr (ScoreRle) return leaf.entries.dirty.begin();
    else return leaf.entries.begin() +
                static_cast<std::ptrdiff_t>(leaf.sorted_size);
  }
  [[nodiscard]] static auto tail_end(auto& leaf) {
    if constexpr (ScoreRle) return leaf.entries.dirty.end();
    else return leaf.entries.end();
  }
  [[nodiscard]] static std::size_t tail_size(const Leaf& leaf) noexcept {
    return static_cast<std::size_t>(tail_end(leaf) - tail_begin(leaf));
  }
  [[nodiscard]] static Entry base_entry(const Leaf& leaf,
                                         std::size_t slot) noexcept {
    if constexpr (ScoreRle) return leaf.entries.entry(slot);
    else return leaf.entries[slot];
  }
  [[nodiscard]] static auto tail_lower_bound(auto& leaf, const Key& key) {
    return std::lower_bound(tail_begin(leaf), tail_end(leaf), key,
        [](const Entry& entry, const Key& candidate) {
          return key_less(entry, candidate);
        });
  }
  [[nodiscard]] static bool tail_contains(const Leaf& leaf,
                                          const Key& key) noexcept {
    const auto found = tail_lower_bound(leaf, key);
    return found != tail_end(leaf) && found->key == key;
  }

  void prepare_dirty(Leaf& leaf) {
    if constexpr (ScoreRle) {
      if (tail_size(leaf) + 1 >= merge_threshold_) {
        leaf.entries.prepare_merge(tail_size(leaf) + 1);
      }
    }
  }

  [[nodiscard]] static bool base_invalidated(const Leaf& leaf,
                                               std::size_t slot) noexcept {
    return (leaf.invalidated[slot / 64] & (std::uint64_t{1} << (slot % 64))) != 0;
  }

  void set_dirty(Leaf& leaf, const Key& key,
                 std::optional<Score> score,
                 std::optional<Entry> retired = std::nullopt) {
    auto found = tail_lower_bound(leaf, key);
    const auto stored = score.value_or(std::numeric_limits<Score>::quiet_NaN());
    if (found != tail_end(leaf) && found->key == key) {
      found->score = stored;
      return;
    }
    if (retired) {
      // Only the first dirty record invalidates a base slot. Repeated updates,
      // deletes and reinsertions retain that bit until the leaf is compacted.
      std::size_t slot;
      if constexpr (ScoreRle) {
        std::size_t first = 0;
        std::size_t end = leaf.sorted_size;
        while (first < end) {
          const auto middle = first + (end - first) / 2;
          if (less(base_entry(leaf, middle), *retired)) first = middle + 1;
          else end = middle;
        }
        slot = first;
      } else {
        const auto base_end = leaf.entries.begin() +
                              static_cast<std::ptrdiff_t>(leaf.sorted_size);
        const auto base = std::lower_bound(leaf.entries.begin(), base_end, *retired,
            [](const Entry& a, const Entry& b) { return less(a, b); });
        slot = static_cast<std::size_t>(base - leaf.entries.begin());
      }
      assert(slot < leaf.sorted_size &&
             equivalent(base_entry(leaf, slot), *retired));
      leaf.invalidated[slot / 64] |= std::uint64_t{1} << (slot % 64);
    }
    if constexpr (ScoreRle) {
      assert(leaf.entries.dirty.size() < leaf.entries.dirty.capacity());
      leaf.entries.dirty.insert(found, Entry{stored, key});
    } else {
      assert(leaf.entries.size() < leaf.entries.capacity());
      leaf.entries.insert(found, Entry{stored, key});
    }
    ++dirty_count_;
  }

  [[nodiscard]] static std::size_t collect_additions(
      const Leaf& leaf, std::array<MergeSlot, kLeafCapacity>& additions) {
    std::size_t addition_count = 0;
    for (auto it = tail_begin(leaf); it != tail_end(leaf); ++it) {
      if (!std::isnan(it->score)) {
        std::construct_at(&additions[addition_count++].entry, *it);
      }
    }
    std::sort(additions.begin(), additions.begin() +
                                     static_cast<std::ptrdiff_t>(addition_count),
              [](const MergeSlot& lhs, const MergeSlot& rhs) {
                return less(lhs.entry, rhs.entry);
              });
    return addition_count;
  }

  template <class Fn>
  bool for_each_leaf_entry(std::uint32_t leaf_id, Fn&& fn) const {
    const auto& leaf = leaves_[leaf_id];
    std::array<MergeSlot, kLeafCapacity> additions;
    const auto addition_count = collect_additions(leaf, additions);

    struct RawCursor {
      const std::vector<Entry>& entries;
      Entry entry(std::size_t slot) const { return entries[slot]; }
    };
    using Cursor = std::conditional_t<ScoreRle, typename RleEntries::Cursor,
                                      RawCursor>;
    Cursor cursor{leaf.entries};
    std::size_t base = 0;
    std::size_t addition = 0;
    const auto next_base = [&]() {
      while (base < leaf.sorted_size &&
             base_invalidated(leaf, base)) {
        ++base;
      }
    };
    next_base();
    while (base < leaf.sorted_size || addition < addition_count) {
      const auto candidate = base < leaf.sorted_size ? cursor.entry(base) : Entry{};
      const bool use_base =
          addition == addition_count ||
          (base < leaf.sorted_size && less(candidate, additions[addition].entry));
      if (use_base) {
        ++base;
        if (!fn(candidate)) return false;
        next_base();
      } else {
        if (!fn(additions[addition++].entry)) return false;
      }
    }
    return true;
  }

  void maybe_compact_leaf(std::uint32_t leaf_id) {
    const auto& leaf = leaves_[leaf_id];
    if (tail_size(leaf) >= merge_threshold_) {
      compact_leaf(leaf_id);
    }
  }

  void assign_rle_base(Leaf& leaf, std::span<const Entry> values) noexcept {
    sorted_count_ = sorted_count_ - leaf.sorted_size + values.size();
    dirty_count_ -= tail_size(leaf);
    leaf.entries.assign(values);
    leaf.sorted_size = values.size();
    leaf.invalidated.fill(0);
  }

  void compact_rle_leaf(std::uint32_t leaf_id) {
    auto& leaf = leaves_[leaf_id];
    if (tail_size(leaf) != 0) {
      std::array<Entry, kLeafCapacity> values;
      std::size_t count = 0;
      for_each_leaf_entry(leaf_id, [&](const Entry& entry) {
        assert(count < values.size());
        values[count++] = entry;
        return true;
      });
      const std::span<const Entry> live(values.data(), count);
      leaf.entries.reserve_words(RleEntries::word_count(live));
      assign_rle_base(leaf, live);
      assert(count == leaf.live_count);
    }
    leaf.entries.maybe_shrink(merge_threshold_);
  }

  void rebalance_rle_leaves(std::uint32_t left_id, std::uint32_t right_id) {
    auto& left = leaves_[left_id];
    auto& right = leaves_[right_id];
    const auto left_old = left.live_count;
    const auto right_old = right.live_count;
    const auto combined = left_old + right_old;
    const auto left_new = combined > kLeafCapacity ? combined / 2 : combined;
    const auto right_new = combined - left_new;
    std::array<Entry, kLeafCapacity * 2> values;
    std::size_t count = 0;
    for (const auto id : {left_id, right_id}) {
      for_each_leaf_entry(id, [&](const Entry& entry) {
        values[count++] = entry;
        return true;
      });
    }
    assert(count == combined);
    const std::span<const Entry> all(values.data(), count);
    try {
      left.entries.reserve_words(RleEntries::word_count(all.first(left_new)));
      right.entries.reserve_words(RleEntries::word_count(all.subspan(left_new)));
    } catch (const std::bad_alloc&) {
      // This is optional maintenance after a committed deletion/rescore.
      // Both old leaves, their dirty records, and all counts remain valid.
      return;
    }
    const auto left_path = locate(left.fence);
    const auto right_path = locate(right.fence);
    assign_rle_base(left, all.first(left_new));
    assign_rle_base(right, all.subspan(left_new));
    left.live_count = left_new;
    right.live_count = right_new;
    if (right_new != 0) {
      left.fence = values[left_new - 1];
      refresh_path(left_path, static_cast<std::ptrdiff_t>(left_new) -
                                   static_cast<std::ptrdiff_t>(left_old));
      refresh_path(right_path, static_cast<std::ptrdiff_t>(right_new) -
                                    static_cast<std::ptrdiff_t>(right_old));
    } else {
      left.fence = right.fence;
      left.next = right.next;
      if (right.next != kNull) leaves_[right.next].prev = left_id;
      if (last_leaf_ == right_id) last_leaf_ = left_id;
      refresh_path(left_path, static_cast<std::ptrdiff_t>(right_old));
      remove_child(right_path, right_old);
      release_leaf(right_id);
      --active_leaf_count_;
    }
    left.entries.maybe_shrink(merge_threshold_);
    right.entries.maybe_shrink(merge_threshold_);
    if (left.live_count < kLeafCapacity / 4 && active_leaf_count_ > 1) {
      maybe_rebalance_leaf(left_id);
    }
  }

  void compact_leaf(std::uint32_t leaf_id) {
    if constexpr (ScoreRle) {
      compact_rle_leaf(leaf_id);
    } else {
      auto& leaf = leaves_[leaf_id];
      const auto tail_size = leaf.entries.size() - leaf.sorted_size;
      if (tail_size == 0) return;

      std::array<MergeSlot, kLeafCapacity> additions;
      const auto addition_count = collect_additions(leaf, additions);

      const auto old_sorted = leaf.sorted_size;
      std::size_t prefix = 0;
      std::size_t source = 0;
      const auto copy_run = [&](std::size_t end) {
        const auto length = end - source;
        if (length != 0 && prefix != source) {
          std::memmove(leaf.entries.data() + prefix, leaf.entries.data() + source,
                       length * sizeof(Entry));
        }
        prefix += length;
      };
      // Visit only the holes. The unchanged prefix needs no writes, and each
      // surviving run after it is compacted with one overlapping block move.
      for (std::size_t word = 0; word < leaf.invalidated.size(); ++word) {
        auto holes = leaf.invalidated[word];
        while (holes != 0) {
          const auto hole = word * 64 + std::countr_zero(holes);
          assert(hole < old_sorted);
          copy_run(hole);
          source = hole + 1;
          holes &= holes - 1;
        }
      }
      copy_run(old_sorted);
      const auto final_size = prefix + addition_count;
      leaf.entries.resize(final_size);

      if (addition_count != 0) {
        if (prefix == 0 || less(leaf.entries[prefix - 1], additions[0].entry)) {
          // An ordered append (including an empty base) never moves the base.
          for (std::size_t i = 0; i < addition_count; ++i) {
            leaf.entries[prefix + i] = additions[i].entry;
          }
        } else {
          std::size_t left = prefix;
          std::size_t right = addition_count;
          std::size_t output = final_size;
          while (right != 0) {
            const auto& addition = additions[right - 1].entry;
            if (left != 0 && less(addition, leaf.entries[left - 1])) {
              // Find the run from its right edge. Exponential probes make short
              // runs cheap, while long runs still use logarithmic comparisons.
              auto start = left - 1;
              std::size_t stride = 1;
              while (start != 0) {
                const auto probe = start > stride ? start - stride : 0;
                if (!less(addition, leaf.entries[probe])) {
                  const auto first = std::upper_bound(
                      leaf.entries.begin() + static_cast<std::ptrdiff_t>(probe + 1),
                      leaf.entries.begin() + static_cast<std::ptrdiff_t>(start),
                      addition,
                      [](const Entry& a, const Entry& b) { return less(a, b); });
                  start = static_cast<std::size_t>(first - leaf.entries.begin());
                  break;
                }
                start = probe;
                stride *= 2;
              }
              const auto length = left - start;
              output -= length;
              std::memmove(leaf.entries.data() + output, leaf.entries.data() + start,
                           length * sizeof(Entry));
              left = start;
            }
            leaf.entries[--output] = additions[--right].entry;
          }
        }
      }
      assert(final_size == leaf.live_count);
      sorted_count_ = sorted_count_ - old_sorted + final_size;
      dirty_count_ -= tail_size;
      leaf.sorted_size = final_size;
      leaf.invalidated.fill(0);
    }
  }

  void expand_fence(const Path& path, const Entry& entry) noexcept {
    auto& leaf = leaves_[path.leaf];
    if (leaf.has_fence && !less(leaf.fence, entry)) return;
    leaf.fence = entry;
    leaf.has_fence = true;
    Entry child_fence = entry;
    for (std::size_t depth = path.depth; depth != 0;) {
      --depth;
      auto& branch = branches_[path.nodes[depth]];
      const auto slot = path.slots[depth];
      branch.maxes[slot] = child_fence;
      child_fence = branch.maxes[branch.count - 1];
    }
  }

  void adjust_path_count(const Path& path, int delta) noexcept {
    for (std::size_t depth = path.depth; depth != 0;) {
      --depth;
      auto& branch = branches_[path.nodes[depth]];
      const auto slot = path.slots[depth];
      if (delta > 0) {
        ++branch.counts[slot];
        ++branch.total;
      } else {
        --branch.counts[slot];
        --branch.total;
      }
    }
  }

  void split_leaf(const Path& path) {
    compact_leaf(path.leaf);
    assert(leaves_[path.leaf].live_count == kLeafCapacity);

    // All allocations precede changes to the live directory.
    reserve_memory_vector(branches_, branches_.size() + height_ + 1);
    const auto right_id = allocate_leaf();
    auto& leaf = leaves_[path.leaf];
    auto& right = leaves_[right_id];
    const auto split = leaf.sorted_size / 2;
    const auto old_fence = leaf.fence;
    if constexpr (ScoreRle) {
      std::array<Entry, kLeafCapacity> values;
      for (std::size_t i = 0; i < leaf.sorted_size; ++i) {
        values[i] = base_entry(leaf, i);
      }
      const std::span<const Entry> all(values.data(), leaf.sorted_size);
      try {
        leaf.entries.reserve_words(RleEntries::word_count(all.first(split)));
        right.entries.reserve_words(RleEntries::word_count(all.subspan(split)));
      } catch (...) {
        release_leaf(right_id);
        throw;
      }
      assign_rle_base(right, all.subspan(split));
      assign_rle_base(leaf, all.first(split));
    } else {
      right.entries.insert(
          right.entries.end(),
          leaf.entries.begin() + static_cast<std::ptrdiff_t>(split),
          leaf.entries.end());
      leaf.entries.resize(split);
      right.sorted_size = right.entries.size();
      leaf.sorted_size = split;
    }
    right.live_count = right.sorted_size;
    leaf.live_count = split;
    leaf.fence = base_entry(leaf, split - 1);
    right.fence = old_fence;
    right.has_fence = true;
    right.prev = path.leaf;
    right.next = leaf.next;
    if (right.next != kNull) leaves_[right.next].prev = right_id;
    leaf.next = right_id;
    if (last_leaf_ == path.leaf) last_leaf_ = right_id;
    ++active_leaf_count_;

    insert_split(path, path.leaf, leaf.fence, leaf.live_count, right_id,
                 right.fence, right.live_count);
  }

  void insert_split(const Path& path, std::uint32_t left_id,
                    Entry left_fence, std::size_t left_count,
                    std::uint32_t right_id, Entry right_fence,
                    std::size_t right_count) {
    auto depth = path.depth;
    while (depth != 0) {
      --depth;
      auto& parent = branches_[path.nodes[depth]];
      const auto slot = static_cast<std::size_t>(path.slots[depth]);
      parent.maxes[slot] = left_fence;
      parent.counts[slot] = left_count;
      for (std::size_t i = parent.count; i > slot + 1; --i) {
        parent.maxes[i] = parent.maxes[i - 1];
        parent.children[i] = parent.children[i - 1];
        parent.counts[i] = parent.counts[i - 1];
      }
      parent.maxes[slot + 1] = right_fence;
      parent.children[slot + 1] = right_id;
      parent.counts[slot + 1] = right_count;
      ++parent.count;
      if (parent.count <= kBranchCapacity) return;

      const auto old_total = parent.total;
      const auto branch_right = allocate_branch();
      auto& left = branches_[path.nodes[depth]];
      auto& right = branches_[branch_right];
      const auto middle = static_cast<std::size_t>(left.count) / 2;
      right.count = static_cast<std::uint16_t>(left.count - middle);
      for (std::size_t i = 0; i < right.count; ++i) {
        right.maxes[i] = left.maxes[middle + i];
        right.children[i] = left.children[middle + i];
        right.counts[i] = left.counts[middle + i];
        right.total += right.counts[i];
      }
      left.count = static_cast<std::uint16_t>(middle);
      left.total = old_total - right.total;
      left_id = path.nodes[depth];
      right_id = branch_right;
      left_fence = left.maxes[left.count - 1];
      right_fence = right.maxes[right.count - 1];
      left_count = left.total;
      right_count = right.total;
    }

    const auto new_root = allocate_branch();
    auto& root = branches_[new_root];
    root.count = 2;
    root.children[0] = left_id;
    root.children[1] = right_id;
    root.maxes[0] = left_fence;
    root.maxes[1] = right_fence;
    root.counts[0] = left_count;
    root.counts[1] = right_count;
    root.total = left_count + right_count;
    root_ = new_root;
    ++height_;
    if (height_ >= kMaxHeight) {
      throw std::length_error("packed zset B+ tree height exhausted");
    }
  }

  void maybe_rebalance_leaf(std::uint32_t leaf_id) {
    if (leaf_id == kNull || !leaves_[leaf_id].active ||
        active_leaf_count_ <= 1 ||
        leaves_[leaf_id].live_count >= kLeafCapacity / 4) {
      return;
    }

    auto left_id = leaf_id;
    auto right_id = leaves_[leaf_id].next;
    if (right_id == kNull) {
      left_id = leaves_[leaf_id].prev;
      right_id = leaf_id;
    }
    if (left_id == kNull || right_id == kNull) return;
    if constexpr (ScoreRle) {
      rebalance_rle_leaves(left_id, right_id);
    } else {
      compact_leaf(left_id);
      compact_leaf(right_id);

      auto left_path = locate(leaves_[left_id].fence);
      auto right_path = locate(leaves_[right_id].fence);
      assert(left_path.leaf == left_id && right_path.leaf == right_id);
      auto& left = leaves_[left_id];
      auto& right = leaves_[right_id];
      const auto left_old = left.live_count;
      const auto right_old = right.live_count;
      const auto combined = left_old + right_old;

      if (combined > kLeafCapacity) {
        const auto left_new = combined / 2;
        const auto right_new = combined - left_new;
        // Compacted neighbors are already globally ordered. Transfer only the
        // boundary run; the destination's reserved leaf capacity is sufficient.
        assert(left_new <= left.entries.capacity());
        assert(right_new <= right.entries.capacity());
        if (left_old < left_new) {
          const auto boundary = right.entries.begin() +
                                static_cast<std::ptrdiff_t>(left_new - left_old);
          left.entries.insert(left.entries.end(), right.entries.begin(), boundary);
          right.entries.erase(right.entries.begin(), boundary);
        } else if (left_old > left_new) {
          const auto boundary = left.entries.begin() +
                                static_cast<std::ptrdiff_t>(left_new);
          right.entries.insert(right.entries.begin(), boundary, left.entries.end());
          left.entries.resize(left_new);
        }
        left.sorted_size = left.live_count = left_new;
        right.sorted_size = right.live_count = right_new;
        left.fence = left.entries.back();
        refresh_path(left_path, static_cast<std::ptrdiff_t>(left_new) -
                                     static_cast<std::ptrdiff_t>(left_old));
        refresh_path(right_path, static_cast<std::ptrdiff_t>(right_new) -
                                      static_cast<std::ptrdiff_t>(right_old));
        return;
      }

      left.entries.insert(left.entries.end(), right.entries.begin(),
                          right.entries.end());
      left.sorted_size = left.live_count = combined;
      left.fence = right.fence;
      left.next = right.next;
      if (right.next != kNull) leaves_[right.next].prev = left_id;
      if (last_leaf_ == right_id) last_leaf_ = left_id;
      refresh_path(left_path, static_cast<std::ptrdiff_t>(right_old));
      remove_child(right_path, right_old);
      release_leaf(right_id);
      --active_leaf_count_;

      if (left.live_count < kLeafCapacity / 4 && active_leaf_count_ > 1) {
        maybe_rebalance_leaf(left_id);
      }
    }
  }

  void refresh_path(const Path& path, std::ptrdiff_t delta) noexcept {
    Entry child_fence = leaves_[path.leaf].fence;
    for (std::size_t depth = path.depth; depth != 0;) {
      --depth;
      auto& branch = branches_[path.nodes[depth]];
      const auto slot = path.slots[depth];
      if (delta >= 0) {
        branch.counts[slot] += static_cast<std::size_t>(delta);
        branch.total += static_cast<std::size_t>(delta);
      } else {
        branch.counts[slot] -= static_cast<std::size_t>(-delta);
        branch.total -= static_cast<std::size_t>(-delta);
      }
      branch.maxes[slot] = child_fence;
      child_fence = branch.maxes[branch.count - 1];
    }
  }

  void remove_child(const Path& path, std::size_t removed_count) noexcept {
    bool erase_child = true;
    Entry child_fence{};
    for (std::size_t depth = path.depth; depth != 0;) {
      --depth;
      auto& branch = branches_[path.nodes[depth]];
      const auto slot = static_cast<std::size_t>(path.slots[depth]);
      if (erase_child) {
        for (std::size_t i = slot + 1; i < branch.count; ++i) {
          branch.maxes[i - 1] = branch.maxes[i];
          branch.children[i - 1] = branch.children[i];
          branch.counts[i - 1] = branch.counts[i];
        }
        --branch.count;
        branch.total -= removed_count;
        if (branch.count == 0) {
          release_branch(path.nodes[depth]);
          continue;
        }
        child_fence = branch.maxes[branch.count - 1];
        erase_child = false;
      } else {
        branch.counts[slot] -= removed_count;
        branch.total -= removed_count;
        branch.maxes[slot] = child_fence;
        child_fence = branch.maxes[branch.count - 1];
      }
    }

    while (height_ != 0) {
      auto& root = branches_[root_];
      if (root.count != 1) break;
      const auto old_root = root_;
      root_ = root.children[0];
      --height_;
      release_branch(old_root);
    }
  }

  [[nodiscard]] bool validate_subtree(std::uint32_t node, std::size_t level,
                                      std::size_t& total,
                                      Entry& fence) const {
    if (level == 0) {
      if (node >= leaves_.size() || !leaves_[node].active) return false;
      total = leaves_[node].live_count;
      fence = leaves_[node].fence;
      return true;
    }
    if (node >= branches_.size()) return false;
    const auto& branch = branches_[node];
    if (!branch.active || branch.count == 0 ||
        branch.count > kBranchCapacity) {
      return false;
    }
    total = 0;
    for (std::size_t i = 0; i < branch.count; ++i) {
      std::size_t child_total = 0;
      Entry child_fence{};
      if (!validate_subtree(branch.children[i], level - 1, child_total,
                            child_fence) ||
          child_total != branch.counts[i] ||
          !equivalent(child_fence, branch.maxes[i]) ||
          (i != 0 && !less(branch.maxes[i - 1], branch.maxes[i]))) {
        return false;
      }
      total += child_total;
    }
    if (total != branch.total) return false;
    fence = branch.maxes[branch.count - 1];
    return true;
  }

  void clear() noexcept {
    leaves_.clear();
    branches_.clear();
    root_ = kNull;
    first_leaf_ = kNull;
    last_leaf_ = kNull;
    free_leaf_ = kNull;
    free_branch_ = kNull;
    height_ = 0;
    active_leaf_count_ = 0;
    active_branch_count_ = 0;
    size_ = 0;
    sorted_count_ = 0;
    dirty_count_ = 0;
  }

  [[nodiscard]] static std::optional<std::pair<std::size_t, std::size_t>>
  normalize_rank_range(std::size_t size, long long start,
                       long long stop) noexcept {
    if (size == 0) return std::nullopt;
    const auto n = static_cast<long long>(
        std::min<std::size_t>(size, static_cast<std::size_t>(
                                        std::numeric_limits<long long>::max())));
    if (start < 0) start = std::max<long long>(0, n + start);
    if (stop < 0) stop = n + stop;
    if (start >= n || stop < 0 || start > stop) return std::nullopt;
    stop = std::min(stop, n - 1);
    return std::pair{static_cast<std::size_t>(start),
                     static_cast<std::size_t>(stop - start + 1)};
  }

  std::vector<Leaf> leaves_;
  std::vector<Branch> branches_;
  std::uint32_t root_{kNull};
  std::uint32_t first_leaf_{kNull};
  std::uint32_t last_leaf_{kNull};
  std::uint32_t free_leaf_{kNull};
  std::uint32_t free_branch_{kNull};
  std::size_t height_{0};
  std::size_t active_leaf_count_{0};
  std::size_t active_branch_count_{0};
  std::size_t size_{0};
  std::size_t sorted_count_{0};
  std::size_t dirty_count_{0};
  double merge_exponent_{0.5};
  std::size_t merge_threshold_{1};
};

}  // namespace goblin::core::detail
