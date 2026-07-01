#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace goblin::core {

template <class T, class Compare = std::less<T>, std::size_t Load = 256>
class ChunkedSortedList {
  static_assert(Load > 1);

 public:
  using value_type = T;
  using size_type = std::size_t;

  ChunkedSortedList() = default;

  explicit ChunkedSortedList(Compare compare) : compare_(std::move(compare)) {}

  void set_compare(Compare compare) {
    compare_ = std::move(compare);
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

  [[nodiscard]] size_type size() const noexcept {
    return size_;
  }

  [[nodiscard]] size_type block_count() const noexcept {
    return blocks_.size();
  }

  [[nodiscard]] bool contains(const T& value) const {
    return rank(value).has_value();
  }

  [[nodiscard]] size_type lower_bound_rank(const T& value) const {
    if (blocks_.empty()) {
      return 0;
    }

    const auto block_index = lower_block(value);
    if (block_index == blocks_.size()) {
      return size_;
    }

    const auto& block = blocks_[block_index];
    const auto found = std::ranges::lower_bound(block, value, compare_);
    return prefix_size(block_index) +
           static_cast<size_type>(found - block.begin());
  }

  [[nodiscard]] std::optional<size_type> rank(const T& value) const {
    if (blocks_.empty()) {
      return std::nullopt;
    }

    const auto block_index = lower_block(value);
    if (block_index == blocks_.size()) {
      return std::nullopt;
    }

    const auto& block = blocks_[block_index];
    const auto found = std::ranges::lower_bound(block, value, compare_);
    if (found == block.end() || !equivalent(*found, value)) {
      return std::nullopt;
    }

    return prefix_size(block_index) +
           static_cast<size_type>(found - block.begin());
  }

  void insert(T value) {
    if (blocks_.empty()) {
      blocks_.push_back(Block{});
      blocks_.back().push_back(std::move(value));
      maxes_.push_back(blocks_.back().back());
      size_ = 1;
      invalidate_index();
      return;
    }

    const auto block_index = upper_block(value);
    auto& block = blocks_[block_index];
    const auto insert_at = std::ranges::upper_bound(block, value, compare_);
    block.insert(insert_at, std::move(value));
    ++size_;

    if (block.size() > Load * 2) {
      split_block(block_index);
      invalidate_index();
      return;
    }

    maxes_[block_index] = block.back();
    update_index(block_index, 1);
  }

  [[nodiscard]] bool erase_one(const T& value) {
    if (blocks_.empty()) {
      return false;
    }

    const auto block_index = lower_block(value);
    if (block_index == blocks_.size()) {
      return false;
    }

    auto& block = blocks_[block_index];
    const auto found = std::ranges::lower_bound(block, value, compare_);
    if (found == block.end() || !equivalent(*found, value)) {
      return false;
    }

    block.erase(found);
    --size_;

    if (block.empty()) {
      blocks_.erase(blocks_.begin() + static_cast<long>(block_index));
      maxes_.erase(maxes_.begin() + static_cast<long>(block_index));
      invalidate_index();
      return true;
    }

    maxes_[block_index] = block.back();
    update_index(block_index, -1);

    if (block.size() < Load / 2 && blocks_.size() > 1) {
      rebalance_after_erase(block_index);
      invalidate_index();
    }

    return true;
  }

  [[nodiscard]] std::vector<T> range(size_type start, size_type count) const {
    std::vector<T> out;
    if (count == 0 || start >= size_) {
      return out;
    }

    count = std::min(count, size_ - start);
    out.reserve(count);

    auto [block_index, offset] = position_at(start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, block.size() - offset);
      out.insert(out.end(), block.begin() + static_cast<long>(offset),
                 block.begin() + static_cast<long>(offset + take));
      count -= take;
      ++block_index;
      offset = 0;
    }

    return out;
  }

  [[nodiscard]] const T& at(size_type index) const {
    const auto [block_index, offset] = position_at(index);
    return blocks_[block_index][offset];
  }

  void clear() {
    blocks_.clear();
    maxes_.clear();
    index_.clear();
    index_offset_ = 0;
    size_ = 0;
  }

  [[nodiscard]] bool validate() const {
    if (blocks_.size() != maxes_.size()) {
      return false;
    }
    if (blocks_.empty()) {
      return size_ == 0;
    }

    size_type counted = 0;
    for (size_type block_index = 0; block_index < blocks_.size(); ++block_index) {
      const auto& block = blocks_[block_index];
      if (block.empty()) {
        return false;
      }

      if (!equivalent(block.back(), maxes_[block_index])) {
        return false;
      }

      for (size_type i = 1; i < block.size(); ++i) {
        if (compare_(block[i], block[i - 1])) {
          return false;
        }
      }

      if (block_index > 0 && compare_(block.front(), blocks_[block_index - 1].back())) {
        return false;
      }

      counted += block.size();
    }

    return counted == size_;
  }

