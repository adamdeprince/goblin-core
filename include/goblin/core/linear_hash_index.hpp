#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "goblin/core/linear_hash_arena.hpp"
#include "goblin/core/simd_ops.hpp"

namespace goblin::core {

// Linear hashing grows and shrinks one logical bucket at a time. Every logical
// bucket is a short chain of fixed-size Swiss groups. Bucket zero is inline;
// subsequent primaries occupy immutable 4/8/16/32/64/128-bucket extents followed
// by 64-bucket terminal extents. Overflow groups remain stable 32-bit handles
// into the same shared, prefaulted buddy arena. A compact directory maps terminal
// extent numbers to bases. No live bucket or directory entry moves during serving.
//
// Field keys are hashed with XXH3; each live slot stores a 32-bit folded hash so
// incremental split/merge never re-reads field bytes to rehash. Serving growth
// and shrink never rebuild the whole table.
template <class Storage, class Meta>
class LinearHashIndex {
 public:
  using size_type = std::size_t;
  using id_type = typename Meta::id_type;
  // Folded XXH3: low 32 bits after xor-fold. Stored beside every live slot for
  // full-hash SIMD probes and migration without rehashing field bytes.
  using hash_type = std::uint32_t;
  using Arena = LinearHashArena<Meta>;
  using Bucket = typename Arena::Bucket;
  using HotBucket = typename Arena::HotBucket;

  static constexpr size_type kGroupWidth = Arena::kGroupWidth;
  // Slightly denser than 12/16: fewer overflow chains and directory primaries
  // at large cardinality, measured against split p99 under mixed load.
  static constexpr size_type kTargetEntriesPerBucket = 13;
  static constexpr size_type kMinimumEntriesPerBucket = 6;
  static constexpr size_type kTieredPrimaryExtentCount =
      Arena::kTieredPrimaryExtentCount;
  static constexpr auto kTieredPrimaryExtentBuckets =
      Arena::kTieredPrimaryExtentBuckets;
  static constexpr size_type kTieredPrimaryBuckets =
      Arena::kTieredPrimaryBuckets;
  static constexpr size_type kTerminalPrimaryExtentBuckets =
      Arena::kTerminalPrimaryExtentBuckets;
  static constexpr size_type kTerminalPrimaryExtentShift =
      Arena::kTerminalPrimaryExtentShift;
  static constexpr size_type kTerminalPrimaryExtentMask =
      Arena::kTerminalPrimaryExtentMask;
  static constexpr size_type kInlineTerminalExtentHandles =
      Arena::kInlineTerminalExtentHandles;
  static constexpr size_type kPrimaryDirectoryBlockEntries =
      Arena::kPrimaryDirectoryBlockEntries;
  static constexpr size_type npos =
      std::numeric_limits<size_type>::max();

  LinearHashIndex() : arena_(std::make_shared<Arena>()) {
    Arena::reset_bucket(inline_primary_, 0);
    tiered_primary_extent_handles_.fill(kNoBucket);
    inline_terminal_extent_handles_.fill(kNoBucket);
    refresh_address_masks();
  }

  explicit LinearHashIndex(const Storage* members,
                           std::shared_ptr<Arena> arena = {})
      : members_(members),
        arena_(arena ? std::move(arena) : std::make_shared<Arena>()) {
    Arena::reset_bucket(inline_primary_, 0);
    tiered_primary_extent_handles_.fill(kNoBucket);
    inline_terminal_extent_handles_.fill(kNoBucket);
    refresh_address_masks();
  }

  ~LinearHashIndex() { clear(); }

  LinearHashIndex(const LinearHashIndex&) = delete;
  LinearHashIndex& operator=(const LinearHashIndex&) = delete;

