#pragma once

// Adaptive packed-memory array for Redis list order.
//
// Values are represented by compact arena references in struct-of-arrays form.
// A list starts with one u32 tagged logical address plus one u16 length per PMA
// slot. If its byte arena crosses the narrow 2 GiB address space, it promotes
// once to split u32 block/u32 offset/u16 length slots. An occupancy bitmap marks
// live slots; a Fenwick tree over 64-slot bitmap words implements rank/select.
// Inserts first consume nearby slack, then redistribute the smallest sufficiently
// sparse window, and finally grow geometrically. Repeated endpoint operations
// bias redistributed slack toward that endpoint.

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "goblin/core/list_value_arena.hpp"

namespace goblin::core {

class AdaptivePma {
 public:
  using size_type = std::size_t;
  enum class EndpointBias { ByRank, Front, Back };

  static constexpr double kDefaultMaxDensity = 0.97;
  static constexpr double kDefaultResizeGrowth = kDefaultArenaGrowth;

  explicit AdaptivePma(double max_density = kDefaultMaxDensity,
                       double resize_growth = kDefaultResizeGrowth,
                       size_type chunk_bytes =
                           ListValueArena::kDefaultChunkBytes)
      : max_density_(valid_density(max_density) ? max_density
                                                : kDefaultMaxDensity),
        resize_growth_(resize_growth > 1.0 ? resize_growth
                                           : kDefaultResizeGrowth) {
    if (!std::isfinite(resize_growth_)) {
      resize_growth_ = kDefaultResizeGrowth;
    }
    if (!std::has_single_bit(chunk_bytes) ||
        chunk_bytes < ListValueArena::kMinChunkBytes ||
        chunk_bytes > ListValueArena::kMaxChunkBytes) {
      chunk_bytes = ListValueArena::kDefaultChunkBytes;
    }
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
  }

  [[nodiscard]] size_type size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] size_type capacity() const noexcept { return offsets_.size(); }
  [[nodiscard]] bool wide_references() const noexcept {
    return !blocks_.empty();
  }
  [[nodiscard]] size_type front_slack() const noexcept {
    return empty() ? capacity() : select_slot(0);
  }
  [[nodiscard]] size_type back_slack() const noexcept {
    return empty() ? capacity() : capacity() - select_slot(size_ - 1) - 1;
  }
  [[nodiscard]] double max_density() const noexcept { return max_density_; }
  [[nodiscard]] double resize_growth() const noexcept {
    return resize_growth_;
  }

  [[nodiscard]] ListValueRef at(size_type rank) const noexcept {
    assert(rank < size_);
    return read_slot(select_slot(rank));
  }

  void set(size_type rank, ListValueRef value) {
    assert(rank < size_);
    write_slot(select_slot(rank), value);
  }

