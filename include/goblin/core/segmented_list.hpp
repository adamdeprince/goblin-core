#pragma once

// Memory-oriented Redis list: packed inline leaves plus a Fenwick count index.
// Rank lookup selects a leaf in O(log leaves), then scans at most one compact
// leaf. Mutations rebuild only the touched leaf; full leaves split and sparse
// neighbors merge. Values never carry a per-element pointer.

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/list_listpack.hpp"

namespace goblin::core {

class SegmentedList {
 public:
  static constexpr std::size_t kDefaultLeafEntries = 128;

  explicit SegmentedList(
      StringEncodingOptions encoding = {},
      StringCompressionMode compression = StringCompressionMode::NeverLz4,
      std::size_t leaf_entries = kDefaultLeafEntries)
      : encoding_(encoding),
        compression_(compression),
        leaf_entries_(std::max<std::size_t>(2, leaf_entries)) {}

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t leaf_count() const noexcept {
    return leaves_.size();
  }

  // Build directly from final-order views. Snapshot restore and a first
  // multi-value push use this path, so values move from the input buffer into
  // their final listpack leaves without an intermediate vector<string>.
  void assign(std::span<const std::string_view> values) {
    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("list element space exhausted");
    }
    auto replacements = build_view_leaves(values);
    leaves_ = std::move(replacements);
    size_ = values.size();
    rebuild_index();
  }

  void assign_raw(std::span<const std::string_view> values) {
    if (values.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("list element space exhausted");
    }
    auto replacements = build_raw_leaves(values);
    leaves_ = std::move(replacements);
    size_ = values.size();
    rebuild_index();
  }

