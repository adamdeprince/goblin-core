#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace goblin::core {

// Transparent functors for SwissTable<std::string, T>: hash and compare keys
// from std::string_view (or other string-like types) without allocating a
// std::string on lookup.
struct StringTableHash {
  using is_transparent = void;

  template <class T>
    requires std::constructible_from<std::string_view, const T&>
  [[nodiscard]] std::size_t operator()(const T& key) const noexcept {
    return std::hash<std::string_view>{}(std::string_view(key));
  }
};

struct StringTableEqual {
  using is_transparent = void;

  template <class Lhs, class Rhs>
    requires std::constructible_from<std::string_view, const Lhs&> &&
             std::constructible_from<std::string_view, const Rhs&>
  [[nodiscard]] bool operator()(const Lhs& lhs, const Rhs& rhs) const noexcept {
    return std::string_view(lhs) == std::string_view(rhs);
  }
};

template <class Key,
          class T,
          class Hash = std::hash<Key>,
          class KeyEqual = std::equal_to<Key>,
          std::size_t GroupWidth = 16,
          class Allocator = std::allocator<std::pair<Key, T>>>
class SwissTable {
  static_assert(GroupWidth > 0 && GroupWidth <= 64);
  static_assert(std::has_single_bit(GroupWidth));

 public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<Key, T>;
  using size_type = std::size_t;
  using hasher = Hash;
  using key_equal = KeyEqual;
  using allocator_type = Allocator;

  SwissTable() = default;

  explicit SwissTable(size_type expected_size) {
    reserve(expected_size);
  }

  SwissTable(Hash hash, KeyEqual equal = KeyEqual{}, Allocator allocator = Allocator{})
      : hasher_(std::move(hash)),
        equal_(std::move(equal)),
        allocator_(std::move(allocator)) {}

  SwissTable(const SwissTable& other)
      : hasher_(other.hasher_),
        equal_(other.equal_),
        allocator_(std::allocator_traits<Allocator>::
                       select_on_container_copy_construction(other.allocator_)) {
    reserve(other.size_);
    for (size_type i = 0; i < other.capacity_; ++i) {
      if (other.is_full(i)) {
        insert_existing(*other.slot_ptr(i));
      }
    }
  }

  SwissTable(SwissTable&& other) noexcept
      : control_(std::move(other.control_)),
        slots_(std::exchange(other.slots_, nullptr)),
        size_(std::exchange(other.size_, 0)),
        tombstones_(std::exchange(other.tombstones_, 0)),
        capacity_(std::exchange(other.capacity_, 0)),
        hasher_(std::move(other.hasher_)),
        equal_(std::move(other.equal_)),
        allocator_(std::move(other.allocator_)) {}

  SwissTable& operator=(const SwissTable& other) {
    if (this == &other) {
      return *this;
    }

    SwissTable copy(other);
    swap(copy);
    return *this;
  }

  SwissTable& operator=(SwissTable&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    destroy_and_deallocate();
    control_ = std::move(other.control_);
    slots_ = std::exchange(other.slots_, nullptr);
    size_ = std::exchange(other.size_, 0);
    tombstones_ = std::exchange(other.tombstones_, 0);
    capacity_ = std::exchange(other.capacity_, 0);
    hasher_ = std::move(other.hasher_);
    equal_ = std::move(other.equal_);
    allocator_ = std::move(other.allocator_);
    return *this;
  }

  ~SwissTable() {
    destroy_and_deallocate();
  }

  void swap(SwissTable& other) noexcept {
    using std::swap;
    swap(control_, other.control_);
    swap(slots_, other.slots_);
    swap(size_, other.size_);
    swap(tombstones_, other.tombstones_);
    swap(capacity_, other.capacity_);
    swap(hasher_, other.hasher_);
    swap(equal_, other.equal_);
    swap(allocator_, other.allocator_);
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

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return control_.capacity() * sizeof(std::uint8_t) +
           capacity_ * sizeof(value_type);
  }

  template <class K>
  [[nodiscard]] bool contains(const K& key) const {
    return find(key) != nullptr;
  }

  template <class K>
  [[nodiscard]] mapped_type* find(const K& key) {
    const auto index = find_index_with_hash(key, hash_key(key));
    if (index == npos) {
      return nullptr;
    }
    return std::addressof(slot_ptr(index)->second);
  }

  template <class K>
  [[nodiscard]] const mapped_type* find(const K& key) const {
    const auto index = find_index_with_hash(key, hash_key(key));
    if (index == npos) {
      return nullptr;
    }
    return std::addressof(slot_ptr(index)->second);
  }

  template <class K>
  [[nodiscard]] value_type* find_entry(const K& key) {
    const auto index = find_index_with_hash(key, hash_key(key));
    if (index == npos) {
      return nullptr;
    }
    return slot_ptr(index);
  }

  template <class K>
  [[nodiscard]] const value_type* find_entry(const K& key) const {
    const auto index = find_index_with_hash(key, hash_key(key));
    if (index == npos) {
      return nullptr;
    }
    return slot_ptr(index);
  }

