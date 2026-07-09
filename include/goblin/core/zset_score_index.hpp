#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/zset_member_storage.hpp"

namespace goblin::core {

struct ZSetScoreEntry {
  double score{0.0};
  std::uint32_t member_id{0};
};

enum class RankCacheMode : std::uint8_t {
  Off,
  Exact,
  BlockHint,
};

[[nodiscard]] constexpr std::string_view rank_cache_mode_name(
    RankCacheMode mode) noexcept {
  switch (mode) {
    case RankCacheMode::Off:
      return "off";
    case RankCacheMode::Exact:
      return "exact";
    case RankCacheMode::BlockHint:
      return "block-hint";
  }
  return "unknown";
}

class ZSetScoreIndex {
 public:
  using size_type = std::size_t;

  static constexpr size_type kDefaultLoad = 256;
  static constexpr std::uint32_t kDefaultBlockHintNarrowLimit =
      std::numeric_limits<std::uint16_t>::max();

  ZSetScoreIndex() = default;

  explicit ZSetScoreIndex(const ZSetMemberStorage* members,
                          RankCacheMode rank_cache_mode = RankCacheMode::Off,
                          std::uint32_t block_hint_narrow_limit =
                              kDefaultBlockHintNarrowLimit,
                          size_type load = kDefaultLoad)
      : members_(members),
        block_hint_narrow_limit_(
            std::min(block_hint_narrow_limit, kDefaultBlockHintNarrowLimit)),
        rank_cache_mode_(rank_cache_mode),
        load_(load == 0 ? kDefaultLoad : load) {}

  void set_members(const ZSetMemberStorage* members) noexcept {
    members_ = members;
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

  [[nodiscard]] size_type allocated_bytes() const noexcept;

  [[nodiscard]] size_type block_capacity_sum() const noexcept;

  [[nodiscard]] size_type location_cache_allocated_bytes() const noexcept {
    return locations_.capacity() * sizeof(std::uint32_t) +
           block_hints16_.capacity() * sizeof(std::uint16_t) +
           block_hints32_.capacity() * sizeof(std::uint32_t) +
           block_hint_offsets16_.capacity() * sizeof(std::uint16_t) +
           block_index_by_id_.capacity() * sizeof(std::uint32_t);
  }

  [[nodiscard]] bool location_cache_enabled() const noexcept {
    return rank_cache_mode_ != RankCacheMode::Off;
  }

  [[nodiscard]] RankCacheMode rank_cache_mode() const noexcept {
    return rank_cache_mode_;
  }

  void set_location_cache_enabled(bool enabled) {
    set_rank_cache_mode(enabled ? RankCacheMode::Exact : RankCacheMode::Off);
  }

  void set_rank_cache_mode(RankCacheMode mode) {
    if (rank_cache_mode_ == mode) {
      return;
    }

    rank_cache_mode_ = mode;
    if (location_cache_enabled()) {
      rebuild_location_cache();
      return;
    }

    clear_location_cache_storage();
  }

  void clear() {
    blocks_.clear();
    maxes_.clear();
    index_.clear();
    index_offset_ = 0;
    clear_location_cache();
    size_ = 0;
    score_width_ = ScoreWidth::I16;
  }

  [[nodiscard]] ScoreWidth score_width() const noexcept { return score_width_; }

  // Deep-copy block storage from another index over the same member layer.
  void copy_blocks_from(const ZSetScoreIndex& source) {
    members_ = source.members_;
    rank_cache_mode_ = source.rank_cache_mode_;
    load_ = source.load_;
    block_hint_narrow_limit_ = source.block_hint_narrow_limit_;
    size_ = source.size_;
    score_width_ = source.score_width_;
    next_block_id_ = source.next_block_id_;
    block_hints_wide_ = source.block_hints_wide_;

    blocks_.clear();
    blocks_.reserve(source.blocks_.size());
    for (const auto& source_block : source.blocks_) {
      blocks_.push_back(source_block.clone());
    }
    maxes_ = source.maxes_;
    locations_ = source.locations_;
    block_hints16_ = source.block_hints16_;
    block_hints32_ = source.block_hints32_;
    block_hint_offsets16_ = source.block_hint_offsets16_;
    block_index_by_id_ = source.block_index_by_id_;
    index_ = source.index_;
    index_offset_ = source.index_offset_;
  }

  void assign_sorted(const std::vector<ZSetScoreEntry>& values) {
    clear();
    if (values.empty()) {
      return;
    }

    // Re-derive the narrowest width that fits every score. This is how OPTIMIZE
    // demotes: a fresh sorted rebuild picks the smallest sufficient width.
    ScoreWidth width = ScoreWidth::I16;
    for (const auto& value : values) {
      width = score_width_for(value.score, width);
    }
    score_width_ = width;

    size_ = values.size();
    blocks_.reserve((values.size() + load_ - 1) / load_);
    maxes_.reserve(blocks_.capacity());

    for (size_type start = 0; start < values.size(); start += load_) {
      const auto count = std::min(load_, values.size() - start);
      Block block;
      block.score_width_ = score_width_;
      block.load_ = load_;
      assign_block_id(block);
      block.reserve(count);
      for (size_type i = 0; i < count; ++i) {
        block.push_back(values[start + i]);
      }
      maxes_.push_back(block.back());
      blocks_.push_back(std::move(block));
    }
    rebuild_location_cache();
  }

  [[nodiscard]] bool contains(ZSetScoreEntry value) const {
    return rank(value).has_value();
  }

  void insert(ZSetScoreEntry value) {
    ensure_score_width(value.score);
    if (blocks_.empty()) {
      blocks_.push_back(Block{});
      blocks_.back().score_width_ = score_width_;
      blocks_.back().load_ = load_;
      assign_block_id(blocks_.back());
      blocks_.back().push_back(value);
      maxes_.push_back(value);
      size_ = 1;
      set_location(value.member_id, 0, 0);
      invalidate_index();
      return;
    }

    const auto position = locate_insert_position(value);
    auto& block = blocks_[position.first];
    const auto offset = position.second;
    block.insert(offset, value);
    const auto block_index = position.first;
    ++size_;

    if (block.size() > load_ * 2) {
      split_block(block_index);
      refresh_block_indices_from(block_index + 1);
      refresh_block_locations(block_index);
      refresh_block_locations(block_index + 1);
      invalidate_index();
      return;
    }

    maxes_[block_index] = block.back();
    update_index(block_index, 1);
    if (rank_cache_mode_ == RankCacheMode::BlockHint) {
      set_location(value.member_id, block_index, offset);
    } else {
      refresh_block_locations_from(block_index, offset);
    }
  }

  [[nodiscard]] bool erase_one(ZSetScoreEntry value) {
    if (blocks_.empty()) {
      return false;
    }

    if (const auto located = cached_entry_location(value)) {
      erase_at(located->first, located->second);
      return true;
    }

    const auto located = locate_entry(value);
    if (!located) {
      return false;
    }

    erase_at(located->first, located->second);
    return true;
  }

  [[nodiscard]] std::optional<std::pair<size_type, size_type>> find_entry_location(
      ZSetScoreEntry value) const {
    if (const auto located = cached_entry_location(value)) {
      return located;
    }
    return locate_entry(value);
  }

  // Remove `removed` and retarget `last` to `new_member_id`. Call only after
  // member storage has been updated so lex-order checks see the moved bytes.
  [[nodiscard]] bool erase_removed_and_retarget_last(
      std::pair<size_type, size_type> erased,
      std::pair<size_type, size_type> last,
      bool erased_block_was_singleton,
      ZSetScoreEntry last_entry,
      std::uint32_t new_member_id) {
    if (erased.first == last.first && !erased_block_was_singleton &&
        erase_and_retarget_same_block(erased.second, last.second, last_entry,
                                      new_member_id, erased.first)) {
      return true;
    }

    erase_at(erased.first, erased.second, EraseRebalancePolicy::Remove);

    const auto r_block = erased.first;
    const auto r_offset = erased.second;
    auto adj_l_block = last.first;
    auto l_offset = last.second;

    if (r_block == adj_l_block) {
      if (r_offset < l_offset) {
        --l_offset;
      }
    } else if (erased_block_was_singleton && adj_l_block > r_block) {
      --adj_l_block;
    }

    if (adj_l_block < blocks_.size() && l_offset < blocks_[adj_l_block].size() &&
        blocks_[adj_l_block].score_at(l_offset) == last_entry.score &&
        blocks_[adj_l_block].member_id_at(l_offset) == last_entry.member_id &&
        retarget_member_id_at(adj_l_block, l_offset, last_entry.member_id, new_member_id)) {
      return true;
    }

    return replace_member_id(last_entry.score, last_entry.member_id, new_member_id);
  }

  [[nodiscard]] bool block_was_singleton(size_type block_index) const {
    return block_index < blocks_.size() && blocks_[block_index].size() == 1;
  }

  [[nodiscard]] bool replace_member_id(double score,
                                       std::uint32_t old_member_id,
                                       std::uint32_t new_member_id) {
    if (blocks_.empty()) {
      return false;
    }

    auto block_index = lower_block_by_score(score);
    while (block_index < blocks_.size()) {
      auto& block = blocks_[block_index];
      if (block.front().score > score) {
        return false;
      }

      auto offset = block.lower_bound_score(score);
      while (offset < block.size() && block.score_at(offset) == score) {
        if (block.member_id_at(offset) == old_member_id) {
          block.set_member_id(offset, new_member_id);

          if (order_valid_at(block_index, offset)) {
            clear_location(old_member_id);
            set_location(new_member_id, block_index, offset);
            maxes_[block_index] = block.back();
            return true;
          }

          clear_location(old_member_id);
          erase_at(block_index, offset);
          insert(ZSetScoreEntry{.score = score, .member_id = new_member_id});
          return true;
        }
        ++offset;
      }

      if (block.back().score > score) {
        return false;
      }
      ++block_index;
    }

    return false;
  }

  [[nodiscard]] std::optional<size_type> rank(ZSetScoreEntry value) const {
    if (blocks_.empty()) {
      return std::nullopt;
    }

    if (const auto cached = cached_rank(value)) {
      return cached;
    }

    const auto located = locate_entry(value);
    if (!located) {
      return std::nullopt;
    }

    if (rank_cache_mode_ == RankCacheMode::BlockHint) {
      repair_block_hint(value.member_id, located->first, located->second);
    }

    return prefix_size(located->first) + located->second;
  }

  [[nodiscard]] std::vector<ZSetScoreEntry> range(size_type start, size_type count) const {
    std::vector<ZSetScoreEntry> out;
    if (count == 0 || start >= size_) {
      return out;
    }

    count = std::min(count, size_ - start);
    out.reserve(count);

    auto [block_index, offset] = position_at(start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, block.size() - offset);
      block.append_range(offset, take, out);
      count -= take;
      ++block_index;
      offset = 0;
    }

    return out;
  }