 private:
  using Block = std::vector<T>;

  [[nodiscard]] bool equivalent(const T& lhs, const T& rhs) const {
    return !compare_(lhs, rhs) && !compare_(rhs, lhs);
  }

  [[nodiscard]] size_type lower_block(const T& value) const {
    return static_cast<size_type>(
        std::ranges::lower_bound(maxes_, value, compare_) - maxes_.begin());
  }

  [[nodiscard]] size_type upper_block(const T& value) const {
    auto block_index = static_cast<size_type>(
        std::ranges::upper_bound(maxes_, value, compare_) - maxes_.begin());
    if (block_index == blocks_.size()) {
      block_index = blocks_.size() - 1;
    }
    return block_index;
  }

  void split_block(size_type block_index) {
    auto& block = blocks_[block_index];
    const auto split_at = block.size() / 2;

    Block right;
    right.reserve(block.size() - split_at);
    right.insert(right.end(),
                 std::make_move_iterator(block.begin() + static_cast<long>(split_at)),
                 std::make_move_iterator(block.end()));
    block.erase(block.begin() + static_cast<long>(split_at), block.end());

    blocks_.insert(blocks_.begin() + static_cast<long>(block_index + 1), std::move(right));
    maxes_[block_index] = blocks_[block_index].back();
    maxes_.insert(maxes_.begin() + static_cast<long>(block_index + 1),
                  blocks_[block_index + 1].back());
  }

  void rebalance_after_erase(size_type block_index) {
    if (block_index + 1 < blocks_.size()) {
      merge_with_next(block_index);
      return;
    }
    merge_with_next(block_index - 1);
  }

  void merge_with_next(size_type block_index) {
    auto& left = blocks_[block_index];
    auto& right = blocks_[block_index + 1];
    left.reserve(left.size() + right.size());
    left.insert(left.end(),
                std::make_move_iterator(right.begin()),
                std::make_move_iterator(right.end()));

    blocks_.erase(blocks_.begin() + static_cast<long>(block_index + 1));
    maxes_.erase(maxes_.begin() + static_cast<long>(block_index + 1));
    maxes_[block_index] = left.back();

    if (left.size() > Load * 2) {
      split_block(block_index);
    }
  }

  void invalidate_index() const {
    index_.clear();
    index_offset_ = 0;
  }

  void build_index() const {
    if (!index_.empty() || blocks_.empty()) {
      return;
    }

    const auto leaf_count = std::bit_ceil(blocks_.size());
    index_offset_ = leaf_count - 1;
    index_.assign(index_offset_ + leaf_count, 0);

    for (size_type i = 0; i < blocks_.size(); ++i) {
      index_[index_offset_ + i] = blocks_[i].size();
    }
    for (size_type i = index_offset_; i > 0; --i) {
      const auto node = i - 1;
      index_[node] = index_[node * 2 + 1] + index_[node * 2 + 2];
    }
  }

  void update_index(size_type block_index, int delta) const {
    if (index_.empty()) {
      return;
    }

    auto node = index_offset_ + block_index;
    for (;;) {
      if (delta > 0) {
        index_[node] += static_cast<size_type>(delta);
      } else {
        index_[node] -= static_cast<size_type>(-delta);
      }

      if (node == 0) {
        break;
      }
      node = (node - 1) / 2;
    }
  }

  [[nodiscard]] size_type prefix_size(size_type block_index) const {
    build_index();

    size_type total = 0;
    auto node = index_offset_ + block_index;
    while (node > 0) {
      const auto parent = (node - 1) / 2;
      const auto right_child = parent * 2 + 2;
      if (node == right_child) {
        total += index_[parent * 2 + 1];
      }
      node = parent;
    }

    return total;
  }

  [[nodiscard]] std::pair<size_type, size_type> position_at(size_type index) const {
    build_index();

    size_type node = 0;
    while (node < index_offset_) {
      const auto left = node * 2 + 1;
      const auto left_size = index_[left];
      if (index < left_size) {
        node = left;
      } else {
        index -= left_size;
        node = left + 1;
      }
    }

    return {node - index_offset_, index};
  }

  std::vector<Block> blocks_;
  std::vector<T> maxes_;
  mutable std::vector<size_type> index_;
  mutable size_type index_offset_{0};
  size_type size_{0};
  [[no_unique_address]] Compare compare_{};
};

}  // namespace goblin::core