  void insert(size_type rank, ListValueRef value) {
    if (rank > size_) {
      throw std::out_of_range("PMA insert rank");
    }
    if (size_ == std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("list element space exhausted");
    }
    note_insert(rank);
    if (capacity() == 0) {
      allocate_slots(kInitialCapacity);
      const std::array initial{value};
      redistribute(0, capacity(), initial);
      size_ = 1;
      return;
    }
    if (rank == 0) {
      const auto first = select_slot(0);
      if (first != 0) {
        write_slot(first - 1, value);
        set_occupied(first - 1, true);
        ++size_;
        return;
      }
    }
    if (rank == size_) {
      const auto last = select_slot(size_ - 1);
      if (last + 1 < capacity()) {
        write_slot(last + 1, value);
        set_occupied(last + 1, true);
        ++size_;
        return;
      }
    }

    const auto right_slot = rank < size_ ? select_slot(rank) : capacity();
    const auto left_slot = rank > 0 ? select_slot(rank - 1) : capacity();
    const auto left_gap = left_slot == capacity()
                              ? capacity()
                              : nearby_empty_left(left_slot);
    const auto right_gap = right_slot == capacity()
                               ? capacity()
                               : nearby_empty_right(right_slot);
    const auto left_distance = left_gap == capacity()
                                   ? capacity()
                                   : left_slot - left_gap;
    const auto right_distance = right_gap == capacity()
                                    ? capacity()
                                    : right_gap - right_slot;

    if (left_distance <= right_distance && left_gap != capacity()) {
      for (size_type slot = left_gap; slot < left_slot; ++slot) {
        move_slot(slot, slot + 1);
      }
      write_slot(left_slot, value);
      set_occupied(left_slot, true);
      ++size_;
      return;
    }
    if (right_gap != capacity()) {
      for (size_type slot = right_gap; slot > right_slot; --slot) {
        move_slot(slot, slot - 1);
      }
      write_slot(right_slot, value);
      set_occupied(right_slot, true);
      ++size_;
      return;
    }

    // max_density is the target density after redistribution or growth, not a
    // prohibition against consuming a gap that already exists. In particular,
    // compact() deliberately reserves endpoint and distributed slack. Let a
    // transient insert use that slack instead of growing the entire PMA merely
    // because size_ currently sits exactly on the density watermark.
    if (size_ + 1 > max_occupancy(capacity())) {
      rebuild_with_insert(grown_capacity(size_ + 1), rank, value);
      return;
    }

    if (redistribute_for_insert(rank, value)) {
      return;
    }
    rebuild_with_insert(grown_capacity(size_ + 1), rank, value);
  }

  // Insert one logical batch with a single redistribution/rebuild. `values`
  // are already in their final list order; callers reverse an LPUSH batch.
  void insert_many(size_type rank, std::span<const ListValueRef> values,
                   EndpointBias endpoint = EndpointBias::ByRank) {
    if (rank > size_) {
      throw std::out_of_range("PMA insert rank");
    }
    if (values.empty()) {
      return;
    }
    const auto limit =
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max());
    if (values.size() > limit - size_) {
      throw std::length_error("list element space exhausted");
    }

    note_insert(rank, values.size(), endpoint);
    const auto next_size = size_ + values.size();
    if (capacity() == 0) {
      allocate_slots(minimum_capacity(next_size));
      redistribute(0, capacity(), values);
      size_ = next_size;
      return;
    }
    // Endpoint batches commonly fit directly in explicitly reserved slack.
    // Populate the whole run before touching size_ so ranks remain stable.
    if (rank == 0) {
      const auto first = select_slot(0);
      if (first >= values.size()) {
        const auto begin = first - values.size();
        for (size_type index = 0; index < values.size(); ++index) {
          write_slot(begin + index, values[index]);
          set_occupied(begin + index, true);
        }
        size_ = next_size;
        return;
      }
    }
    if (rank == size_) {
      const auto last = select_slot(size_ - 1);
      if (capacity() - last - 1 >= values.size()) {
        for (size_type index = 0; index < values.size(); ++index) {
          write_slot(last + 1 + index, values[index]);
          set_occupied(last + 1 + index, true);
        }
        size_ = next_size;
        return;
      }
    }

    if (next_size > max_occupancy(capacity())) {
      rebuild_with_insert_many(grown_capacity(next_size), rank, values);
      return;
    }