  [[nodiscard]] std::vector<ZSetScoreEntry> reverse_range(size_type start,
                                                          size_type count) const {
    std::vector<ZSetScoreEntry> out;
    if (count == 0 || start >= size_) {
      return out;
    }

    count = std::min(count, size_ - start);
    out.reserve(count);

    auto [block_index, offset] = position_at(size_ - 1 - start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, offset + 1);
      block.append_reverse_range(offset, take, out);
      count -= take;
      if (count == 0 || block_index == 0) {
        break;
      }
      --block_index;
      offset = blocks_[block_index].size() - 1;
    }

    return out;
  }

  template <class Fn>
  void for_range(size_type start, size_type count, Fn& fn) const {
    if (count == 0 || start >= size_) {
      return;
    }

    count = std::min(count, size_ - start);

    auto [block_index, offset] = position_at(start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, block.size() - offset);
      block.for_range(offset, take, fn);
      count -= take;
      ++block_index;
      offset = 0;
    }
  }

  template <class Fn>
  void for_member_ids(size_type start, size_type count, Fn& fn) const {
    if (count == 0 || start >= size_) {
      return;
    }

    count = std::min(count, size_ - start);

    auto [block_index, offset] = position_at(start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, block.size() - offset);
      block.for_member_ids(offset, take, fn);
      count -= take;
      ++block_index;
      offset = 0;
    }
  }

  // Hand each block's contiguous run of member ids to `fn(const uint32_t*,
  // count)`. Member ids are laid out sequentially within a block, so a caller
  // can walk the span and prefetch the (scattered) member storage ahead.
  template <class Fn>
  void for_member_id_spans(size_type start, size_type count, Fn& fn) const {
    if (count == 0 || start >= size_) {
      return;
    }

    count = std::min(count, size_ - start);

    auto [block_index, offset] = position_at(start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, block.size() - offset);
      fn(block.member_ids_ptr(offset), take);
      count -= take;
      ++block_index;
      offset = 0;
    }
  }

  template <class Fn>
  void for_reverse_range(size_type start, size_type count, Fn& fn) const {
    if (count == 0 || start >= size_) {
      return;
    }

    count = std::min(count, size_ - start);

    auto [block_index, offset] = position_at(size_ - 1 - start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, offset + 1);
      block.for_reverse_range(offset, take, fn);
      count -= take;
      if (count == 0 || block_index == 0) {
        break;
      }
      --block_index;
      offset = blocks_[block_index].size() - 1;
    }
  }

  template <class Fn>
  void for_reverse_member_ids(size_type start, size_type count, Fn& fn) const {
    if (count == 0 || start >= size_) {
      return;
    }

    count = std::min(count, size_ - start);

    auto [block_index, offset] = position_at(size_ - 1 - start);
    while (count > 0 && block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      const auto take = std::min(count, offset + 1);
      block.for_reverse_member_ids(offset, take, fn);
      count -= take;
      if (count == 0 || block_index == 0) {
        break;
      }
      --block_index;
      offset = blocks_[block_index].size() - 1;
    }
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
      if (!block.validate()) {
        return false;
      }
      if (!equivalent(block.back(), maxes_[block_index])) {
        return false;
      }
      for (size_type i = 1; i < block.size(); ++i) {
        if (less(block.at(i), block.at(i - 1))) {
          return false;
        }
      }
      if (block_index > 0 && less(block.front(), blocks_[block_index - 1].back())) {
        return false;
      }
      counted += block.size();
    }

    return counted == size_;
  }

 private:
  static constexpr std::uint32_t kInvalidBlockIndex =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kInvalidLocation =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kLocationOffsetBits = 10;
  static constexpr std::uint32_t kLocationOffsetMask =
      (std::uint32_t{1} << kLocationOffsetBits) - 1;
  static constexpr std::uint32_t kMaxLocationBlockId =
      kInvalidLocation >> kLocationOffsetBits;
  static constexpr std::uint16_t kInvalidBlockHint16 =
      std::numeric_limits<std::uint16_t>::max();
  static constexpr std::uint32_t kInvalidBlockHint32 =
      std::numeric_limits<std::uint32_t>::max();

  struct Block {
    [[nodiscard]] bool empty() const noexcept {
      return size_ == 0;
    }

    [[nodiscard]] size_type size() const noexcept {
      return size_;
    }

    [[nodiscard]] size_type capacity() const noexcept {
      return capacity_;
    }

    [[nodiscard]] ScoreWidth score_width() const noexcept { return score_width_; }

    [[nodiscard]] size_type score_bytes() const noexcept {
      return score_width_bytes(score_width_);
    }

    // Scores are stored at the index's runtime width (i16/i32/f64) in a raw byte
    // buffer; read/write decode/encode to double via memcpy (alignment-safe). The
    // comparators stay double-based (decode-on-read), so the SortedList is intact.
    [[nodiscard]] double read_score(size_type index) const noexcept {
      const std::byte* p = scores_.get() + index * score_bytes();
      switch (score_width_) {
        case ScoreWidth::I16: {
          std::int16_t v;
          std::memcpy(&v, p, sizeof(v));
          return static_cast<double>(v);
        }
        case ScoreWidth::I32: {
          std::int32_t v;
          std::memcpy(&v, p, sizeof(v));
          return static_cast<double>(v);
        }
        case ScoreWidth::F64: {
          double v;
          std::memcpy(&v, p, sizeof(v));
          return v;
        }
      }
      return 0.0;
    }

    void write_score(size_type index, double value) noexcept {
      std::byte* p = scores_.get() + index * score_bytes();
      switch (score_width_) {
        case ScoreWidth::I16: {
          const auto v = static_cast<std::int16_t>(value);
          std::memcpy(p, &v, sizeof(v));
          return;
        }
        case ScoreWidth::I32: {
          const auto v = static_cast<std::int32_t>(value);
          std::memcpy(p, &v, sizeof(v));
          return;
        }
        case ScoreWidth::F64:
          std::memcpy(p, &value, sizeof(value));
          return;
      }
    }

    [[nodiscard]] size_type allocated_bytes() const noexcept {
      return static_cast<size_type>(capacity_) *
             (score_bytes() + sizeof(std::uint32_t));
    }

    [[nodiscard]] ZSetScoreEntry at(size_type index) const noexcept {
      return ZSetScoreEntry{.score = read_score(index),
                            .member_id = member_ids_[index]};
    }

    [[nodiscard]] ZSetScoreEntry front() const noexcept {
      return at(0);
    }

    [[nodiscard]] ZSetScoreEntry back() const noexcept {
      return at(size() - 1);
    }

    [[nodiscard]] size_type lower_bound(const ZSetScoreIndex& owner,
                                        ZSetScoreEntry value) const noexcept {
      size_type first = 0;
      size_type count = size();
      while (count > 0) {
        const auto step = count / 2;
        const auto mid = first + step;
        if (owner.less_parts(read_score(mid), member_ids_[mid],
                             value.score, value.member_id)) {
          first = mid + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }
      return first;
    }

    [[nodiscard]] size_type upper_bound(const ZSetScoreIndex& owner,
                                        ZSetScoreEntry value) const noexcept {
      size_type first = 0;
      size_type count = size();
      while (count > 0) {
        const auto step = count / 2;
        const auto mid = first + step;
        if (!owner.less_parts(value.score, value.member_id,
                              read_score(mid), member_ids_[mid])) {
          first = mid + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }
      return first;
    }

    [[nodiscard]] size_type lower_bound_score(double score) const noexcept {
      size_type first = 0;
      size_type count = size_;
      while (count > 0) {
        const auto step = count / 2;
        const auto mid = first + step;
        if (read_score(mid) < score) {
          first = mid + 1;
          count -= step + 1;
        } else {
          count = step;
        }
      }
      return first;
    }

    [[nodiscard]] double score_at(size_type index) const noexcept {
      return read_score(index);
    }

    [[nodiscard]] std::uint32_t member_id_at(size_type index) const noexcept {
      return member_ids_[index];
    }

    [[nodiscard]] const std::uint32_t* member_ids_ptr(size_type offset) const noexcept {
      return member_ids_.get() + offset;
    }

    void set_member_id(size_type index, std::uint32_t member_id) noexcept {
      member_ids_[index] = member_id;
    }

    void push_back(ZSetScoreEntry value) {
      reserve(size_ + 1);
      write_score(size_, value.score);
      member_ids_[size_] = value.member_id;
      ++size_;
    }

    void reserve(size_type capacity) {
      if (capacity <= capacity_) {
        return;
      }

      reallocate(capacity);
    }

    // Shrink the backing arrays to the smallest capacity that still holds the
    // current size. Used after a split, where the left half keeps the (large)
    // pre-split capacity it no longer needs.
    void trim() {
      if (allocation_capacity(size_) < capacity_) {
        reallocate(size_);
      }
    }

    void insert(size_type offset, ZSetScoreEntry value) {
      reserve(size_ + 1);
      const auto move_count = size_ - offset;
      if (move_count > 0) {
        const auto sb = score_bytes();
        std::memmove(scores_.get() + (offset + 1) * sb,
                     scores_.get() + offset * sb,
                     move_count * sb);
        std::memmove(member_ids_.get() + offset + 1,
                     member_ids_.get() + offset,
                     move_count * sizeof(std::uint32_t));
      }
      write_score(offset, value.score);
      member_ids_[offset] = value.member_id;
      ++size_;
    }

    void erase(size_type offset) {
      const auto move_count = size_ - offset - 1;
      if (move_count > 0) {
        const auto sb = score_bytes();
        std::memmove(scores_.get() + offset * sb,
                     scores_.get() + (offset + 1) * sb,
                     move_count * sb);
        std::memmove(member_ids_.get() + offset,
                     member_ids_.get() + offset + 1,
                     move_count * sizeof(std::uint32_t));
      }
      --size_;
    }

    void append_range(size_type offset,
                      size_type count,
                      std::vector<ZSetScoreEntry>& out) const {
      for (size_type i = 0; i < count; ++i) {
        out.push_back(at(offset + i));
      }
    }

    void append_reverse_range(size_type offset,
                              size_type count,
                              std::vector<ZSetScoreEntry>& out) const {
      for (size_type i = 0; i < count; ++i) {
        out.push_back(at(offset - i));
      }
    }

    template <class Fn>
    void for_range(size_type offset, size_type count, Fn& fn) const {
      for (size_type i = 0; i < count; ++i) {
        const auto index = offset + i;
        fn(read_score(index), member_ids_[index]);
      }
    }

    template <class Fn>
    void for_member_ids(size_type offset, size_type count, Fn& fn) const {
      for (size_type i = 0; i < count; ++i) {
        fn(member_ids_[offset + i]);
      }
    }

    template <class Fn>
    void for_reverse_range(size_type offset, size_type count, Fn& fn) const {
      for (size_type i = 0; i < count; ++i) {
        const auto index = offset - i;
        fn(read_score(index), member_ids_[index]);
      }
    }

    template <class Fn>
    void for_reverse_member_ids(size_type offset, size_type count, Fn& fn) const {
      for (size_type i = 0; i < count; ++i) {
        fn(member_ids_[offset - i]);
      }
    }

    [[nodiscard]] Block split_off(size_type split_at) {
      Block right;
      right.score_width_ = score_width_;
      right.load_ = load_;
      const auto right_size = size_ - split_at;
      right.reserve(right_size);
      const auto sb = score_bytes();
      std::memcpy(right.scores_.get(), scores_.get() + split_at * sb,
                  right_size * sb);
      std::copy_n(member_ids_.get() + split_at, right_size, right.member_ids_.get());
      right.size_ = right_size;
      size_ = split_at;
      return right;
    }

    void append(Block&& right) {
      reserve(size_ + right.size_);
      const auto sb = score_bytes();
      std::memcpy(scores_.get() + size_ * sb, right.scores_.get(),
                  right.size_ * sb);
      std::copy_n(right.member_ids_.get(), right.size_, member_ids_.get() + size_);
      size_ += right.size_;
    }

    [[nodiscard]] Block clone() const {
      Block copy;
      copy.score_width_ = score_width_;
      copy.size_ = size_;
      copy.capacity_ = capacity_;
      copy.id_ = id_;
      copy.load_ = load_;
      if (capacity_ > 0) {
        const auto sb = score_bytes();
        copy.scores_ = std::make_unique_for_overwrite<std::byte[]>(
            static_cast<size_type>(capacity_) * sb);
        copy.member_ids_ = std::make_unique_for_overwrite<std::uint32_t[]>(capacity_);
        if (size_ > 0) {
          std::memcpy(copy.scores_.get(), scores_.get(), size_ * sb);
          std::copy_n(member_ids_.get(), size_, copy.member_ids_.get());
        }
      }
      return copy;
    }

    // Re-encode every live score to a wider width in place (index-driven promote).
    void promote_scores(ScoreWidth target) {
      if (target == score_width_) {
        return;
      }
      const auto new_bytes = score_width_bytes(target);
      auto fresh = std::make_unique_for_overwrite<std::byte[]>(
          static_cast<size_type>(capacity_) * new_bytes);
      for (size_type i = 0; i < size_; ++i) {
        const double value = read_score(i);  // decodes at the old width
        std::byte* p = fresh.get() + i * new_bytes;
        switch (target) {
          case ScoreWidth::I16: {
            const auto v = static_cast<std::int16_t>(value);
            std::memcpy(p, &v, sizeof(v));
            break;
          }
          case ScoreWidth::I32: {
            const auto v = static_cast<std::int32_t>(value);
            std::memcpy(p, &v, sizeof(v));
            break;
          }
          case ScoreWidth::F64:
            std::memcpy(p, &value, sizeof(value));
            break;
        }
      }
      scores_ = std::move(fresh);
      score_width_ = target;
    }

    [[nodiscard]] bool validate() const noexcept {
      return size_ <= capacity_ &&
             static_cast<size_type>(capacity_) <= load_ * 2 + 1 &&
             (size_ == 0 || (scores_ != nullptr && member_ids_ != nullptr));
    }

    [[nodiscard]] size_type allocation_capacity(size_type required) const noexcept {
      if (required <= 16) {
        return 16;
      }
      if (required <= 64) {
        return 64;
      }
      if (required <= load_) {
        return load_;
      }
      if (required > load_ * 2 + 1) {
        // A block's steady-state maximum is 2*load_+1, but merge_with_next
        // transiently appends two adjacent blocks into one before splitting it
        // back. Allocate the exact size so that append does not overflow; the
        // split_block + trim that immediately follow restore the normal bound.
        return required;
      }
      if (required >= load_ * 2) {
        return load_ * 2 + 1;
      }
      // A block spends its life oscillating in (load_, 2*load_] before it
      // splits. Rounding to a coarse step here (instead of jumping straight to
      // 2*load_) keeps most of that range's capacity slack out of the score
      // index while bounding the number of growth reallocations.
      constexpr size_type kStep = 64;
      return ((required + kStep - 1) / kStep) * kStep;
    }

    void reallocate(size_type required) {
      const auto new_capacity = allocation_capacity(required);
      const auto sb = score_bytes();
      auto scores = std::make_unique_for_overwrite<std::byte[]>(new_capacity * sb);
      auto member_ids = std::make_unique_for_overwrite<std::uint32_t[]>(new_capacity);
      if (size_ > 0) {
        std::memcpy(scores.get(), scores_.get(), size_ * sb);
        std::copy_n(member_ids_.get(), size_, member_ids.get());
      }
      scores_ = std::move(scores);
      member_ids_ = std::move(member_ids);
      capacity_ = static_cast<std::uint32_t>(new_capacity);
    }

    std::unique_ptr<std::byte[]> scores_;
    std::unique_ptr<std::uint32_t[]> member_ids_;
    ScoreWidth score_width_{ScoreWidth::I16};
    // 32-bit so the sublist can hold a large runtime load factor; the extra 4 B
    // fall in existing alignment padding, so sizeof(Block) is unchanged (32 B).
    std::uint32_t size_{0};
    std::uint32_t capacity_{0};
    std::uint32_t id_{kInvalidBlockIndex};
    // The index's load factor, copied in on creation so this sublist's capacity
    // sizing (allocation_capacity/validate) is self-contained.
    size_type load_{kDefaultLoad};
  };

  [[nodiscard]] static std::uint32_t pack_location(std::uint32_t block_id,
                                                   size_type offset) noexcept {
    return (block_id << kLocationOffsetBits) |
           static_cast<std::uint32_t>(offset);
  }

  [[nodiscard]] static std::uint32_t location_block_id(
      std::uint32_t location) noexcept {
    return location >> kLocationOffsetBits;
  }

  [[nodiscard]] static size_type location_offset(std::uint32_t location) noexcept {
    return static_cast<size_type>(location & kLocationOffsetMask);
  }

  void clear_location_cache() {
    if (!location_cache_enabled()) {
      return;
    }

    clear_location_cache_storage();
  }

  void clear_location_cache_storage() {
    locations_ = {};
    block_hints16_ = {};
    block_hints32_ = {};
    block_hint_offsets16_ = {};
    block_index_by_id_ = {};
    next_block_id_ = 0;
    block_hints_wide_ = false;
  }

  void assign_block_id(Block& block) {
    if (!location_cache_enabled() || block.id_ != kInvalidBlockIndex) {
      return;
    }
    const auto max_block_id = rank_cache_mode_ == RankCacheMode::Exact
                                  ? kMaxLocationBlockId
                                  : kInvalidBlockHint32;
    if (next_block_id_ >= max_block_id) {
      throw std::length_error("zset rank location cache block id space exhausted");
    }
    block.id_ = next_block_id_++;
    if (block.id_ >= block_index_by_id_.size()) {
      block_index_by_id_.resize(static_cast<size_type>(block.id_) + 1,
                                kInvalidBlockIndex);
    }
  }

  void rebuild_location_cache() {
    if (!location_cache_enabled()) {
      return;
    }

    clear_location_cache_storage();

    for (size_type block_index = 0; block_index < blocks_.size(); ++block_index) {
      blocks_[block_index].id_ = kInvalidBlockIndex;
      assign_block_id(blocks_[block_index]);
      block_index_by_id_[blocks_[block_index].id_] =
          static_cast<std::uint32_t>(block_index);
      refresh_block_locations(block_index);
    }
  }

  void refresh_block_indices_from(size_type first_block_index) {
    if (!location_cache_enabled()) {
      return;
    }

    for (size_type block_index = first_block_index; block_index < blocks_.size();
         ++block_index) {
      assign_block_id(blocks_[block_index]);
      block_index_by_id_[blocks_[block_index].id_] =
          static_cast<std::uint32_t>(block_index);
    }
  }

  void invalidate_block_id(std::uint32_t block_id) noexcept {
    if (!location_cache_enabled() || block_id >= block_index_by_id_.size()) {
      return;
    }
    block_index_by_id_[block_id] = kInvalidBlockIndex;
  }

  void ensure_location_capacity(std::uint32_t member_id) {
    if (member_id >= locations_.size()) {
      locations_.resize(static_cast<size_type>(member_id) + 1, kInvalidLocation);
    }
  }

  void promote_block_hints_to_wide() {
    if (block_hints_wide_) {
      return;
    }

    block_hints32_.assign(block_hints16_.size(), kInvalidBlockHint32);
    for (size_type i = 0; i < block_hints16_.size(); ++i) {
      const auto hint = block_hints16_[i];
      if (hint != kInvalidBlockHint16) {
        block_hints32_[i] = hint;
      }
    }
    block_hints16_ = {};
    block_hints_wide_ = true;
  }

  void ensure_block_hint_capacity(std::uint32_t member_id) {
    const auto required = static_cast<size_type>(member_id) + 1;
    if (block_hints_wide_) {
      if (required > block_hints32_.size()) {
        block_hints32_.resize(required, kInvalidBlockHint32);
      }
      return;
    }

    if (required > block_hints16_.size()) {
      block_hints16_.resize(required, kInvalidBlockHint16);
    }
  }

  void clear_block_hint_offset(std::uint32_t member_id) const noexcept {
    if (member_id < block_hint_offsets16_.size()) {
      block_hint_offsets16_[member_id] = kInvalidBlockHint16;
    }
  }

  void ensure_block_hint_offset_capacity(std::uint32_t member_id) const {
    const auto required = static_cast<size_type>(member_id) + 1;
    if (required > block_hint_offsets16_.size()) {
      block_hint_offsets16_.resize(required, kInvalidBlockHint16);
    }
  }

  void repair_block_hint(std::uint32_t member_id,
                         size_type block_index,
                         size_type offset) const {
    if (!location_cache_enabled() ||
        rank_cache_mode_ != RankCacheMode::BlockHint ||
        block_index >= blocks_.size()) {
      return;
    }

    const auto& block = blocks_[block_index];
    if (offset >= block.size() ||
        block.member_id_at(offset) != member_id) {
      return;
    }

    ensure_block_hint_offset_capacity(member_id);
    block_hint_offsets16_[member_id] = static_cast<std::uint16_t>(offset);
  }

  void set_block_hint(std::uint32_t member_id, std::uint32_t block_id) {
    clear_block_hint_offset(member_id);
    if (block_id >= block_hint_narrow_limit_) {
      promote_block_hints_to_wide();
    }
    ensure_block_hint_capacity(member_id);
    if (block_hints_wide_) {
      block_hints32_[member_id] = block_id;
      return;
    }
    block_hints16_[member_id] = static_cast<std::uint16_t>(block_id);
  }

  [[nodiscard]] std::optional<std::uint32_t> block_hint(
      std::uint32_t member_id) const {
    if (block_hints_wide_) {
      if (member_id >= block_hints32_.size()) {
        return std::nullopt;
      }
      const auto hint = block_hints32_[member_id];
      if (hint == kInvalidBlockHint32) {
        return std::nullopt;
      }
      return hint;
    }

    if (member_id >= block_hints16_.size()) {
      return std::nullopt;
    }
    const auto hint = block_hints16_[member_id];
    if (hint == kInvalidBlockHint16) {
      return std::nullopt;
    }
    return hint;
  }

  void set_location(std::uint32_t member_id,
                    size_type block_index,
                    size_type offset) {
    if (!location_cache_enabled()) {
      return;
    }

    assert(block_index < blocks_.size());
    assign_block_id(blocks_[block_index]);
    assert(offset <= kLocationOffsetMask);
    if (rank_cache_mode_ == RankCacheMode::Exact) {
      ensure_location_capacity(member_id);
      locations_[member_id] = pack_location(blocks_[block_index].id_, offset);
      return;
    }

    set_block_hint(member_id, blocks_[block_index].id_);
  }

  void clear_location(std::uint32_t member_id) noexcept {
    if (!location_cache_enabled()) {
      return;
    }
    if (rank_cache_mode_ == RankCacheMode::Exact) {
      if (member_id < locations_.size()) {
        locations_[member_id] = kInvalidLocation;
      }
      return;
    }
    clear_block_hint_offset(member_id);
    if (block_hints_wide_) {
      if (member_id < block_hints32_.size()) {
        block_hints32_[member_id] = kInvalidBlockHint32;
      }
      return;
    }
    if (member_id < block_hints16_.size()) {
      block_hints16_[member_id] = kInvalidBlockHint16;
    }
  }

  void refresh_block_locations(size_type block_index) {
    refresh_block_locations_from(block_index, 0);
  }

  void refresh_block_locations_from(size_type block_index, size_type first_offset) {
    if (!location_cache_enabled() || block_index >= blocks_.size()) {
      return;
    }

    auto& block = blocks_[block_index];
    assign_block_id(block);
    const auto block_id = block.id_;
    assert(block.size() <= kLocationOffsetMask);
    for (size_type offset = first_offset; offset < block.size(); ++offset) {
      const auto member_id = block.member_id_at(offset);
      if (rank_cache_mode_ == RankCacheMode::Exact) {
        ensure_location_capacity(member_id);
        locations_[member_id] = pack_location(block_id, offset);
      } else {
        set_block_hint(member_id, block_id);
      }
    }
  }

  [[nodiscard]] std::optional<size_type> cached_rank(ZSetScoreEntry value) const {
    if (const auto located = cached_entry_location(value)) {
      return prefix_size(located->first) + located->second;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::pair<size_type, size_type>> cached_entry_location(
      ZSetScoreEntry value) const {
    if (!location_cache_enabled()) {
      return std::nullopt;
    }

    std::uint32_t block_id = 0;
    size_type exact_offset = 0;
    if (rank_cache_mode_ == RankCacheMode::Exact) {
      if (value.member_id >= locations_.size()) {
        return std::nullopt;
      }
      const auto location = locations_[value.member_id];
      if (location == kInvalidLocation) {
        return std::nullopt;
      }
      block_id = location_block_id(location);
      exact_offset = location_offset(location);
    } else {
      const auto hint = block_hint(value.member_id);
      if (!hint) {
        return std::nullopt;
      }
      block_id = *hint;
      if (value.member_id < block_hint_offsets16_.size()) {
        const auto repaired_offset = block_hint_offsets16_[value.member_id];
        if (repaired_offset != kInvalidBlockHint16) {
          exact_offset = repaired_offset;
        }
      }
    }
    if (block_id >= block_index_by_id_.size()) {
      return std::nullopt;
    }

    const auto block_index = static_cast<size_type>(block_index_by_id_[block_id]);
    if (block_index == kInvalidBlockIndex || block_index >= blocks_.size()) {
      return std::nullopt;
    }

    const auto& block = blocks_[block_index];
    if (rank_cache_mode_ == RankCacheMode::Exact) {
      if (exact_offset >= block.size() ||
          block.member_id_at(exact_offset) != value.member_id ||
          block.score_at(exact_offset) != value.score) {
        return std::nullopt;
      }

      return std::pair<size_type, size_type>{block_index, exact_offset};
    }

    if (exact_offset < block.size() &&
        block.member_id_at(exact_offset) == value.member_id &&
        block.score_at(exact_offset) == value.score) {
      return std::pair<size_type, size_type>{block_index, exact_offset};
    }

    if (exact_offset < block.size()) {
      clear_block_hint_offset(value.member_id);
    }

    if (const auto offset = locate_entry_offset(block, value)) {
      repair_block_hint(value.member_id, block_index, *offset);
      return std::pair<size_type, size_type>{block_index, *offset};
    }

    return std::nullopt;
  }

  [[nodiscard]] std::optional<std::pair<size_type, size_type>> locate_entry(
      ZSetScoreEntry value) const {
    if (blocks_.empty()) {
      return std::nullopt;
    }

    auto block_index = lower_block_by_score(value.score);
    while (block_index < blocks_.size()) {
      const auto& block = blocks_[block_index];
      if (block.front().score > value.score) {
        return std::nullopt;
      }
      if (block.back().score < value.score) {
        ++block_index;
        continue;
      }

      if (const auto offset = locate_entry_offset(block, value)) {
        return std::pair<size_type, size_type>{block_index, *offset};
      }

      ++block_index;
    }

    return std::nullopt;
  }

  template <class BlockRef>
  [[nodiscard]] std::optional<size_type> locate_entry_offset(
      BlockRef&& block,
      ZSetScoreEntry value) const noexcept {
    const auto score_start = block.lower_bound_score(value.score);
    if (score_start == block.size() ||
        block.score_at(score_start) != value.score) {
      return std::nullopt;
    }

    auto score_end = score_start;
    while (score_end < block.size() &&
           block.score_at(score_end) == value.score) {
      ++score_end;
    }

    const auto run_length = score_end - score_start;
    if (run_length == 1) {
      if (block.member_id_at(score_start) == value.member_id) {
        return score_start;
      }
      return std::nullopt;
    }

    size_type first = score_start;
    size_type count = run_length;
    while (count > 0) {
      const auto step = count / 2;
      const auto mid = first + step;
      if (less_parts(block.score_at(mid), block.member_id_at(mid), value.score,
                     value.member_id)) {
        first = mid + 1;
        count -= step + 1;
      } else {
        count = step;
      }
    }

    if (first >= score_end || block.member_id_at(first) != value.member_id) {
      return std::nullopt;
    }

    return first;
  }

  [[nodiscard]] std::pair<size_type, size_type> locate_insert_position(
      ZSetScoreEntry value) const {
    assert(!blocks_.empty());

    auto block_index = lower_block_by_score(value.score);
    if (block_index >= blocks_.size()) {
      block_index = blocks_.size() - 1;
      return {block_index, blocks_[block_index].size()};
    }

    while (true) {
      const auto& block = blocks_[block_index];
      if (block.front().score > value.score) {
        return {block_index, 0};
      }
      if (block.back().score < value.score) {
        ++block_index;
        if (block_index >= blocks_.size()) {
          const auto last = blocks_.size() - 1;
          return {last, blocks_[last].size()};
        }
        continue;
      }

      const auto offset = locate_insert_offset(block, value);
      if (offset < block.size()) {
        return {block_index, offset};
      }

      ++block_index;
      if (block_index >= blocks_.size()) {
        const auto last = blocks_.size() - 1;
        return {last, blocks_[last].size()};
      }
    }
  }

  template <class BlockRef>
  [[nodiscard]] size_type locate_insert_offset(BlockRef&& block,
                                               ZSetScoreEntry value) const noexcept {
    const auto score_start = block.lower_bound_score(value.score);
    if (score_start == block.size()) {
      return block.size();
    }
    if (block.score_at(score_start) > value.score) {
      return score_start;
    }

    auto score_end = score_start;
    while (score_end < block.size() &&
           block.score_at(score_end) == value.score) {
      ++score_end;
    }

    const auto run_length = score_end - score_start;
    if (run_length == 1) {
      if (less_parts(value.score, value.member_id,
                     block.score_at(score_start), block.member_id_at(score_start))) {
        return score_start;
      }
      return score_start + 1;
    }

    size_type first = score_start;
    size_type count = run_length;
    while (count > 0) {
      const auto step = count / 2;
      const auto mid = first + step;
      if (less_parts(value.score, value.member_id,
                     block.score_at(mid), block.member_id_at(mid))) {
        count = step;
      } else {
        first = mid + 1;
        count -= step + 1;
      }
    }
    return first;
  }

  [[nodiscard]] bool less(ZSetScoreEntry lhs, ZSetScoreEntry rhs) const noexcept {
    return less_parts(lhs.score, lhs.member_id, rhs.score, rhs.member_id);
  }

  // One-way widen: if `score` doesn't fit the current width, re-encode every
  // block to the narrowest width that does. All blocks share the index's width,
  // so they promote together (see the CoW-promotion danger note in ZSet::add).
  void ensure_score_width(double score) {
    if (score_fits(score, score_width_)) {
      return;
    }
    const auto target = score_width_for(score, score_width_);
    for (auto& block : blocks_) {
      block.promote_scores(target);
    }
    score_width_ = target;
  }

  [[nodiscard]] bool less_parts(double lhs_score,
                                std::uint32_t lhs_member_id,
                                double rhs_score,
                                std::uint32_t rhs_member_id) const noexcept {
    if (lhs_score < rhs_score) {
      return true;
    }
    if (lhs_score > rhs_score) {
      return false;
    }

    assert(members_ != nullptr);
    assert(lhs_member_id < members_->size());
    assert(rhs_member_id < members_->size());
    return members_->view(lhs_member_id) < members_->view(rhs_member_id);
  }

  [[nodiscard]] bool equivalent(ZSetScoreEntry lhs, ZSetScoreEntry rhs) const noexcept {
    return lhs.score == rhs.score && lhs.member_id == rhs.member_id;
  }

  [[nodiscard]] size_type lower_block(ZSetScoreEntry value) const {
    return static_cast<size_type>(
        std::lower_bound(maxes_.begin(), maxes_.end(), value,
                         [this](ZSetScoreEntry lhs, ZSetScoreEntry rhs) {
                           return less(lhs, rhs);
                         }) -
        maxes_.begin());
  }

  [[nodiscard]] size_type lower_block_by_score(double score) const {
    return static_cast<size_type>(
        std::lower_bound(maxes_.begin(), maxes_.end(), score,
                         [](ZSetScoreEntry lhs, double rhs) {
                           return lhs.score < rhs;
                         }) -
        maxes_.begin());
  }

  [[nodiscard]] size_type upper_block(ZSetScoreEntry value) const {
    auto block_index = static_cast<size_type>(
        std::upper_bound(maxes_.begin(), maxes_.end(), value,
                         [this](ZSetScoreEntry lhs, ZSetScoreEntry rhs) {
                           return less(lhs, rhs);
                         }) -
        maxes_.begin());
    if (block_index == blocks_.size()) {
      block_index = blocks_.size() - 1;
    }
    return block_index;
  }

  [[nodiscard]] bool order_valid_at(size_type block_index, size_type offset) const {
    const auto& block = blocks_[block_index];
    const auto value = block.at(offset);
    if (offset > 0 && less(value, block.at(offset - 1))) {
      return false;
    }
    if (offset + 1 < block.size() && less(block.at(offset + 1), value)) {
      return false;
    }
    if (offset == 0 && block_index > 0 &&
        less(value, blocks_[block_index - 1].back())) {
      return false;
    }
    if (offset + 1 == block.size() && block_index + 1 < blocks_.size() &&
        less(blocks_[block_index + 1].front(), value)) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool retarget_member_id_at(size_type block_index,
                                           size_type offset,
                                           std::uint32_t old_member_id,
                                           std::uint32_t new_member_id) {
    if (block_index >= blocks_.size()) {
      return false;
    }

    auto& block = blocks_[block_index];
    if (offset >= block.size() || block.member_id_at(offset) != old_member_id) {
      return false;
    }

    block.set_member_id(offset, new_member_id);
    if (!order_valid_at(block_index, offset)) {
      block.set_member_id(offset, old_member_id);
      return false;
    }

    clear_location(old_member_id);
    set_location(new_member_id, block_index, offset);
    maxes_[block_index] = block.back();
    return true;
  }

  enum class EraseRebalancePolicy {
    Default,
    Remove,
  };

  [[nodiscard]] bool erase_and_retarget_same_block(size_type r_offset,
                                                    size_type l_offset,
                                                    ZSetScoreEntry last_entry,
                                                    std::uint32_t new_member_id,
                                                    size_type block_index) {
    if (r_offset == l_offset || block_index >= blocks_.size()) {
      return false;
    }

    auto& block = blocks_[block_index];
    if (r_offset >= block.size() || l_offset >= block.size()) {
      return false;
    }

    const auto removed_member_id = block.member_id_at(r_offset);
    if (block.member_id_at(l_offset) != last_entry.member_id ||
        block.score_at(l_offset) != last_entry.score) {
      return false;
    }

    clear_location(removed_member_id);
    clear_location(last_entry.member_id);

    block.erase(r_offset);
    --size_;

    auto adj_l = l_offset;
    if (r_offset < l_offset) {
      --adj_l;
    }
    if (adj_l >= block.size()) {
      return false;
    }

    block.set_member_id(adj_l, new_member_id);
    if (!order_valid_at(block_index, adj_l)) {
      return false;
    }

    set_location(new_member_id, block_index, adj_l);
    maxes_[block_index] = block.back();
    update_index(block_index, -1);
    finish_erase_block(block_index, std::min(r_offset, adj_l),
                       EraseRebalancePolicy::Remove);
    return true;
  }

  void erase_at(size_type block_index,
                size_type offset,
                EraseRebalancePolicy policy = EraseRebalancePolicy::Default) {
    auto& block = blocks_[block_index];
    const auto removed_member_id = block.member_id_at(offset);
    block.erase(offset);
    clear_location(removed_member_id);
    --size_;

    if (block.empty()) {
      invalidate_block_id(block.id_);
      blocks_.erase(blocks_.begin() + static_cast<long>(block_index));
      maxes_.erase(maxes_.begin() + static_cast<long>(block_index));
      refresh_block_indices_from(block_index);
      invalidate_index();
      return;
    }

    maxes_[block_index] = block.back();
    update_index(block_index, -1);
    finish_erase_block(block_index, offset, policy);
  }

  void finish_erase_block(size_type block_index,
                          size_type refresh_from,
                          EraseRebalancePolicy policy) {
    const auto& block = blocks_[block_index];
    const auto rebalance_threshold =
        policy == EraseRebalancePolicy::Remove ? (load_ / 4) : (load_ / 2);

    if (block.size() < rebalance_threshold && blocks_.size() > 1) {
      rebalance_after_erase(block_index);
      invalidate_index();
      return;
    }

    if (rank_cache_mode_ == RankCacheMode::Exact) {
      refresh_block_locations_from(block_index, refresh_from);
    }
  }

  void split_block(size_type block_index) {
    auto& block = blocks_[block_index];
    const auto split_at = block.size() / 2;
    auto right = block.split_off(split_at);
    block.trim();
    assign_block_id(right);

    blocks_.insert(blocks_.begin() + static_cast<long>(block_index + 1),
                   std::move(right));
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
    auto right = std::move(blocks_[block_index + 1]);
    const auto removed_block_id = right.id_;
    left.append(std::move(right));

    blocks_.erase(blocks_.begin() + static_cast<long>(block_index + 1));
    maxes_.erase(maxes_.begin() + static_cast<long>(block_index + 1));
    invalidate_block_id(removed_block_id);
    refresh_block_indices_from(block_index + 1);
    maxes_[block_index] = left.back();

    if (left.size() > load_ * 2) {
      split_block(block_index);
      refresh_block_indices_from(block_index + 1);
      refresh_block_locations(block_index);
      refresh_block_locations(block_index + 1);
      return;
    }

    refresh_block_locations(block_index);
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

  const ZSetMemberStorage* members_{nullptr};
  std::vector<Block> blocks_;
  std::vector<ZSetScoreEntry> maxes_;
  mutable std::vector<size_type> index_;
  std::vector<std::uint32_t> locations_;
  std::vector<std::uint16_t> block_hints16_;
  std::vector<std::uint32_t> block_hints32_;
  mutable std::vector<std::uint16_t> block_hint_offsets16_;
  std::vector<std::uint32_t> block_index_by_id_;
  mutable size_type index_offset_{0};
  size_type size_{0};
  ScoreWidth score_width_{ScoreWidth::I16};
  std::uint32_t next_block_id_{0};
  std::uint32_t block_hint_narrow_limit_{kDefaultBlockHintNarrowLimit};
  bool block_hints_wide_{false};
  RankCacheMode rank_cache_mode_{RankCacheMode::Off};
  size_type load_{kDefaultLoad};  // SortedList sublist target size (runtime-tunable)
};

inline ZSetScoreIndex::size_type ZSetScoreIndex::allocated_bytes() const noexcept {
  size_type total = blocks_.capacity() * sizeof(Block) +
                    maxes_.capacity() * sizeof(ZSetScoreEntry) +
                    index_.capacity() * sizeof(size_type) +
                    location_cache_allocated_bytes();
  for (const auto& block : blocks_) {
    total += block.allocated_bytes();
  }
  return total;
}

inline ZSetScoreIndex::size_type ZSetScoreIndex::block_capacity_sum() const noexcept {
  size_type total = 0;
  for (const auto& block : blocks_) {
    total += block.capacity();
  }
  return total;
}

}  // namespace goblin::core