  void insert(std::size_t index, std::string_view value) {
    assert(index <= size_);
    if (leaves_.empty()) {
      ListListpack leaf;
      const bool inserted = leaf.insert(0, value, leaf_entries_, encoding_,
                                        compression_);
      if (!inserted) {
        throw std::length_error("list value does not fit packed leaf");
      }
      leaves_.push_back(std::move(leaf));
      size_ = 1;
      rebuild_index();
      return;
    }

    const auto bias = index == size_ ? LeafBias::Back
                                     : index == 0 ? LeafBias::Front
                                                  : LeafBias::Balanced;
    auto location = index == size_ ? tail_location() : locate(index);
    if (leaves_[location.leaf].insert(
            location.offset, value, leaf_entries_, encoding_, compression_)) {
      ++size_;
      add_count(location.leaf, 1);
      return;
    }

    auto values = leaf_values(location.leaf);
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(location.offset),
                  std::string(value));
    replace_leaf(location.leaf, values, bias);
    ++size_;
  }

  void insert_many(std::size_t index,
                   std::span<const std::string_view> values) {
    assert(index <= size_);
    if (values.empty()) {
      return;
    }

    if (leaves_.empty()) {
      assign(values);
      return;
    }

    // A full endpoint leaf is already final. Pack an adjacent batch into new
    // leaves and splice those leaves directly instead of decoding and rebuilding
    // the full endpoint along with the new values.
    if (index == size_ && leaves_.back().size() == leaf_entries_) {
      auto additions = build_view_leaves(values);
      reserve_leaf_count(leaves_.size() + additions.size());
      leaves_.insert(leaves_.end(),
                     std::make_move_iterator(additions.begin()),
                     std::make_move_iterator(additions.end()));
      size_ += values.size();
      rebuild_index();
      return;
    }
    if (index == 0 && leaves_.front().size() == leaf_entries_) {
      auto additions = build_view_leaves(values);
      reserve_leaf_count(leaves_.size() + additions.size());
      leaves_.insert(leaves_.begin(),
                     std::make_move_iterator(additions.begin()),
                     std::make_move_iterator(additions.end()));
      size_ += values.size();
      rebuild_index();
      return;
    }

    const auto bias = index == size_ ? LeafBias::Back
                                     : index == 0 ? LeafBias::Front
                                                  : LeafBias::Balanced;
    std::vector<std::string> combined;
    const auto location = index == size_ ? tail_location() : locate(index);
    const auto leaf = location.leaf;
    const auto offset = location.offset;
    combined = leaf_values(leaf);
    combined.reserve(combined.size() + values.size());
    combined.insert(combined.begin() + static_cast<std::ptrdiff_t>(offset),
                    values.begin(), values.end());

    replace_leaf(leaf, combined, bias);
    size_ += values.size();
  }

  [[nodiscard]] EncodedStringView at(std::size_t index) const noexcept {
    assert(index < size_);
    const auto location = locate(index);
    return leaves_[location.leaf].at(location.offset, encoding_);
  }

  void set(std::size_t index, std::string_view value) {
    assert(index < size_);
    const auto location = locate(index);
    if (leaves_[location.leaf].set(location.offset, value, encoding_,
                                   compression_)) {
      return;
    }
    auto values = leaf_values(location.leaf);
    values[location.offset] = std::string(value);
    replace_leaf(location.leaf, values);
  }

  [[nodiscard]] std::string erase(std::size_t index) {
    assert(index < size_);
    const auto location = locate(index);
    auto removed = leaves_[location.leaf].erase(location.offset, encoding_);
    --size_;
    if (leaves_[location.leaf].empty()) {
      leaves_.erase(leaves_.begin() +
                    static_cast<std::ptrdiff_t>(location.leaf));
      rebuild_index();
    } else {
      add_count(location.leaf, -1);
      maybe_merge(location.leaf);
    }
    return removed;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (const auto& leaf : leaves_) {
      leaf.for_each(fn, encoding_);
    }
  }

  template <class Fn>
  void for_range(std::size_t first, std::size_t count, Fn&& fn) const {
    if (first >= size_ || count == 0) {
      return;
    }
    auto location = locate(first);
    auto remaining = std::min(count, size_ - first);
    while (location.leaf < leaves_.size() && remaining != 0) {
      const auto& leaf = leaves_[location.leaf];
      const auto available = leaf.size() - location.offset;
      const auto take = std::min(available, remaining);
      leaf.for_range(location.offset, take, fn, encoding_);
      remaining -= take;
      ++location.leaf;
      location.offset = 0;
    }
  }

  [[nodiscard]] std::optional<std::size_t> find_first(
      std::string_view value, std::size_t first = 0) const {
    if (first >= size_) {
      return std::nullopt;
    }
    auto location = locate(first);
    auto rank = first;
    for (auto leaf = location.leaf; leaf < leaves_.size(); ++leaf) {
      const auto begin = leaf == location.leaf ? location.offset : 0;
      if (const auto match =
              leaves_[leaf].find_first(value, begin, encoding_)) {
        return rank + *match - begin;
      }
      rank += leaves_[leaf].size() - begin;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::size_t> find_last(
      std::string_view value, std::size_t end) const {
    end = std::min(end, size_);
    if (end == 0) {
      return std::nullopt;
    }
    auto location = locate(end - 1);
    for (std::size_t leaf_cursor = location.leaf + 1; leaf_cursor != 0;) {
      const auto leaf = --leaf_cursor;
      const auto local_end = leaf == location.leaf ? location.offset + 1
                                                    : leaves_[leaf].size();
      if (const auto match =
              leaves_[leaf].find_last(value, local_end, encoding_)) {
        return static_cast<std::size_t>(prefix_count(leaf)) + *match;
      }
    }
    return std::nullopt;
  }

  void compact() {
    for (std::size_t leaf = 0; leaf + 1 < leaves_.size();) {
      if (leaves_[leaf].size() + leaves_[leaf + 1].size() <= leaf_entries_) {
        auto values = leaf_values(leaf);
        auto next = leaf_values(leaf + 1);
        values.insert(values.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        replace_leaf_range(leaf, 2, values, LeafBias::Balanced);
      } else {
        ++leaf;
      }
    }
    leaves_.shrink_to_fit();
    rebuild_index();
    fenwick_.shrink_to_fit();
  }

  [[nodiscard]] std::size_t value_allocated_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& leaf : leaves_) {
      bytes += leaf.allocated_bytes();
    }
    return bytes;
  }

  [[nodiscard]] std::size_t index_allocated_bytes() const noexcept {
    return leaves_.capacity() * sizeof(ListListpack) +
           fenwick_.capacity() * sizeof(std::uint32_t);
  }

  [[nodiscard]] bool check_invariants() const noexcept {
    std::size_t count = 0;
    for (const auto& leaf : leaves_) {
      if (leaf.empty() || leaf.size() > leaf_entries_) {
        return false;
      }
      count += leaf.size();
    }
    return count == size_ && fenwick_.size() == leaves_.size() + 1 &&
           prefix_count(leaves_.size()) == size_;
  }

 private:
  enum class LeafBias { Balanced, Front, Back };

  struct Location {
    std::size_t leaf{0};
    std::size_t offset{0};
  };

  [[nodiscard]] std::uint32_t prefix_count(
      std::size_t leaves) const noexcept {
    std::uint32_t sum = 0;
    for (auto node = leaves; node != 0; node &= node - 1) {
      sum += fenwick_[node];
    }
    return sum;
  }

  [[nodiscard]] Location locate(std::size_t rank) const noexcept {
    assert(rank < size_);
    auto remaining = static_cast<std::uint32_t>(rank + 1);
    std::size_t prefix_leaves = 0;
    for (auto step = std::bit_floor(leaves_.size()); step != 0; step >>= 1) {
      const auto next = prefix_leaves + step;
      if (next < fenwick_.size() && fenwick_[next] < remaining) {
        prefix_leaves = next;
        remaining -= fenwick_[next];
      }
    }
    return {.leaf = prefix_leaves,
            .offset = static_cast<std::size_t>(remaining - 1)};
  }

  [[nodiscard]] Location tail_location() const noexcept {
    assert(!leaves_.empty());
    return {.leaf = leaves_.size() - 1,
            .offset = leaves_.back().size()};
  }

  void rebuild_index() {
    fenwick_.assign(leaves_.size() + 1, 0);
    for (std::size_t leaf = 0; leaf < leaves_.size(); ++leaf) {
      const auto count = static_cast<std::uint32_t>(leaves_[leaf].size());
      for (auto node = leaf + 1; node < fenwick_.size();
           node += node & (~node + 1)) {
        fenwick_[node] += count;
      }
    }
  }

  void add_count(std::size_t leaf, int delta) noexcept {
    for (auto node = leaf + 1; node < fenwick_.size();
         node += node & (~node + 1)) {
      if (delta >= 0) {
        fenwick_[node] += static_cast<std::uint32_t>(delta);
      } else {
        fenwick_[node] -= static_cast<std::uint32_t>(-delta);
      }
    }
  }

  [[nodiscard]] std::vector<std::string> leaf_values(
      std::size_t leaf) const {
    std::vector<std::string> values;
    values.reserve(leaves_[leaf].size());
    leaves_[leaf].for_each(
        [&values](EncodedStringView value) {
          values.push_back(value.to_string());
        },
        encoding_);
    return values;
  }

  [[nodiscard]] bool build_balanced_leaves(
      std::span<const std::string> values,
      std::vector<ListListpack>& replacements) const {
    const auto count =
        (values.size() + leaf_entries_ - 1) / leaf_entries_;
    const auto base = values.size() / count;
    const auto extra = values.size() % count;
    auto cursor = std::size_t{0};
    replacements.reserve(count);
    for (std::size_t leaf = 0; leaf < count; ++leaf) {
      const auto take = base + static_cast<std::size_t>(leaf < extra);
      std::vector<std::string_view> views;
      views.reserve(take);
      for (std::size_t index = 0; index < take; ++index) {
        views.push_back(values[cursor + index]);
      }
      ListListpack packed;
      if (!packed.assign(views, leaf_entries_, encoding_, compression_)) {
        return false;
      }
      replacements.push_back(std::move(packed));
      cursor += take;
    }
    return true;
  }

  [[nodiscard]] bool assign_leaf(std::span<const std::string> values,
                                 std::size_t begin, std::size_t count,
                                 ListListpack& leaf) const {
    std::vector<std::string_view> views;
    views.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      views.push_back(values[begin + index]);
    }
    return leaf.assign(views, leaf_entries_, encoding_, compression_);
  }

  [[nodiscard]] std::vector<ListListpack> build_greedy_leaves(
      std::span<const std::string> values, LeafBias bias) const {
    std::vector<ListListpack> replacements;
    auto cursor = bias == LeafBias::Front ? values.size() : std::size_t{0};
    while ((bias == LeafBias::Front && cursor != 0) ||
           (bias != LeafBias::Front && cursor < values.size())) {
      const auto remaining = bias == LeafBias::Front
                                 ? cursor
                                 : values.size() - cursor;
      auto low = std::size_t{1};
      auto high = std::min(leaf_entries_, remaining);
      auto best = std::size_t{0};
      ListListpack best_leaf;
      while (low <= high) {
        const auto take = low + (high - low) / 2;
        const auto begin =
            bias == LeafBias::Front ? cursor - take : cursor;
        ListListpack candidate;
        if (assign_leaf(values, begin, take, candidate)) {
          best = take;
          best_leaf = std::move(candidate);
          low = take + 1;
        } else {
          high = take - 1;
        }
      }
      if (best == 0) {
        throw std::length_error("list value does not fit packed leaf");
      }
      replacements.push_back(std::move(best_leaf));
      if (bias == LeafBias::Front) {
        cursor -= best;
      } else {
        cursor += best;
      }
    }
    if (bias == LeafBias::Front) {
      std::reverse(replacements.begin(), replacements.end());
    }
    return replacements;
  }

  [[nodiscard]] std::vector<ListListpack> build_leaves(
      std::span<const std::string> values, LeafBias bias) const {
    std::vector<ListListpack> replacements;
    if (values.empty()) {
      return replacements;
    }
    if (bias == LeafBias::Balanced &&
        build_balanced_leaves(values, replacements)) {
      return replacements;
    }
    // Endpoints keep interior leaves full; middle splits share slack. Large
    // values can hit the 64 KiB byte ceiling before the entry ceiling, in which
    // case the same greedy packing finds the largest fitting range.
    return build_greedy_leaves(values, bias);
  }

  [[nodiscard]] std::vector<ListListpack> build_view_leaves(
      std::span<const std::string_view> values) const {
    std::vector<ListListpack> replacements;
    if (values.empty()) {
      return replacements;
    }
    replacements.reserve((values.size() + leaf_entries_ - 1) / leaf_entries_);
    for (std::size_t cursor = 0; cursor < values.size();) {
      const auto maximum = std::min(leaf_entries_, values.size() - cursor);
      ListListpack best_leaf;
      std::size_t best = 0;

      // The common fixed-width case fills a leaf immediately. Only search when
      // the 64 KiB blob limit, rather than the entry limit, determines packing.
      if (best_leaf.assign(values.subspan(cursor, maximum), leaf_entries_,
                           encoding_, compression_)) {
        best = maximum;
      } else {
        auto low = std::size_t{1};
        auto high = maximum - 1;
        while (low <= high) {
          const auto take = low + (high - low) / 2;
          ListListpack candidate;
          if (candidate.assign(values.subspan(cursor, take), leaf_entries_,
                               encoding_, compression_)) {
            best = take;
            best_leaf = std::move(candidate);
            low = take + 1;
          } else {
            high = take - 1;
          }
        }
      }
      if (best == 0) {
        throw std::length_error("list value does not fit packed leaf");
      }
      replacements.push_back(std::move(best_leaf));
      cursor += best;
    }
    return replacements;
  }

  [[nodiscard]] std::vector<ListListpack> build_raw_leaves(
      std::span<const std::string_view> values) const {
    std::vector<ListListpack> replacements;
    if (values.empty()) {
      return replacements;
    }
    replacements.reserve((values.size() + leaf_entries_ - 1) / leaf_entries_);
    for (std::size_t cursor = 0; cursor < values.size();) {
      const auto maximum = std::min(leaf_entries_, values.size() - cursor);
      ListListpack best_leaf;
      std::size_t best = 0;
      if (best_leaf.assign_raw(values.subspan(cursor, maximum),
                               leaf_entries_)) {
        best = maximum;
      } else {
        auto low = std::size_t{1};
        auto high = maximum - 1;
        while (low <= high) {
          const auto take = low + (high - low) / 2;
          ListListpack candidate;
          if (candidate.assign_raw(values.subspan(cursor, take),
                                   leaf_entries_)) {
            best = take;
            best_leaf = std::move(candidate);
            low = take + 1;
          } else {
            high = take - 1;
          }
        }
      }
      if (best == 0) {
        throw std::length_error("list value does not fit packed leaf");
      }
      replacements.push_back(std::move(best_leaf));
      cursor += best;
    }
    return replacements;
  }

  void insert_leaves(std::size_t leaf, std::span<const std::string> values,
                     LeafBias bias) {
    auto replacements = build_leaves(values, bias);
    reserve_leaf_count(leaves_.size() + replacements.size());
    leaves_.insert(leaves_.begin() + static_cast<std::ptrdiff_t>(leaf),
                   std::make_move_iterator(replacements.begin()),
                   std::make_move_iterator(replacements.end()));
    rebuild_index();
  }

  void replace_leaf(std::size_t leaf, std::span<const std::string> values,
                    LeafBias bias = LeafBias::Balanced) {
    replace_leaf_range(leaf, 1, values, bias);
  }

  void replace_leaf_range(std::size_t leaf, std::size_t remove_count,
                          std::span<const std::string> values,
                          LeafBias bias) {
    auto replacements = build_leaves(values, bias);
    reserve_leaf_count(leaves_.size() - remove_count + replacements.size());
    leaves_.erase(leaves_.begin() + static_cast<std::ptrdiff_t>(leaf),
                  leaves_.begin() +
                      static_cast<std::ptrdiff_t>(leaf + remove_count));
    leaves_.insert(leaves_.begin() + static_cast<std::ptrdiff_t>(leaf),
                   std::make_move_iterator(replacements.begin()),
                   std::make_move_iterator(replacements.end()));
    rebuild_index();
  }

  void reserve_leaf_count(std::size_t required) {
    if (required <= leaves_.capacity()) {
      return;
    }
    const auto grown = leaves_.capacity() == 0
                           ? std::size_t{1}
                           : leaves_.capacity() * 2;
    leaves_.reserve(std::max(required, grown));
  }

  void maybe_merge(std::size_t leaf) {
    if (leaves_.size() < 2) {
      return;
    }
    const auto left = leaf != 0 ? leaf - 1 : leaf;
    const auto right = left + 1;
    if (right >= leaves_.size() ||
        leaves_[left].size() + leaves_[right].size() > leaf_entries_) {
      return;
    }
    auto values = leaf_values(left);
    auto tail = leaf_values(right);
    values.insert(values.end(), std::make_move_iterator(tail.begin()),
                  std::make_move_iterator(tail.end()));
    replace_leaf_range(left, 2, values, LeafBias::Balanced);
  }

  std::vector<ListListpack> leaves_;
  std::vector<std::uint32_t> fenwick_;
  std::size_t size_{0};
  StringEncodingOptions encoding_{};
  StringCompressionMode compression_{StringCompressionMode::NeverLz4};
  std::size_t leaf_entries_{kDefaultLeafEntries};
};

}  // namespace goblin::core
