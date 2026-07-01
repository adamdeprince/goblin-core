#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/zset_member_storage.hpp"

namespace goblin::core {

struct ZSetMemberMeta {
  std::uint32_t member_id{0};
};
static_assert(sizeof(ZSetMemberMeta) == 4);

class ZSetMemberIndex {
 public:
  using size_type = std::size_t;
  using hash_type = std::uint32_t;

  static constexpr size_type kGroupWidth = 16;

  ZSetMemberIndex() = default;

  explicit ZSetMemberIndex(const ZSetMemberStorage* members) : members_(members) {}

  void set_members(const ZSetMemberStorage* members) noexcept {
    members_ = members;
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

  [[nodiscard]] size_type size() const noexcept {
    return size_;
  }

  [[nodiscard]] size_type capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] size_type tombstone_count() const noexcept {
    return tombstones_;
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return control_.capacity() * sizeof(std::uint8_t) +
           slots_.capacity() * sizeof(Slot);
  }

  [[nodiscard]] size_type member_slot_capacity() const noexcept {
    return 0;
  }

  bool cleanup_after_removal_if_needed(size_type removed_count) {
    if (removed_count == 0 || tombstones_ == 0) {
      return false;
    }

    if (size_ == 0) {
      clear();
      return true;
    }

    const auto shrunk_capacity = capacity_for_size(size_);
    if (capacity_ > kGroupWidth && size_ < capacity_ / 4 &&
        shrunk_capacity < capacity_) {
      rehash(shrunk_capacity);
      return true;
    }

    if (tombstones_ >= kMinTombstonesForSameCapacityCleanup &&
        tombstones_ >= size_) {
      rehash(capacity_);
      return true;
    }

    return false;
  }

  bool rehash_same_capacity() {
    if (capacity_ == 0 || tombstones_ == 0) {
      return false;
    }

    rehash(capacity_);
    return true;
  }

  void reserve(size_type expected_size) {
    if (expected_size == 0 || expected_size <= max_usable(capacity_)) {
      return;
    }
    rehash(capacity_for_size(expected_size));
  }

  [[nodiscard]] ZSetMemberMeta* find(std::string_view member) {
    const auto index = find_index(member);
    if (index == npos) {
      return nullptr;
    }
    return &slots_[index].meta;
  }

  [[nodiscard]] const ZSetMemberMeta* find(std::string_view member) const {
    const auto index = find_index(member);
    if (index == npos) {
      return nullptr;
    }
    return &slots_[index].meta;
  }

  [[nodiscard]] bool move_member_id(std::uint32_t old_member_id,
                                    std::uint32_t new_member_id) {
    const auto index = find_index(member_view(old_member_id));
    if (index == npos) {
      return false;
    }
    if (slots_[index].meta.member_id != old_member_id) {
      return false;
    }

    slots_[index].meta.member_id = new_member_id;
    return true;
  }

  std::pair<ZSetMemberMeta*, bool> insert(std::string_view member, ZSetMemberMeta meta) {
    ensure_capacity_for_insert();

    const auto hash = hash_member(member);
    const auto existing = find_index_with_hash(member, hash);
    if (existing != npos) {
      return {&slots_[existing].meta, false};
    }

    const auto index = find_insert_index(hash);
    const auto old_control = control_[index];
    slots_[index].meta = meta;
    set_control(index, fingerprint(hash));
    ++size_;
    if (old_control == kDeleted) {
      --tombstones_;
    }
    return {&slots_[index].meta, true};
  }