  void set_members(const Storage* members) noexcept { members_ = members; }
  [[nodiscard]] const std::shared_ptr<Arena>& arena() const noexcept {
    return arena_;
  }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] size_type size() const noexcept { return size_; }
  [[nodiscard]] size_type capacity() const noexcept {
    return (primary_count_ + overflow_count_) * kGroupWidth;
  }
  [[nodiscard]] size_type tombstone_count() const noexcept { return 0; }
  [[nodiscard]] size_type member_slot_capacity() const noexcept { return 0; }
  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return arena_cells_ * arena_->cell_bytes() +
           directory_block_count_ * Arena::directory_block_bytes() +
           (directory_table_ == kNoBucket
                ? 0
                : arena_->primary_directory_table_bytes());
  }
  [[nodiscard]] size_type arena_cell_count() const noexcept {
    return arena_cells_;
  }

  // Empty overflow groups are returned immediately. Primary contraction is a
  // reverse linear-hash merge, advanced one physical group per mutation.
  bool cleanup_after_removal_if_needed(size_type) noexcept { return false; }
  bool rehash_same_capacity() noexcept { return false; }

  // Reserve is a bulk/restore operation, not a serving mutation. It may append
  // many primary handles, but it never relocates an existing bucket.
  void reserve(size_type expected_size) {
    reserve_for_density(expected_size,
                        static_cast<double>(kTargetEntriesPerBucket) /
                            static_cast<double>(kGroupWidth));
  }
  // Serving bulk HSET into an empty index can pre-grow primaries so the first
  // batch does not interleave every insert with a split step. Non-empty indexes
  // keep incremental split as the only growth path (addressing invariants).
  void reserve_additional(size_type additional) {
    if (additional == 0 || size_ != 0 || split_active_ || merge_active_) {
      return;
    }
    reserve_for_density(additional,
                        static_cast<double>(kTargetEntriesPerBucket) /
                            static_cast<double>(kGroupWidth));
  }

  void reserve_for_density(size_type expected_size, double density) {
    if (size_ != 0 || split_active_ || merge_active_ || expected_size == 0) {
      return;
    }
    if (!(density > 0.0) || density > 1.0) {
      density = static_cast<double>(kTargetEntriesPerBucket) /
                static_cast<double>(kGroupWidth);
    }
    density = std::min(
        density, static_cast<double>(kTargetEntriesPerBucket) /
                     static_cast<double>(kGroupWidth));
    const auto buckets = std::max<size_type>(
        1, static_cast<size_type>(
               static_cast<double>(expected_size) /
                   (density * static_cast<double>(kGroupWidth)) +
               0.999999999));
    if (buckets >= kNoBucket) {
      throw std::length_error("linear hash primary bucket space exhausted");
    }
    ensure_primary();
    while (primary_count_ < buckets) {
      append_primary();
    }
    level_size_ = std::bit_floor(buckets);
    split_ = buckets - level_size_;
    refresh_address_masks();
  }

  void insert_packed(std::string_view member, Meta meta) {
    const auto hash = hash_member(member);
    ensure_primary();
    (void)insert_known_absent(hash, meta);
    ++size_;
    maintenance_step();
  }

  [[nodiscard]] Meta* find(std::string_view member) {
    const auto location = find_location(member);
    return location == npos ? nullptr
                            : &bucket(location).slots[slot_of(location)];
  }
  [[nodiscard]] const Meta* find(std::string_view member) const {
    const auto location = find_location(member);
    return location == npos ? nullptr
                            : &bucket(location).slots[slot_of(location)];
  }

  [[nodiscard]] std::optional<size_type> find_slot(
      std::string_view member) const {
    const auto location = find_location(member);
    if (location == npos) {
      return std::nullopt;
    }
    return location;
  }
  [[nodiscard]] id_type member_id_at(size_type location) const noexcept {
    return bucket(location).slots[slot_of(location)].get();
  }

  bool erase_at_index(size_type location) {
    auto& physical = bucket(location);
    const auto slot = slot_of(location);
    if (slot >= kGroupWidth || !is_occupied(physical, slot)) {
      return false;
    }
    physical.occupied &= static_cast<std::uint16_t>(~(std::uint16_t{1} << slot));
    --physical.size;
    --size_;
    if (location_kind(location) == LocationKind::overflow &&
        physical.size == 0 && handle_of(location) != split_cursor_ &&
        handle_of(location) != merge_cursor_) {
      unlink_empty_overflow(handle_of(location));
    }
    maintenance_step();
    return true;
  }

  [[nodiscard]] bool move_member_id(id_type old_member_id,
                                    id_type new_member_id) {
    const auto location = find_location(member_view(old_member_id));
    if (location == npos) {
      return false;
    }
    return move_member_id_at_slot(location, old_member_id, new_member_id);
  }

  [[nodiscard]] bool move_member_id_at_slot(size_type location,
                                            id_type old_member_id,
                                            id_type new_member_id) {
    auto& meta = bucket(location).slots[slot_of(location)];
    if (meta.get() != old_member_id) {
      return false;
    }
    meta.set(new_member_id);
    return true;
  }

  std::pair<Meta*, bool> insert(std::string_view member, Meta meta) {
    const auto hash = hash_member(member);
    if (const auto existing = find_location_with_hash(member, hash);
        existing != npos) {
      return {&bucket(existing).slots[slot_of(existing)], false};
    }
    ensure_primary();
    const auto location = insert_known_absent(hash, meta);
    ++size_;
    maintenance_step();
    return {&bucket(location).slots[slot_of(location)], true};
  }

  // `maintain` runs one physical split/merge step after a structural insert.
  // Deferring it is only safe when the caller later repays one step per
  // structural mutation; ordinary multi-field HSET keeps it enabled.
  template <class OnExisting, class MakeMeta>
  bool find_or_emplace(std::string_view member, OnExisting&& on_existing,
                       MakeMeta&& make_meta, bool maintain = true) {
    const auto hash = hash_member(member);
    size_type empty_location = npos;
    if (const auto existing =
            find_location_with_hash(member, hash, &empty_location);
        existing != npos) {
      // Value-overwrite hits are not structural: do not advance split/merge on
      // the HGET/HSET-update path (keeps point latency flat under growth).
      std::forward<OnExisting>(on_existing)(
          bucket(existing).slots[slot_of(existing)]);
      return false;
    }
    ensure_primary();
    size_type location = empty_location;
    if (location == npos) {
      location = prepare_insert_location(
          static_cast<std::uint32_t>(address(hash)));
    }
    const auto meta = std::forward<MakeMeta>(make_meta)();
    commit_insert(location, hash, meta);
    ++size_;
    if (maintain) {
      maintenance_step();
    }
    return true;
  }

  // Run up to `steps` deferred physical split/merge groups.
  void run_maintenance_steps(size_type steps) noexcept {
    for (size_type i = 0; i < steps; ++i) {
      if (!split_active_ && !merge_active_) {
        start_split_if_needed();
        if (!split_active_) {
          start_merge_if_needed();
        }
      }
      if (!split_active_ && !merge_active_) {
        break;
      }
      advance_active_maintenance();
    }
  }

  void insert_absent(std::string_view member, Meta meta,
                     bool maintain = true) {
    ensure_primary();
    (void)insert_known_absent(hash_member(member), meta);
    ++size_;
    if (maintain) {
      maintenance_step();
    }
  }

  bool erase(std::string_view member) {
    const auto location = find_location(member);
    return location != npos && erase_at_index(location);
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (std::uint32_t primary = 0; primary < primary_count_; ++primary) {
      for_each_chain(primary, fn);
    }
  }

  void clear() noexcept {
    if (!arena_) {
      return;
    }
    for (std::uint32_t primary = 0; primary < primary_count_; ++primary) {
      auto next = primary_bucket(primary).next;
      while (next != kNoBucket) {
        const auto following = arena_->bucket(next).next;
        arena_->release(next);
        next = following;
      }
    }
    for (size_type extent = 0; extent < kTieredPrimaryExtentCount; ++extent) {
      const auto handle = tiered_primary_extent_handles_[extent];
      if (handle != kNoBucket) {
        arena_->release_primary_extent(handle,
                                       kTieredPrimaryExtentBuckets[extent]);
      }
    }
    for (size_type extent = 0; extent < terminal_extent_count_; ++extent) {
      arena_->release_primary_extent(terminal_extent_base(extent),
                                     kTerminalPrimaryExtentBuckets);
    }
    for (size_type block = 0; block < directory_block_count_; ++block) {
      assert(directory_blocks_[block] != kNoBucket);
      arena_->release_primary_directory_block(directory_blocks_[block]);
    }
    if (directory_table_ != kNoBucket) {
      arena_->release_primary_directory_table(directory_table_);
    }
    Arena::reset_bucket(inline_primary_, 0);
    tiered_primary_extent_handles_.fill(kNoBucket);
    inline_terminal_extent_handles_.fill(kNoBucket);
    directory_table_ = kNoBucket;
    directory_blocks_ = nullptr;
    directory_block_count_ = 0;
    primary_count_ = 0;
    terminal_extent_count_ = 0;
    overflow_count_ = 0;
    arena_cells_ = 0;
    size_ = 0;
    level_size_ = 1;
    split_ = 0;
    split_active_ = false;
    split_cursor_ = kPrimaryCursor;
    merge_active_ = false;
    merge_cursor_ = kPrimaryCursor;
    merge_source_ = kNoBucket;
    merge_destination_ = kNoBucket;
    refresh_address_masks();
  }

 private:
  enum class LocationKind : size_type {
    inline_primary = 0,
    arena_primary = 1,
    overflow = 2,
  };

  static constexpr std::uint16_t kAllSlots = 0xffffU;
  static constexpr std::uint32_t kNoBucket = Arena::kNoCell;
  static constexpr std::uint32_t kPrimaryCursor = kNoBucket;
  static constexpr size_type kKindShift =
      std::numeric_limits<size_type>::digits - 2;
  static constexpr size_type kKindMask = size_type{3} << kKindShift;
  static constexpr size_type kHandleMask =
      static_cast<size_type>(std::numeric_limits<std::uint32_t>::max());
  static_assert(std::numeric_limits<size_type>::digits >= 64);

  [[nodiscard]] static bool is_occupied(const Bucket& bucket,
                                        size_type slot) noexcept {
    return (bucket.occupied & (std::uint16_t{1} << slot)) != 0;
  }
  [[nodiscard]] static size_type first_set_bit(std::uint64_t mask) noexcept {
    return static_cast<size_type>(std::countr_zero(mask));
  }
  [[nodiscard]] static std::uint64_t empty_mask(
      const Bucket& bucket) noexcept {
    return static_cast<std::uint16_t>(~bucket.occupied) & kAllSlots;
  }

  [[nodiscard]] std::string_view member_view(id_type id) const noexcept {
    return members_->view(id);
  }

  [[nodiscard]] hash_type hash_member(std::string_view member) const noexcept {
    const auto wide = XXH3_64bits(member.data(), member.size());
    return static_cast<hash_type>(wide) ^
           static_cast<hash_type>(wide >> 32);
  }

  void refresh_address_masks() noexcept {
    level_mask_ = level_size_ - 1;
    next_level_mask_ = (level_size_ << 1) - 1;
    refresh_bucket_count();
  }
  void refresh_bucket_count() noexcept {
    bucket_count_ = level_size_ + split_ + (split_active_ ? 1 : 0);
  }

  [[nodiscard]] size_type address(hash_type hash) const noexcept {
    const auto low = static_cast<size_type>(hash & level_mask_);
    const auto high = static_cast<size_type>(hash & next_level_mask_);
    return high < bucket_count_ ? high : low;
  }

  [[nodiscard]] size_type split_source() const noexcept { return split_; }
  [[nodiscard]] size_type split_destination() const noexcept {
    return level_size_ + split_;
  }

  void ensure_primary() {
    if (primary_count_ == 0) {
      Arena::reset_bucket(inline_primary_, 0);
      primary_count_ = 1;
      level_size_ = 1;
      split_ = 0;
      refresh_address_masks();
    }
  }

  [[nodiscard]] static size_type make_location(LocationKind kind,
                                               std::uint32_t handle,
                                               size_type slot) noexcept {
    return (static_cast<size_type>(kind) << kKindShift) |
           (static_cast<size_type>(handle) << 4) | slot;
  }
  [[nodiscard]] static size_type make_inline_location(size_type slot) noexcept {
    return make_location(LocationKind::inline_primary, 0, slot);
  }
  [[nodiscard]] static size_type make_primary_location(std::uint32_t handle,
                                                       size_type slot) noexcept {
    return make_location(LocationKind::arena_primary, handle, slot);
  }
  [[nodiscard]] static size_type make_overflow_location(std::uint32_t handle,
                                                        size_type slot) noexcept {
    return make_location(LocationKind::overflow, handle, slot);
  }
  [[nodiscard]] static LocationKind location_kind(size_type location) noexcept {
    return static_cast<LocationKind>((location & kKindMask) >> kKindShift);
  }
  [[nodiscard]] static std::uint32_t handle_of(size_type location) noexcept {
    return static_cast<std::uint32_t>((location >> 4) & kHandleMask);
  }
  [[nodiscard]] static size_type slot_of(size_type location) noexcept {
    return location & (kGroupWidth - 1);
  }

  [[nodiscard]] Bucket& bucket(size_type location) noexcept {
    return location_kind(location) == LocationKind::inline_primary
               ? inline_primary_
               : arena_->bucket(handle_of(location));
  }
  [[nodiscard]] const Bucket& bucket(size_type location) const noexcept {
    return location_kind(location) == LocationKind::inline_primary
               ? inline_primary_
               : arena_->bucket(handle_of(location));
  }
  [[nodiscard]] std::uint32_t* hash_group(size_type location) noexcept {
    return location_kind(location) == LocationKind::inline_primary
               ? inline_hashes_.data()
               : arena_->hot_bucket(handle_of(location)).hashes.data();
  }
  [[nodiscard]] const std::uint32_t* hash_group(
      size_type location) const noexcept {
    return location_kind(location) == LocationKind::inline_primary
               ? inline_hashes_.data()
               : arena_->hot_bucket(handle_of(location)).hashes.data();
  }

  [[nodiscard]] static size_type tiered_extent_index(
      size_type offset) noexcept {
    assert(offset < kTieredPrimaryBuckets);
    return std::bit_width(offset + 4) - 3;
  }

  [[nodiscard]] static constexpr size_type tiered_extent_start(
      size_type extent) noexcept {
    return (size_type{4} << extent) - 4;
  }

  [[nodiscard]] std::uint32_t terminal_extent_base(
      size_type extent) const noexcept {
    assert(extent < terminal_extent_count_);
    if (extent < kInlineTerminalExtentHandles) {
      const auto handle = inline_terminal_extent_handles_[extent];
      assert(handle != kNoBucket);
      return handle;
    }
    const auto offset = extent - kInlineTerminalExtentHandles;
    // 512 = 2^9: keep the shift form so directory addressing stays shift/mask
    // rather than a general divide on every large-table probe.
    static_assert(kPrimaryDirectoryBlockEntries == 512);
    const auto block_slot = offset >> 9;
    const auto entry_slot = offset & 511;
    assert(directory_blocks_ != nullptr &&
           block_slot < directory_block_count_);
    const auto block = directory_blocks_[block_slot];
    assert(block != kNoBucket);
    const auto handle = arena_->primary_directory_entry(block, entry_slot);
    assert(handle != kNoBucket);
    return handle;
  }

  [[nodiscard]] std::uint32_t primary_handle(std::uint32_t primary) const
      noexcept {
    // append_primary resolves the next logical primary after its containing
    // extent is installed but before primary_count_ is advanced.
    assert(primary != 0 && primary <= primary_count_);
    const auto offset = static_cast<size_type>(primary - 1);
    if (offset < kTieredPrimaryBuckets) {
      const auto extent = tiered_extent_index(offset);
      const auto handle = tiered_primary_extent_handles_[extent];
      assert(handle != kNoBucket);
      return handle + static_cast<std::uint32_t>(
                          offset - tiered_extent_start(extent));
    }
    const auto terminal_offset = offset - kTieredPrimaryBuckets;
    return terminal_extent_base(terminal_offset >>
                                kTerminalPrimaryExtentShift) +
           static_cast<std::uint32_t>(terminal_offset &
                                      kTerminalPrimaryExtentMask);
  }

  [[nodiscard]] Bucket& primary_bucket(std::uint32_t primary) noexcept {
    return primary == 0 ? inline_primary_
                        : arena_->bucket(primary_handle(primary));
  }
  [[nodiscard]] const Bucket& primary_bucket(
      std::uint32_t primary) const noexcept {
    return primary == 0 ? inline_primary_
                        : arena_->bucket(primary_handle(primary));
  }
  [[nodiscard]] std::uint32_t* primary_hash_group(
      std::uint32_t primary) noexcept {
    return primary == 0
               ? inline_hashes_.data()
               : arena_->hot_bucket(primary_handle(primary)).hashes.data();
  }
  [[nodiscard]] const std::uint32_t* primary_hash_group(
      std::uint32_t primary) const noexcept {
    return primary == 0
               ? inline_hashes_.data()
               : arena_->hot_bucket(primary_handle(primary)).hashes.data();
  }

  void set_terminal_extent_handle(size_type extent, std::uint32_t handle) {
    assert(extent == terminal_extent_count_);
    if (extent < kInlineTerminalExtentHandles) {
      inline_terminal_extent_handles_[extent] = handle;
      return;
    }
    static_assert(kPrimaryDirectoryBlockEntries == 512);
    const auto offset = extent - kInlineTerminalExtentHandles;
    const auto block_slot = offset >> 9;
    const auto entry_slot = offset & 511;
    assert(block_slot < arena_->primary_directory_slots_per_index());
    if (directory_blocks_ == nullptr) {
      assert(block_slot == 0 && directory_table_ == kNoBucket);
      directory_table_ = arena_->allocate_primary_directory_table();
      directory_blocks_ = arena_->primary_directory_table(directory_table_);
    }
    auto& block = directory_blocks_[block_slot];
    if (entry_slot == 0) {
      assert(block_slot == directory_block_count_);
      block = arena_->allocate_primary_directory_block();
      ++directory_block_count_;
    } else {
      assert(block_slot < directory_block_count_ && block != kNoBucket);
    }
    arena_->primary_directory_entry(block, entry_slot) = handle;
  }

  void erase_terminal_extent_handle(size_type extent) noexcept {
    assert(extent + 1 == terminal_extent_count_);
    if (extent < kInlineTerminalExtentHandles) {
      inline_terminal_extent_handles_[extent] = kNoBucket;
      return;
    }
    static_assert(kPrimaryDirectoryBlockEntries == 512);
    const auto offset = extent - kInlineTerminalExtentHandles;
    const auto block_slot = offset >> 9;
    const auto entry_slot = offset & 511;
    assert(block_slot < directory_block_count_);
    const auto block = directory_blocks_[block_slot];
    assert(block != kNoBucket);
    arena_->primary_directory_entry(block, entry_slot) = kNoBucket;
    if (entry_slot == 0) {
      assert(block_slot + 1 == directory_block_count_);
      arena_->release_primary_directory_block(block);
      directory_blocks_[block_slot] = kNoBucket;
      --directory_block_count_;
      if (directory_block_count_ == 0) {
        arena_->release_primary_directory_table(directory_table_);
        directory_table_ = kNoBucket;
        directory_blocks_ = nullptr;
      }
    }
  }

  [[nodiscard]] static bool primary_extent_for_offset(
      size_type offset, size_type& extent, size_type& extent_start,
      size_type& extent_buckets, bool& terminal) noexcept {
    if (offset < kTieredPrimaryBuckets) {
      extent = tiered_extent_index(offset);
      extent_start = tiered_extent_start(extent);
      extent_buckets = kTieredPrimaryExtentBuckets[extent];
      terminal = false;
      return offset == extent_start;
    }
    const auto terminal_offset = offset - kTieredPrimaryBuckets;
    extent = terminal_offset >> kTerminalPrimaryExtentShift;
    extent_start = kTieredPrimaryBuckets +
                   extent * kTerminalPrimaryExtentBuckets;
    extent_buckets = kTerminalPrimaryExtentBuckets;
    terminal = true;
    return (terminal_offset & kTerminalPrimaryExtentMask) == 0;
  }

  void append_primary() {
    if (primary_count_ >= kNoBucket) {
      throw std::length_error("linear hash primary bucket space exhausted");
    }
    const auto primary = static_cast<std::uint32_t>(primary_count_);
    const auto offset = static_cast<size_type>(primary - 1);
    size_type extent = 0;
    size_type extent_start = 0;
    size_type extent_buckets = 0;
    bool terminal = false;
    const bool needs_primary_extent = primary_extent_for_offset(
        offset, extent, extent_start, extent_buckets, terminal);
    const auto needs_directory_block =
        needs_primary_extent && terminal &&
        extent >= kInlineTerminalExtentHandles &&
        ((extent - kInlineTerminalExtentHandles) %
             kPrimaryDirectoryBlockEntries ==
         0);
    if (needs_primary_extent &&
        !arena_->can_allocate_primary_extent(extent_buckets)) {
      throw std::length_error(
          "real-time hash index arena has no contiguous primary extent; increase "
          "--rt-hash-index-bytes");
    }
    if (needs_directory_block && arena_->available_directory_blocks() == 0) {
      throw std::length_error(
          "real-time hash primary-directory pool exhausted; increase "
          "--rt-hash-index-bytes");
    }
    if (needs_directory_block && directory_blocks_ == nullptr &&
        arena_->available_directory_tables() == 0) {
      throw std::length_error(
          "real-time hash primary-directory table pool exhausted; increase "
          "--rt-hash-index-bytes");
    }
    std::uint32_t handle = kNoBucket;
    if (needs_primary_extent) {
      handle = arena_->allocate_uninitialized_primary_extent(extent_buckets);
      arena_cells_ += extent_buckets;
      try {
        if (terminal) {
          set_terminal_extent_handle(extent, handle);
        } else {
          assert(tiered_primary_extent_handles_[extent] == kNoBucket);
          tiered_primary_extent_handles_[extent] = handle;
        }
      } catch (...) {
        arena_->release_primary_extent(handle, extent_buckets);
        arena_cells_ -= extent_buckets;
        throw;
      }
      if (terminal) {
        ++terminal_extent_count_;
      }
    } else {
      handle = primary_handle(primary);
    }
    // Reserving an extent is allocation-only. Initialize exactly the bucket
    // becoming logical so an extent boundary cannot create a 64-bucket write
    // burst on one serving mutation.
    arena_->initialize_primary_bucket(handle, primary);
    ++primary_count_;
  }

  void pop_empty_last_primary() noexcept {
    assert(primary_count_ > 1);
    const auto primary = static_cast<std::uint32_t>(primary_count_ - 1);
    const auto handle = primary_handle(primary);
    assert(arena_->bucket(handle).size == 0 &&
           arena_->bucket(handle).next == kNoBucket);
    --primary_count_;
    const auto offset = static_cast<size_type>(primary - 1);
    size_type extent = 0;
    size_type extent_start = 0;
    size_type extent_buckets = 0;
    bool terminal = false;
    if (primary_extent_for_offset(offset, extent, extent_start, extent_buckets,
                                  terminal)) {
      if (terminal) {
        erase_terminal_extent_handle(extent);
        --terminal_extent_count_;
      } else {
        assert(tiered_primary_extent_handles_[extent] == handle);
        tiered_primary_extent_handles_[extent] = kNoBucket;
      }
      arena_->release_primary_extent(handle, extent_buckets);
      arena_cells_ -= extent_buckets;
    }
  }

  // Probe one physical bucket. Optionally records the first empty slot for a
  // fused find-or-insert walk. Prefetches the overflow successor when present.
  // Empty-slot scanning is deferred until after a hash miss so the common
  // HGET/HSET-hit path scans one 64-byte cache line of full 32-bit hashes.
  [[nodiscard]] size_type find_in_bucket(
      const std::uint32_t* hashes, const Bucket& physical, LocationKind kind,
      std::uint32_t handle, std::string_view member, hash_type hash,
      size_type* empty_out) const {
    if (physical.next != kNoBucket) {
      __builtin_prefetch(&arena_->hot_bucket(physical.next), 0, 1);
      __builtin_prefetch(&arena_->bucket(physical.next), 0, 1);
    }
    auto matches = simd::match_hash_group_16(hashes, hash) &
                   physical.occupied;
    while (matches != 0) {
      const auto slot = first_set_bit(matches);
      matches &= matches - 1;
      const auto field = member_view(physical.slots[slot].get());
#if defined(__GNUC__) || defined(__clang__)
      if (!field.empty()) {
        __builtin_prefetch(field.data(), 0, 3);
      }
#endif
      if (simd::bytes_equal(field, member)) {
        return make_location(kind, handle, slot);
      }
    }
    if (empty_out != nullptr && *empty_out == npos) {
      const auto empty = empty_mask(physical);
      if (empty != 0) {
        *empty_out = make_location(kind, handle, first_set_bit(empty));
      }
    }
    return npos;
  }

  [[nodiscard]] size_type find_in_chain(std::uint32_t primary,
                                        std::string_view member, hash_type hash,
                                        size_type* empty_out) const {
    const auto handle =
        primary == 0 ? std::uint32_t{0} : primary_handle(primary);
    const auto kind = primary == 0 ? LocationKind::inline_primary
                                   : LocationKind::arena_primary;
    const auto& first =
        primary == 0 ? inline_primary_ : arena_->bucket(handle);
    const auto* first_hashes =
        primary == 0 ? inline_hashes_.data()
                     : arena_->hot_bucket(handle).hashes.data();
    if (const auto found = find_in_bucket(first_hashes, first, kind, handle,
                                          member, hash, empty_out);
        found != npos) {
      return found;
    }
    auto next = first.next;
    while (next != kNoBucket) {
      const auto& overflow = arena_->bucket(next);
      const auto* overflow_hashes = arena_->hot_bucket(next).hashes.data();
      if (const auto found =
              find_in_bucket(overflow_hashes, overflow, LocationKind::overflow,
                             next, member, hash, empty_out);
          found != npos) {
        return found;
      }
      next = overflow.next;
    }
    return npos;
  }

  // When empty_out is non-null, records the first empty slot on the *home*
  // chain only (insert destination). Alternate split/merge chains are probed
  // for existence without empty tracking.
  [[nodiscard]] size_type find_location_with_hash(
      std::string_view member, hash_type hash,
      size_type* empty_out = nullptr) const {
    if (primary_count_ == 0) {
      return npos;
    }
    const auto target = static_cast<std::uint32_t>(address(hash));
    if (const auto found = find_in_chain(target, member, hash, empty_out);
        found != npos) {
      return found;
    }
    if (split_active_ && target == split_destination()) {
      if (const auto found = find_in_chain(
              static_cast<std::uint32_t>(split_source()), member, hash, nullptr);
          found != npos) {
        return found;
      }
    }
    if (merge_active_ && target == merge_destination_) {
      if (const auto found =
              find_in_chain(merge_source_, member, hash, nullptr);
          found != npos) {
        return found;
      }
    }
    return npos;
  }

  [[nodiscard]] size_type find_location(std::string_view member) const {
    if (primary_count_ == 0) {
      return npos;
    }
    return find_location_with_hash(member, hash_member(member));
  }

  [[nodiscard]] std::uint32_t allocate_overflow(std::uint32_t owner) {
    const auto handle = arena_->allocate_bucket(owner);
    ++overflow_count_;
    ++arena_cells_;
    return handle;
  }

  [[nodiscard]] size_type prepare_insert_location(std::uint32_t primary) {
    auto* physical = &primary_bucket(primary);
    LocationKind kind = primary == 0 ? LocationKind::inline_primary
                                     : LocationKind::arena_primary;
    std::uint32_t handle = primary == 0 ? 0 : primary_handle(primary);
    for (;;) {
      if (physical->next != kNoBucket) {
        __builtin_prefetch(&arena_->hot_bucket(physical->next), 0, 1);
        __builtin_prefetch(&arena_->bucket(physical->next), 0, 1);
      }
      const auto empty = empty_mask(*physical);
      if (empty != 0) {
        return make_location(kind, handle, first_set_bit(empty));
      }
      if (physical->next == kNoBucket) {
        const auto next = allocate_overflow(primary);
        arena_->bucket(next).previous =
            kind == LocationKind::overflow ? handle : kNoBucket;
        physical->next = next;
      }
      handle = physical->next;
      physical = &arena_->bucket(handle);
      kind = LocationKind::overflow;
    }
  }

  void commit_insert(size_type location, hash_type hash, Meta meta) {
    auto& physical = bucket(location);
    auto* hashes = hash_group(location);
    const auto slot = slot_of(location);
    physical.slots[slot] = meta;
    hashes[slot] = hash;
    physical.occupied |= std::uint16_t{1} << slot;
    ++physical.size;
  }

  [[nodiscard]] size_type insert_into_chain(std::uint32_t primary,
                                            hash_type hash, Meta meta) {
    const auto location = prepare_insert_location(primary);
    commit_insert(location, hash, meta);
    return location;
  }

  [[nodiscard]] size_type insert_known_absent(hash_type hash, Meta meta) {
    return insert_into_chain(static_cast<std::uint32_t>(address(hash)), hash,
                             meta);
  }

  void start_split_if_needed() noexcept {
    if (split_active_ || merge_active_ || primary_count_ == 0 ||
        size_ <= primary_count_ * kTargetEntriesPerBucket) {
      return;
    }
    try {
      append_primary();
    } catch (const std::length_error&) {
      // The completed mutation remains valid. A later delete can return arena
      // cells; an insert that truly needs another physical group will report
      // the configured arena exhaustion before it commits.
      return;
    }
    split_active_ = true;
    split_cursor_ = kPrimaryCursor;
    refresh_bucket_count();
  }

  void start_merge_if_needed() noexcept {
    if (split_active_ || merge_active_ || primary_count_ <= 1 ||
        size_ >= (primary_count_ - 1) * kMinimumEntriesPerBucket) {
      return;
    }
    merge_source_ = static_cast<std::uint32_t>(primary_count_ - 1);
    if (split_ != 0) {
      --split_;
    } else {
      level_size_ >>= 1;
      split_ = level_size_ - 1;
    }
    merge_destination_ = static_cast<std::uint32_t>(split_);
    merge_active_ = true;
    merge_cursor_ = kPrimaryCursor;
    refresh_address_masks();
  }

  void advance_active_maintenance() noexcept {
    // Predicted-not-taken on steady-state hits with no in-flight split/merge.
    if (split_active_) {
      split_step();
    } else if (merge_active_) {
      merge_step();
    }
  }

  void maintenance_step() noexcept {
    if (!split_active_ && !merge_active_) {
      // Occupancy checks only when no maintenance is already in flight.
      start_split_if_needed();
      if (!split_active_) {
        start_merge_if_needed();
      }
    }
    advance_active_maintenance();
  }

  void split_step() noexcept {
    const auto source = static_cast<std::uint32_t>(split_source());
    const bool primary_cursor = split_cursor_ == kPrimaryCursor;
    auto* physical = primary_cursor ? &primary_bucket(source)
                                    : &arena_->bucket(split_cursor_);
    auto* hashes = primary_cursor
                       ? primary_hash_group(source)
                       : arena_->hot_bucket(split_cursor_).hashes.data();
    const auto next = physical->next;
    if (next != kNoBucket) {
      __builtin_prefetch(&arena_->hot_bucket(next), 1, 1);
      __builtin_prefetch(&arena_->bucket(next), 1, 1);
    }

    for (size_type slot = 0; slot < kGroupWidth; ++slot) {
      if (!is_occupied(*physical, slot)) {
        continue;
      }
      const auto hash = hashes[slot];
      if (address(hash) != split_destination()) {
        continue;
      }
      try {
        (void)insert_into_chain(
            static_cast<std::uint32_t>(split_destination()), hash,
            physical->slots[slot]);
      } catch (const std::length_error&) {
        return;
      }
      physical->occupied &=
          static_cast<std::uint16_t>(~(std::uint16_t{1} << slot));
      --physical->size;
    }

    if (!primary_cursor && physical->size == 0) {
      unlink_empty_overflow(split_cursor_);
    }
    split_cursor_ = next;
    if (split_cursor_ != kNoBucket) {
      return;
    }

    split_active_ = false;
    split_cursor_ = kPrimaryCursor;
    ++split_;
    if (split_ == level_size_) {
      level_size_ <<= 1;
      split_ = 0;
    }
    refresh_address_masks();
  }

  void merge_step() noexcept {
    const bool primary_cursor = merge_cursor_ == kPrimaryCursor;
    auto* physical = primary_cursor ? &primary_bucket(merge_source_)
                                    : &arena_->bucket(merge_cursor_);
    auto* hashes = primary_cursor
                       ? primary_hash_group(merge_source_)
                       : arena_->hot_bucket(merge_cursor_).hashes.data();
    const auto next = physical->next;
    if (next != kNoBucket) {
      __builtin_prefetch(&arena_->hot_bucket(next), 1, 1);
      __builtin_prefetch(&arena_->bucket(next), 1, 1);
    }

    for (size_type slot = 0; slot < kGroupWidth; ++slot) {
      if (!is_occupied(*physical, slot)) {
        continue;
      }
      const auto hash = hashes[slot];
      try {
        (void)insert_into_chain(merge_destination_, hash,
                                physical->slots[slot]);
      } catch (const std::length_error&) {
        return;
      }
      physical->occupied &=
          static_cast<std::uint16_t>(~(std::uint16_t{1} << slot));
      --physical->size;
    }

    if (!primary_cursor && physical->size == 0) {
      unlink_empty_overflow(merge_cursor_);
    }
    merge_cursor_ = next;
    if (merge_cursor_ != kNoBucket) {
      return;
    }
    assert(primary_bucket(merge_source_).size == 0);
    assert(primary_bucket(merge_source_).next == kNoBucket);
    pop_empty_last_primary();
    merge_active_ = false;
    merge_cursor_ = kPrimaryCursor;
    merge_source_ = kNoBucket;
    merge_destination_ = kNoBucket;
  }

  void unlink_empty_overflow(std::uint32_t victim) noexcept {
    if (victim == kNoBucket) {
      return;
    }
    auto& empty = arena_->bucket(victim);
    if (empty.size != 0 || empty.owner >= primary_count_) {
      return;
    }
    const auto next = empty.next;
    if (empty.previous == kNoBucket) {
      primary_bucket(empty.owner).next = next;
    } else {
      arena_->bucket(empty.previous).next = next;
    }
    if (next != kNoBucket) {
      arena_->bucket(next).previous = empty.previous;
    }
    arena_->release(victim);
    --overflow_count_;
    --arena_cells_;
  }

  template <class Fn>
  void for_each_chain(std::uint32_t primary, Fn& fn) const {
    const auto visit = [&](const Bucket& physical) {
      for (size_type slot = 0; slot < kGroupWidth; ++slot) {
        if (is_occupied(physical, slot)) {
          fn(member_view(physical.slots[slot].get()), physical.slots[slot]);
        }
      }
    };
    const auto handle =
        primary == 0 ? std::uint32_t{0} : primary_handle(primary);
    const auto& first =
        primary == 0 ? inline_primary_ : arena_->bucket(handle);
    visit(first);
    auto next = first.next;
    while (next != kNoBucket) {
      const auto& overflow = arena_->bucket(next);
      visit(overflow);
      next = overflow.next;
    }
  }

  const Storage* members_{nullptr};
  std::shared_ptr<Arena> arena_;
  std::array<std::uint32_t, kGroupWidth> inline_hashes_{};
  Bucket inline_primary_{};
  std::array<std::uint32_t, kTieredPrimaryExtentCount>
      tiered_primary_extent_handles_{};
  std::array<std::uint32_t, kInlineTerminalExtentHandles>
      inline_terminal_extent_handles_{};
  std::uint32_t directory_table_{kNoBucket};
  std::uint32_t* directory_blocks_{nullptr};
  size_type directory_block_count_{0};
  size_type primary_count_{0};
  size_type terminal_extent_count_{0};
  size_type overflow_count_{0};
  size_type arena_cells_{0};
  size_type size_{0};
  size_type level_size_{1};
  size_type split_{0};
  size_type level_mask_{0};
  size_type next_level_mask_{1};
  size_type bucket_count_{1};
  bool split_active_{false};
  std::uint32_t split_cursor_{kPrimaryCursor};
  bool merge_active_{false};
  std::uint32_t merge_cursor_{kPrimaryCursor};
  std::uint32_t merge_source_{kNoBucket};
  std::uint32_t merge_destination_{kNoBucket};
};

}  // namespace goblin::core