  template <class Fn>
  void for_each(Fn&& fn) {
    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        fn(*slot_ptr(i));
      }
    }
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        fn(*slot_ptr(i));
      }
    }
  }

  template <class K, class... Args>
  std::pair<mapped_type*, bool> try_emplace(K&& key, Args&&... args) {
    ensure_capacity_for_insert();

    const auto hash = hash_key(key);
    const auto existing = find_index_with_hash(key, hash);
    if (existing != npos) {
      return {std::addressof(slot_ptr(existing)->second), false};
    }

    const auto index = find_insert_index(hash);
    const auto old_control = control_[index];
    std::construct_at(slot_ptr(index),
                      std::piecewise_construct,
                      std::forward_as_tuple(std::forward<K>(key)),
                      std::forward_as_tuple(std::forward<Args>(args)...));
    set_control(index, fingerprint(hash));
    ++size_;
    if (old_control == kDeleted) {
      --tombstones_;
    }
    return {std::addressof(slot_ptr(index)->second), true};
  }

  template <class K>
  std::pair<mapped_type*, bool> insert_or_assign(K&& key, T value) {
    ensure_capacity_for_insert();

    const auto hash = hash_key(key);
    const auto existing = find_index_with_hash(key, hash);
    if (existing != npos) {
      slot_ptr(existing)->second = std::move(value);
      return {std::addressof(slot_ptr(existing)->second), false};
    }

    const auto index = find_insert_index(hash);
    const auto old_control = control_[index];
    std::construct_at(slot_ptr(index), std::forward<K>(key), std::move(value));
    set_control(index, fingerprint(hash));
    ++size_;
    if (old_control == kDeleted) {
      --tombstones_;
    }
    return {std::addressof(slot_ptr(index)->second), true};
  }

  mapped_type& operator[](const Key& key) {
    return *try_emplace(key).first;
  }

  mapped_type& operator[](Key&& key) {
    return *try_emplace(std::move(key)).first;
  }

  template <class K>
  bool erase(const K& key) {
    const auto index = find_index_with_hash(key, hash_key(key));
    if (index == npos) {
      return false;
    }

    std::destroy_at(slot_ptr(index));
    set_control(index, kDeleted);
    --size_;
    ++tombstones_;
    return true;
  }

  void clear() noexcept {
    destroy_slots();
    if (!control_.empty()) {
      std::fill(control_.begin(), control_.end(), kEmpty);
    }
    size_ = 0;
    tombstones_ = 0;
  }

  void reserve(size_type expected_size) {
    if (expected_size <= max_usable(capacity_)) {
      return;
    }
    rehash(capacity_for_size(expected_size));
  }

 private:
  using alloc_traits = std::allocator_traits<Allocator>;

  static constexpr std::uint8_t kEmpty = 0x80;
  static constexpr std::uint8_t kDeleted = 0xFE;
  static constexpr size_type npos = std::numeric_limits<size_type>::max();

  [[nodiscard]] static bool is_full_control(std::uint8_t control) noexcept {
    return control < kEmpty;
  }

  [[nodiscard]] bool is_full(size_type index) const noexcept {
    return is_full_control(control_[index]);
  }

  [[nodiscard]] value_type* slot_ptr(size_type index) noexcept {
    return slots_ + index;
  }

  [[nodiscard]] const value_type* slot_ptr(size_type index) const noexcept {
    return slots_ + index;
  }

  template <class K>
  [[nodiscard]] size_type hash_key(const K& key) const {
    return static_cast<size_type>(hasher_(key));
  }

  [[nodiscard]] static std::uint8_t fingerprint(size_type hash) noexcept {
    constexpr auto bits = std::numeric_limits<size_type>::digits;
    return static_cast<std::uint8_t>((hash >> (bits - 7U)) & 0x7FU);
  }

  [[nodiscard]] static size_type first_set_bit(std::uint64_t mask) noexcept {
    return static_cast<size_type>(std::countr_zero(mask));
  }

  [[nodiscard]] static std::uint64_t match_byte(const std::uint8_t* group,
                                                std::uint8_t needle) noexcept {
    // Fixed-width, branch-light byte comparison intended to auto-vectorize.
    std::uint64_t mask = 0;
    for (size_type i = 0; i < GroupWidth; ++i) {
      mask |= static_cast<std::uint64_t>(group[i] == needle) << i;
    }
    return mask;
  }

  [[nodiscard]] static size_type max_usable(size_type capacity) noexcept {
    return capacity - (capacity / 8);
  }

  [[nodiscard]] static size_type capacity_for_size(size_type expected_size) {
    if (expected_size == 0) {
      return 0;
    }

    const auto min_capacity = GroupWidth;
    auto required = (expected_size * 8 + 6) / 7;
    if (required < min_capacity) {
      required = min_capacity;
    }
    return std::bit_ceil(required);
  }

  void ensure_capacity_for_insert() {
    if (capacity_ == 0) {
      rehash(GroupWidth);
      return;
    }

    if (size_ + tombstones_ + 1 > max_usable(capacity_)) {
      rehash(capacity_ * 2);
    }
  }

  void allocate_capacity(size_type requested_capacity) {
    if (requested_capacity == 0) {
      return;
    }

    capacity_ = std::bit_ceil(requested_capacity < GroupWidth ? GroupWidth
                                                             : requested_capacity);
    slots_ = alloc_traits::allocate(allocator_, capacity_);
    control_.assign(capacity_ + GroupWidth, kEmpty);
  }

  void destroy_slots() noexcept {
    if (slots_ == nullptr) {
      return;
    }

    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        std::destroy_at(slot_ptr(i));
      }
    }
  }

  void destroy_and_deallocate() noexcept {
    destroy_slots();
    if (slots_ != nullptr) {
      alloc_traits::deallocate(allocator_, slots_, capacity_);
    }
    slots_ = nullptr;
    capacity_ = 0;
    size_ = 0;
    tombstones_ = 0;
    control_.clear();
  }

  void set_control(size_type index, std::uint8_t value) noexcept {
    control_[index] = value;
    if (index < GroupWidth) {
      control_[capacity_ + index] = value;
    }
  }

  template <class K>
  [[nodiscard]] size_type find_index_with_hash(const K& key, size_type hash) const {
    if (capacity_ == 0) {
      return npos;
    }

    const auto needle = fingerprint(hash);
    const auto mask = capacity_ - 1;
    auto group_start = hash & mask;

    for (size_type probed = 0; probed < capacity_; probed += GroupWidth) {
      const auto* group = control_.data() + group_start;
      auto matches = match_byte(group, needle);
      while (matches != 0) {
        const auto offset = first_set_bit(matches);
        auto index = group_start + offset;
        if (index >= capacity_) {
          index -= capacity_;
        }

        if (equal_(slot_ptr(index)->first, key)) {
          return index;
        }
        matches &= matches - 1;
      }

      if (match_byte(group, kEmpty) != 0) {
        return npos;
      }

      group_start = (group_start + GroupWidth) & mask;
    }

    return npos;
  }

  [[nodiscard]] size_type find_insert_index(size_type hash) const {
    const auto mask = capacity_ - 1;
    auto group_start = hash & mask;
    auto first_deleted = npos;

    for (size_type probed = 0; probed < capacity_; probed += GroupWidth) {
      const auto* group = control_.data() + group_start;

      auto deleted = match_byte(group, kDeleted);
      if (first_deleted == npos && deleted != 0) {
        const auto offset = first_set_bit(deleted);
        first_deleted = group_start + offset;
        if (first_deleted >= capacity_) {
          first_deleted -= capacity_;
        }
      }

      auto empty = match_byte(group, kEmpty);
      if (empty != 0) {
        const auto offset = first_set_bit(empty);
        auto index = group_start + offset;
        if (index >= capacity_) {
          index -= capacity_;
        }
        return first_deleted == npos ? index : first_deleted;
      }

      group_start = (group_start + GroupWidth) & mask;
    }

    return first_deleted;
  }

  void insert_existing(const value_type& value) {
    const auto hash = hash_key(value.first);
    const auto index = find_insert_index(hash);
    std::construct_at(slot_ptr(index), value);
    set_control(index, fingerprint(hash));
    ++size_;
  }

  void insert_existing(value_type&& value) {
    const auto hash = hash_key(value.first);
    const auto index = find_insert_index(hash);
    std::construct_at(slot_ptr(index), std::move(value));
    set_control(index, fingerprint(hash));
    ++size_;
  }

  void rehash(size_type requested_capacity) {
    SwissTable replacement(hasher_, equal_, allocator_);
    replacement.allocate_capacity(requested_capacity);

    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        replacement.insert_existing(std::move(*slot_ptr(i)));
        std::destroy_at(slot_ptr(i));
      }
    }

    if (slots_ != nullptr) {
      alloc_traits::deallocate(allocator_, slots_, capacity_);
    }

    control_ = std::move(replacement.control_);
    slots_ = std::exchange(replacement.slots_, nullptr);
    size_ = replacement.size_;
    tombstones_ = 0;
    capacity_ = replacement.capacity_;
    replacement.size_ = 0;
    replacement.capacity_ = 0;
  }

  std::vector<std::uint8_t> control_;
  value_type* slots_{nullptr};
  size_type size_{0};
  size_type tombstones_{0};
  size_type capacity_{0};
  [[no_unique_address]] Hash hasher_{};
  [[no_unique_address]] KeyEqual equal_{};
  [[no_unique_address]] Allocator allocator_{};
};

template <class Key, class T, class Hash, class KeyEqual, std::size_t GroupWidth, class Allocator>
void swap(SwissTable<Key, T, Hash, KeyEqual, GroupWidth, Allocator>& lhs,
          SwissTable<Key, T, Hash, KeyEqual, GroupWidth, Allocator>& rhs) noexcept {
  lhs.swap(rhs);
}

}  // namespace goblin::core