  bool erase(std::string_view member) {
    const auto index = find_index(member);
    if (index == npos) {
      return false;
    }

    set_control(index, kDeleted);
    --size_;
    ++tombstones_;
    return true;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        fn(member_view(slots_[i].meta.member_id), slots_[i].meta);
      }
    }
  }

 private:
  struct Slot {
    ZSetMemberMeta meta;
  };

  static constexpr std::uint8_t kEmpty = 0x80;
  static constexpr std::uint8_t kDeleted = 0xFE;
  static constexpr size_type kMinTombstonesForSameCapacityCleanup = 4096;
  static constexpr size_type npos = std::numeric_limits<size_type>::max();

  [[nodiscard]] static bool is_full_control(std::uint8_t control) noexcept {
    return control < kEmpty;
  }

  [[nodiscard]] bool is_full(size_type index) const noexcept {
    return is_full_control(control_[index]);
  }

  [[nodiscard]] static std::uint8_t fingerprint(hash_type hash) noexcept {
    constexpr auto bits = std::numeric_limits<hash_type>::digits;
    return static_cast<std::uint8_t>((hash >> (bits - 7U)) & 0x7FU);
  }

  [[nodiscard]] static size_type first_set_bit(std::uint64_t mask) noexcept {
    return static_cast<size_type>(std::countr_zero(mask));
  }

  [[nodiscard]] static std::uint64_t match_byte(const std::uint8_t* group,
                                                std::uint8_t needle) noexcept {
    std::uint64_t mask = 0;
    for (size_type i = 0; i < kGroupWidth; ++i) {
      mask |= static_cast<std::uint64_t>(group[i] == needle) << i;
    }
    return mask;
  }

  [[nodiscard]] static size_type max_usable(size_type capacity) noexcept {
    return capacity - (capacity / 32);
  }

  [[nodiscard]] static size_type capacity_for_size(size_type expected_size) {
    if (expected_size == 0) {
      return 0;
    }

    auto required = (expected_size * 32 + 30) / 31;
    if (required < kGroupWidth) {
      required = kGroupWidth;
    }
    return std::bit_ceil(required);
  }

  [[nodiscard]] std::string_view member_view(std::uint32_t member_id) const noexcept {
    return members_->view(member_id);
  }

  [[nodiscard]] hash_type hash_member(std::string_view member) const {
    auto hash = static_cast<size_type>(hasher_(member));
    if constexpr (sizeof(size_type) > sizeof(hash_type)) {
      hash ^= hash >> std::numeric_limits<hash_type>::digits;
    }
    return static_cast<hash_type>(hash);
  }

  void clear() {
    control_.clear();
    slots_.clear();
    size_ = 0;
    tombstones_ = 0;
    capacity_ = 0;
  }

  void ensure_capacity_for_insert() {
    if (capacity_ == 0) {
      rehash(kGroupWidth);
      return;
    }

    if (size_ + tombstones_ + 1 > max_usable(capacity_)) {
      if (size_ + 1 <= max_usable(capacity_)) {
        rehash(capacity_);
      } else {
        rehash(capacity_ * 2);
      }
    }
  }

  void allocate_capacity(size_type requested_capacity) {
    capacity_ = std::bit_ceil(requested_capacity < kGroupWidth ? kGroupWidth
                                                               : requested_capacity);
    if (capacity_ > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member index capacity exhausted");
    }
    slots_.clear();
    slots_.resize(capacity_);
    control_.assign(capacity_ + kGroupWidth, kEmpty);
    size_ = 0;
    tombstones_ = 0;
  }

  void set_control(size_type index, std::uint8_t value) noexcept {
    control_[index] = value;
    if (index < kGroupWidth) {
      control_[capacity_ + index] = value;
    }
  }

  [[nodiscard]] size_type find_index(std::string_view member) const {
    if (capacity_ == 0) {
      return npos;
    }
    return find_index_with_hash(member, hash_member(member));
  }

  [[nodiscard]] size_type find_index_with_hash(std::string_view member,
                                               hash_type hash) const {
    const auto needle = fingerprint(hash);
    const auto mask = capacity_ - 1;
    auto group_start = static_cast<size_type>(hash) & mask;

    for (size_type probed = 0; probed < capacity_; probed += kGroupWidth) {
      const auto* group = control_.data() + group_start;
      auto matches = match_byte(group, needle);
      while (matches != 0) {
        const auto offset = first_set_bit(matches);
        auto index = group_start + offset;
        if (index >= capacity_) {
          index -= capacity_;
        }

        if (member_view(slots_[index].meta.member_id) == member) {
          return index;
        }
        matches &= matches - 1;
      }

      if (match_byte(group, kEmpty) != 0) {
        return npos;
      }

      group_start = (group_start + kGroupWidth) & mask;
    }

    return npos;
  }

  [[nodiscard]] size_type find_insert_index(hash_type hash) const {
    const auto mask = capacity_ - 1;
    auto group_start = static_cast<size_type>(hash) & mask;
    auto first_deleted = npos;

    for (size_type probed = 0; probed < capacity_; probed += kGroupWidth) {
      const auto* group = control_.data() + group_start;

      auto deleted = match_byte(group, kDeleted);
      if (first_deleted == npos && deleted != 0) {
        first_deleted = group_start + first_set_bit(deleted);
        if (first_deleted >= capacity_) {
          first_deleted -= capacity_;
        }
      }

      auto empty = match_byte(group, kEmpty);
      if (empty != 0) {
        auto index = group_start + first_set_bit(empty);
        if (index >= capacity_) {
          index -= capacity_;
        }
        return first_deleted == npos ? index : first_deleted;
      }

      group_start = (group_start + kGroupWidth) & mask;
    }

    return first_deleted;
  }

  void insert_existing(Slot slot) {
    const auto hash = hash_member(member_view(slot.meta.member_id));
    const auto index = find_insert_index(hash);
    slots_[index] = slot;
    set_control(index, fingerprint(hash));
    ++size_;
  }

  void rehash(size_type requested_capacity) {
    ZSetMemberIndex replacement(members_);
    replacement.allocate_capacity(requested_capacity);

    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        replacement.insert_existing(slots_[i]);
      }
    }

    control_ = std::move(replacement.control_);
    slots_ = std::move(replacement.slots_);
    size_ = replacement.size_;
    tombstones_ = 0;
    capacity_ = replacement.capacity_;
  }

  const ZSetMemberStorage* members_{nullptr};
  std::vector<std::uint8_t> control_;
  std::vector<Slot> slots_;
  size_type size_{0};
  size_type tombstones_{0};
  size_type capacity_{0};
  [[no_unique_address]] std::hash<std::string_view> hasher_{};
};

}  // namespace goblin::core