    if (redistribute_for_insert_many(rank, values)) {
      return;
    }
    rebuild_with_insert_many(grown_capacity(next_size), rank, values);
  }

  [[nodiscard]] ListValueRef erase(size_type rank) {
    if (rank >= size_) {
      throw std::out_of_range("PMA erase rank");
    }
    const auto slot = select_slot(rank);
    const auto removed = read_slot(slot);
    set_occupied(slot, false);
    --size_;
    note_erase(rank);
    if (size_ == 0) {
      clear();
    } else {
      maybe_shrink();
    }
    return removed;
  }

  void clear() noexcept {
    blocks_.clear();
    offsets_.clear();
    lengths_.clear();
    occupied_.clear();
    fenwick_.clear();
    size_ = 0;
    front_bias_ = 1;
    back_bias_ = 1;
  }

  // Drop vector over-allocation and redistribute at the minimum capacity that
  // still respects max_density. This is the explicit GOBLIN.OPTIMIZE path.
  void compact() {
    if (size_ == 0) {
      clear();
      return;
    }
    rebuild(minimum_capacity(size_));
    if (wide_references()) {
      blocks_.shrink_to_fit();
    }
    offsets_.shrink_to_fit();
    lengths_.shrink_to_fit();
    occupied_.shrink_to_fit();
    fenwick_.shrink_to_fit();
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (size_type slot = 0; slot < capacity(); ++slot) {
      if (is_occupied(slot)) {
        fn(read_slot(slot));
      }
    }
  }

  template <class Fn>
  void for_range(size_type first, size_type count, Fn&& fn) const {
    if (first >= size_ || count == 0) {
      return;
    }
    auto remaining = std::min(count, size_ - first);
    for (size_type slot = select_slot(first);
         slot < capacity() && remaining != 0; ++slot) {
      if (is_occupied(slot)) {
        fn(read_slot(slot));
        --remaining;
      }
    }
  }

  template <class Predicate>
  [[nodiscard]] std::optional<size_type> find_first(
      size_type first, Predicate&& predicate) const {
    if (first >= size_) {
      return std::nullopt;
    }
    auto rank = first;
    for (size_type slot = select_slot(first); slot < capacity(); ++slot) {
      if (is_occupied(slot)) {
        if (predicate(read_slot(slot))) {
          return rank;
        }
        ++rank;
      }
    }
    return std::nullopt;
  }

  template <class Predicate>
  [[nodiscard]] std::optional<size_type> find_last(
      size_type end, Predicate&& predicate) const {
    end = std::min(end, size_);
    if (end == 0) {
      return std::nullopt;
    }
    auto rank = end - 1;
    for (size_type cursor = select_slot(rank) + 1; cursor != 0;) {
      const auto slot = --cursor;
      if (!is_occupied(slot)) {
        continue;
      }
      if (predicate(read_slot(slot))) {
        return rank;
      }
      if (rank == 0) {
        return std::nullopt;
      }
      --rank;
    }
    return std::nullopt;
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return offsets_.capacity() * sizeof(std::uint32_t) +
           blocks_.capacity() * sizeof(std::uint32_t) +
           lengths_.capacity() * sizeof(std::uint16_t) +
           occupied_.capacity() * sizeof(std::uint64_t) +
           fenwick_.capacity() * sizeof(std::uint32_t);
  }

  [[nodiscard]] bool check_invariants() const noexcept {
    if (capacity() == 0) {
      return size_ == 0 && blocks_.empty() && lengths_.empty() &&
             occupied_.empty() && fenwick_.empty();
    }
    if ((!blocks_.empty() && blocks_.size() != offsets_.size()) ||
        offsets_.size() != lengths_.size() ||
        occupied_.size() != word_count(capacity()) ||
        fenwick_.size() != occupied_.size() + 1) {
      return false;
    }
    size_type counted = 0;
    for (const auto word : occupied_) {
      counted += std::popcount(word);
    }
    if (counted != size_) {
      return false;
    }
    for (size_type rank = 0; rank < size_; ++rank) {
      const auto slot = select_slot(rank);
      if (!is_occupied(slot) || rank_before(slot) != rank) {
        return false;
      }
    }
    return true;
  }

 private:
  static constexpr size_type kInitialCapacity = 8;
  static constexpr size_type kNearbyShiftLimit = 64;
  static constexpr size_type kInitialWindow = 64;
  static constexpr std::uint8_t kMaxEndpointBias = 8;

  [[nodiscard]] static bool valid_density(double value) noexcept {
    return value > 0.0 && value <= 1.0 && std::isfinite(value);
  }

  [[nodiscard]] static size_type word_count(size_type slots) noexcept {
    return (slots + 63) / 64;
  }

  [[nodiscard]] size_type max_occupancy(size_type slots) const noexcept {
    if (slots == 0) {
      return 0;
    }
    const auto count = static_cast<size_type>(
        std::floor(static_cast<long double>(slots) * max_density_));
    return std::max<size_type>(1, count);
  }

  [[nodiscard]] size_type minimum_capacity(size_type count) const {
    if (count == 0) {
      return 0;
    }
    const auto required_real =
        std::ceil(static_cast<long double>(count) / max_density_);
    if (required_real > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("list PMA capacity exhausted");
    }
    const auto required = static_cast<size_type>(required_real);
    return std::max(kInitialCapacity, required);
  }

  [[nodiscard]] size_type grown_capacity(size_type count) const {
    const auto geometric_real =
        std::ceil(static_cast<long double>(capacity()) * resize_growth_);
    if (geometric_real > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("list PMA capacity exhausted");
    }
    const auto geometric = static_cast<size_type>(geometric_real);
    auto result = std::max(minimum_capacity(count), geometric);
    if (result <= capacity()) {
      result = capacity() + 1;
    }
    if (result > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("list PMA capacity exhausted");
    }
    return result;
  }

  void allocate_slots(size_type slots) {
    const bool wide = wide_references();
    offsets_.assign(slots, 0);
    lengths_.assign(slots, 0);
    if (wide) {
      blocks_.assign(slots, 0);
    } else {
      blocks_.clear();
    }
    occupied_.assign(word_count(slots), 0);
    fenwick_.assign(occupied_.size() + 1, 0);
  }

  [[nodiscard]] bool is_occupied(size_type slot) const noexcept {
    return (occupied_[slot / 64] & (std::uint64_t{1} << (slot % 64))) != 0;
  }

  void set_occupied(size_type slot, bool occupied) noexcept {
    const auto word_index = slot / 64;
    const auto mask = std::uint64_t{1} << (slot % 64);
    const bool was_occupied = (occupied_[word_index] & mask) != 0;
    if (was_occupied == occupied) {
      return;
    }
    if (occupied) {
      occupied_[word_index] |= mask;
    } else {
      occupied_[word_index] &= ~mask;
    }
    for (size_type node = word_index + 1; node < fenwick_.size();
         node += node & (~node + 1)) {
      if (occupied) {
        ++fenwick_[node];
      } else {
        assert(fenwick_[node] != 0);
        --fenwick_[node];
      }
    }
  }

  void rebuild_fenwick() noexcept {
    fenwick_.assign(occupied_.size() + 1, 0);
    for (size_type word = 0; word < occupied_.size(); ++word) {
      const auto count = static_cast<std::uint32_t>(std::popcount(occupied_[word]));
      for (size_type node = word + 1; node < fenwick_.size();
           node += node & (~node + 1)) {
        fenwick_[node] += count;
      }
    }
  }

  [[nodiscard]] size_type fenwick_prefix(size_type words) const noexcept {
    std::uint32_t sum = 0;
    for (size_type node = words; node != 0; node &= node - 1) {
      sum += fenwick_[node];
    }
    return sum;
  }

  [[nodiscard]] size_type rank_before(size_type slot) const noexcept {
    const auto word = slot / 64;
    const auto bit = slot % 64;
    auto result = fenwick_prefix(word);
    if (bit != 0 && word < occupied_.size()) {
      result += std::popcount(occupied_[word] & ((std::uint64_t{1} << bit) - 1));
    }
    return result;
  }

  [[nodiscard]] size_type select_slot(size_type rank) const noexcept {
    assert(rank < size_);
    auto remaining = static_cast<std::uint32_t>(rank + 1);
    size_type prefix_words = 0;
    for (size_type step = std::bit_floor(occupied_.size()); step != 0;
         step >>= 1) {
      const auto next = prefix_words + step;
      if (next < fenwick_.size() && fenwick_[next] < remaining) {
        prefix_words = next;
        remaining -= fenwick_[next];
      }
    }
    assert(prefix_words < occupied_.size());
    auto bits = occupied_[prefix_words];
    for (std::uint32_t n = 1; n < remaining; ++n) {
      bits &= bits - 1;
    }
    return prefix_words * 64 + std::countr_zero(bits);
  }

  [[nodiscard]] ListValueRef read_slot(size_type slot) const noexcept {
    if (wide_references()) {
      return {.block = blocks_[slot],
              .offset = offsets_[slot],
              .length = lengths_[slot]};
    }
    const auto tagged = offsets_[slot];
    const auto logical = tagged & ListValueRef::kBlockMask;
    const auto raw = tagged & ListValueRef::kRawMask;
    return {.block = static_cast<std::uint32_t>(logical >> chunk_shift_) | raw,
            .offset = static_cast<std::uint32_t>(logical & chunk_mask_),
            .length = lengths_[slot]};
  }

  [[nodiscard]] bool fits_narrow(ListValueRef value) const noexcept {
    const auto logical =
        (static_cast<std::uint64_t>(value.block_index()) << chunk_shift_) |
        value.offset;
    return logical <= ListValueRef::kBlockMask;
  }

  [[nodiscard]] std::uint32_t narrow_location(
      ListValueRef value) const noexcept {
    assert(fits_narrow(value));
    const auto logical =
        (static_cast<std::uint64_t>(value.block_index()) << chunk_shift_) |
        value.offset;
    return static_cast<std::uint32_t>(logical) |
           (value.raw_prefix_omitted() ? ListValueRef::kRawMask : 0);
  }

  void promote_references() {
    assert(!wide_references());
    blocks_.resize(capacity());
    for (size_type slot = 0; slot < capacity(); ++slot) {
      const auto tagged = offsets_[slot];
      const auto logical = tagged & ListValueRef::kBlockMask;
      blocks_[slot] =
          static_cast<std::uint32_t>(logical >> chunk_shift_) |
          (tagged & ListValueRef::kRawMask);
      offsets_[slot] = static_cast<std::uint32_t>(logical & chunk_mask_);
    }
  }

  void write_slot(size_type slot, ListValueRef value) {
    if (!wide_references() && !fits_narrow(value)) {
      promote_references();
    }
    if (wide_references()) {
      blocks_[slot] = value.block;
      offsets_[slot] = value.offset;
    } else {
      offsets_[slot] = narrow_location(value);
    }
    lengths_[slot] = value.length;
  }

  void move_slot(size_type dst, size_type src) noexcept {
    assert(is_occupied(src));
    assert(!is_occupied(dst));
    if (wide_references()) {
      blocks_[dst] = blocks_[src];
    }
    offsets_[dst] = offsets_[src];
    lengths_[dst] = lengths_[src];
    set_occupied(dst, true);
    set_occupied(src, false);
  }

  [[nodiscard]] size_type nearby_empty_left(size_type from) const noexcept {
    const auto floor = from > kNearbyShiftLimit ? from - kNearbyShiftLimit : 0;
    for (size_type slot = from; slot > floor; --slot) {
      if (!is_occupied(slot - 1)) {
        return slot - 1;
      }
    }
    return capacity();
  }

  [[nodiscard]] size_type nearby_empty_right(size_type from) const noexcept {
    const auto stop = std::min(capacity(), from + kNearbyShiftLimit + 1);
    for (size_type slot = from; slot < stop; ++slot) {
      if (!is_occupied(slot)) {
        return slot;
      }
    }
    return capacity();
  }

  [[nodiscard]] std::vector<ListValueRef> values_in(size_type begin,
                                                     size_type end) const {
    std::vector<ListValueRef> values;
    values.reserve(rank_before(end) - rank_before(begin));
    for (size_type slot = begin; slot < end; ++slot) {
      if (is_occupied(slot)) {
        values.push_back(read_slot(slot));
      }
    }
    return values;
  }

  [[nodiscard]] std::vector<ListValueRef> all_values() const {
    return values_in(0, capacity());
  }

  void clear_occupancy(size_type begin, size_type end) noexcept {
    for (size_type slot = begin; slot < end; ++slot) {
      occupied_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64));
    }
  }

  // Distribute every supplied value into [begin,end), preserving order. Half of
  // the available slack is reserved explicitly for active global endpoints;
  // the remainder is spread across every gap. A small endpoint weight among
  // n+1 gaps rounds to nothing on a large list, whereas an explicit quota makes
  // sustained stack/queue traffic consume thousands of slots before rebalance.
  void redistribute(size_type begin, size_type end,
                    std::span<const ListValueRef> values) {
    assert(end <= capacity() && begin <= end);
    const auto width = end - begin;
    assert(values.size() <= width);
    clear_occupancy(begin, end);
    if (values.empty()) {
      rebuild_fenwick();
      return;
    }

    const auto empty_slots = width - values.size();
    const auto front_strength = static_cast<size_type>(
        begin == 0 && front_bias_ > 1 ? front_bias_ - 1 : 0);
    const auto back_strength = static_cast<size_type>(
        end == capacity() && back_bias_ > 1 ? back_bias_ - 1 : 0);
    const auto endpoint_strength = front_strength + back_strength;
    const auto endpoint_budget = endpoint_strength == 0
                                     ? size_type{0}
                                     : (empty_slots + 1) / 2;
    const auto front_reserved = endpoint_strength == 0
                                    ? size_type{0}
                                    : endpoint_budget * front_strength /
                                          endpoint_strength;
    const auto back_reserved = endpoint_budget - front_reserved;
    const auto spread_empty = empty_slots - endpoint_budget;
    size_type assigned_spread = 0;
    size_type slot = begin;

    for (size_type index = 0; index <= values.size(); ++index) {
      const auto target_spread = static_cast<size_type>(
          static_cast<std::uint64_t>(spread_empty) * (index + 1) /
          (values.size() + 1));
      auto gap = target_spread - assigned_spread;
      assigned_spread = target_spread;
      if (index == 0) {
        gap += front_reserved;
      }
      if (index == values.size()) {
        gap += back_reserved;
      }
      slot += gap;
      if (index != values.size()) {
        assert(slot < end);
        write_slot(slot, values[index]);
        occupied_[slot / 64] |= std::uint64_t{1} << (slot % 64);
        ++slot;
      }
    }
    rebuild_fenwick();
  }

  [[nodiscard]] bool redistribute_for_insert(size_type rank,
                                              ListValueRef value) {
    const auto target = rank < size_ ? select_slot(rank)
                                     : select_slot(size_ - 1);
    auto width = std::min(kInitialWindow, capacity());
    while (width <= capacity()) {
      auto begin = (target / width) * width;
      if (begin + width > capacity()) {
        begin = capacity() - width;
      }
      const auto end = begin + width;
      auto values = values_in(begin, end);
      const auto before = rank_before(begin);
      if (rank >= before && rank - before <= values.size() &&
          values.size() + 1 <= max_occupancy(width)) {
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(rank - before),
                      value);
        redistribute(begin, end, values);
        ++size_;
        return true;
      }
      if (width == capacity()) {
        break;
      }
      width = width > capacity() / 2 ? capacity() : width * 2;
    }
    return false;
  }

  [[nodiscard]] bool redistribute_for_insert_many(
      size_type rank, std::span<const ListValueRef> inserted) {
    const auto target = rank < size_ ? select_slot(rank)
                                     : select_slot(size_ - 1);
    auto width = std::min(kInitialWindow, capacity());
    while (width <= capacity()) {
      auto begin = (target / width) * width;
      if (begin + width > capacity()) {
        begin = capacity() - width;
      }
      const auto end = begin + width;
      auto values = values_in(begin, end);
      const auto before = rank_before(begin);
      const auto local_limit = max_occupancy(width);
      if (rank >= before && rank - before <= values.size() &&
          values.size() <= local_limit &&
          inserted.size() <= local_limit - values.size()) {
        values.insert(values.begin() + static_cast<std::ptrdiff_t>(rank - before),
                      inserted.begin(), inserted.end());
        redistribute(begin, end, values);
        size_ += inserted.size();
        return true;
      }
      if (width == capacity()) {
        break;
      }
      width = width > capacity() / 2 ? capacity() : width * 2;
    }
    return false;
  }

  void rebuild_with_insert(size_type slots, size_type rank, ListValueRef value) {
    auto values = all_values();
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(rank), value);
    allocate_slots(slots);
    redistribute(0, slots, values);
    size_ = values.size();
  }

  void rebuild_with_insert_many(size_type slots, size_type rank,
                                std::span<const ListValueRef> inserted) {
    auto values = all_values();
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(rank),
                  inserted.begin(), inserted.end());
    allocate_slots(slots);
    redistribute(0, slots, values);
    size_ = values.size();
  }

  void rebuild(size_type slots) {
    auto values = all_values();
    allocate_slots(slots);
    redistribute(0, slots, values);
    size_ = values.size();
  }

  void maybe_shrink() {
    const auto target = minimum_capacity(size_);
    // Two geometric steps of hysteresis keep a push/pop pair at the density
    // boundary from growing and immediately shrinking the entire PMA. Shrink
    // one step at a time, retaining one step of insertion slack afterward.
    const auto shrink_threshold = static_cast<size_type>(std::floor(
        static_cast<long double>(capacity()) / resize_growth_ /
        resize_growth_));
    if (target <= shrink_threshold && target < capacity()) {
      const auto one_step = static_cast<size_type>(std::ceil(
          static_cast<long double>(capacity()) / resize_growth_));
      const auto next_capacity =
          std::min(capacity() - 1, std::max(target, one_step));
      rebuild(next_capacity);
    }
  }

  void note_insert(size_type rank, size_type count = 1,
                   EndpointBias endpoint = EndpointBias::ByRank) noexcept {
    const auto bump = static_cast<std::uint8_t>(
        std::min<size_type>(count, kMaxEndpointBias));
    const auto decay = [count](std::uint8_t& bias) {
      const auto amount = static_cast<std::uint8_t>(
          std::min<size_type>(count, bias > 1 ? bias - 1 : 0));
      bias -= amount;
    };
    const bool front = endpoint == EndpointBias::Front ||
                       (endpoint == EndpointBias::ByRank && rank == 0);
    const bool back = endpoint == EndpointBias::Back ||
                      (endpoint == EndpointBias::ByRank && rank == size_ &&
                       rank != 0);
    if (front) {
      front_bias_ = std::min<std::uint8_t>(
          kMaxEndpointBias, static_cast<std::uint8_t>(front_bias_ + bump));
      decay(back_bias_);
    } else if (back) {
      back_bias_ = std::min<std::uint8_t>(
          kMaxEndpointBias, static_cast<std::uint8_t>(back_bias_ + bump));
      decay(front_bias_);
    } else {
      decay(front_bias_);
      decay(back_bias_);
    }
  }

  void note_erase(size_type rank) noexcept {
    if (rank == 0) {
      front_bias_ = std::min<std::uint8_t>(kMaxEndpointBias, front_bias_ + 1);
    } else if (rank == size_) {
      back_bias_ = std::min<std::uint8_t>(kMaxEndpointBias, back_bias_ + 1);
    }
  }

  std::vector<std::uint32_t> blocks_;
  std::vector<std::uint32_t> offsets_;
  std::vector<std::uint16_t> lengths_;
  std::vector<std::uint64_t> occupied_;
  std::vector<std::uint32_t> fenwick_;  // one-based counts per bitmap word
  size_type size_{0};
  double max_density_{kDefaultMaxDensity};
  double resize_growth_{kDefaultResizeGrowth};
  std::uint8_t front_bias_{1};
  std::uint8_t back_bias_{1};
  size_type chunk_shift_{21};
  size_type chunk_mask_{ListValueArena::kDefaultChunkBytes - 1};
};

}  // namespace goblin::core
