#pragma once

// Shared fixed-record storage for real-time linear-hash indexes.
//
// The list value arenas established the allocation policy used here: a small
// vector of page-backed slabs, stable integer addressing, and one mmap/lock
// decision per slab rather than one allocator object per collection. RT index
// slabs differ in one important respect: the entire configured pool is
// allocated and prefaulted up front. Serving mutations only pop or push a
// 32-bit cell handle; they never call malloc/mmap and never move a live bucket.
//
// Each physical bucket stores a 32-bit XXH3-derived hash beside every slot so
// split/merge migration never rehashes field bytes. A separate prefaulted pool
// supplies flat primary-directory blocks; hashes allocate those blocks without
// malloc/mmap or moving an existing directory entry.

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "goblin/core/page_arena.hpp"

namespace goblin::core {

template <class Meta>
class LinearHashArena {
 public:
  static constexpr std::size_t kGroupWidth = 16;
  static constexpr std::size_t kTieredPrimaryExtentCount = 6;
  static constexpr std::array<std::size_t, kTieredPrimaryExtentCount>
      kTieredPrimaryExtentBuckets{4, 8, 16, 32, 64, 128};
  static constexpr std::size_t kTieredPrimaryBuckets = 252;
  static constexpr std::size_t kTerminalPrimaryExtentBuckets = 64;
  static constexpr std::size_t kTerminalPrimaryExtentShift = 6;
  static constexpr std::size_t kTerminalPrimaryExtentMask = 63;
  static constexpr std::size_t kInlineTerminalExtentHandles = 4;
  static constexpr std::size_t kPrimaryDirectoryBlockEntries = 512;
  static constexpr std::size_t kDefaultBytes = std::size_t{16} << 20;
  static constexpr std::size_t kSlabBytes = std::size_t{2} << 20;
  static constexpr std::uint32_t kNoCell =
      std::numeric_limits<std::uint32_t>::max();
  static_assert(kTieredPrimaryExtentBuckets[0] == 4);
  static_assert(kTieredPrimaryExtentBuckets.back() == 128);

  struct alignas(64) HotBucket {
    std::array<std::uint32_t, kGroupWidth> hashes{};
  };
  static_assert(sizeof(HotBucket) == 64);

  struct Bucket {
    std::array<Meta, kGroupWidth> slots{};
    std::uint32_t next{kNoCell};
    std::uint32_t previous{kNoCell};
    std::uint32_t owner{kNoCell};
    std::uint16_t occupied{0};
    std::uint8_t size{0};
  };

 private:
  // Hashes live in a parallel 64-byte-aligned plane. The cold cell contains
  // member ids and links; a handle indexes both planes.
  union alignas(16) Cell {
    Bucket bucket;
    std::uint32_t next_free;

    Cell() noexcept {}
    ~Cell() noexcept {}
  };
  static constexpr std::size_t kCellBytes = sizeof(HotBucket) + sizeof(Cell);
  // The physical buddy/TLB unit stays 256 buckets even though terminal primary
  // extents are smaller. Four 64-bucket terminal extents share the 16 KiB hot
  // plane of one superblock when they occupy the same buddy superblock.
  static constexpr std::size_t kSuperblockBuckets = 256;
  static constexpr std::size_t kBuddyOrderCount = 9;
  static constexpr std::size_t kSuperblocksPerSlab =
      (kSlabBytes / kCellBytes) / kSuperblockBuckets;
  static constexpr std::size_t kCellsPerSlab =
      kSuperblocksPerSlab * kSuperblockBuckets;
  static constexpr std::size_t kHotSlabBytes = kCellsPerSlab * sizeof(HotBucket);
  static constexpr std::size_t kColdSlabBytes = kCellsPerSlab * sizeof(Cell);
  static_assert(sizeof(Cell) == sizeof(Bucket));
  static_assert(kSuperblocksPerSlab != 0);
  static_assert(kCellsPerSlab != 0);

  // Free bitmaps for buddy orders 0..8. Orders 0 and 1 need four and two
  // words; every larger order fits in one. A superblock begins as one free
  // order-8 run and is split/coalesced without touching live cells.
  static constexpr std::size_t kBuddyBitmapWords = 13;
  struct BuddySuperblock {
    std::array<std::uint64_t, kBuddyBitmapWords> free{};
    std::array<std::uint32_t, kBuddyOrderCount> available_position{};

    BuddySuperblock() noexcept { available_position.fill(kNoCell); }
  };

 public:
  static_assert(std::is_trivially_destructible_v<Bucket>);
  static_assert(std::is_nothrow_default_constructible_v<Bucket>);

  explicit LinearHashArena(std::size_t configured_bytes = kDefaultBytes) {
    if (configured_bytes == 0) {
      configured_bytes = kDefaultBytes;
    }
    const auto requested_slabs =
        (configured_bytes + kSlabBytes - 1) / kSlabBytes;
    const auto max_slabs_by_handle =
        static_cast<std::size_t>(kNoCell) / kCellsPerSlab;
    if (requested_slabs == 0 || requested_slabs > max_slabs_by_handle) {
      throw std::length_error("real-time hash index arena is too large");
    }
    hot_slabs_.reserve(requested_slabs);
    slabs_.reserve(requested_slabs);
    for (std::size_t slab = 0; slab < requested_slabs; ++slab) {
      auto hot = alloc_page_block(kHotSlabBytes);
      auto block = alloc_page_block(kColdSlabBytes);
      // Explicit writes make residency independent of mmap's lazy-zero policy.
      std::memset(hot.get(), 0, kHotSlabBytes);
      std::memset(block.get(), 0, kColdSlabBytes);
      hot_slabs_.push_back(std::move(hot));
      slabs_.push_back(std::move(block));
    }
    capacity_ = slabs_.size() * kCellsPerSlab;
    superblock_capacity_ = slabs_.size() * kSuperblocksPerSlab;
    buddy_.resize(superblock_capacity_);
    for (auto& available : available_superblocks_) {
      available.reserve(superblock_capacity_);
    }
    for (std::size_t superblock = superblock_capacity_; superblock != 0;
         --superblock) {
      auto& state = buddy_[superblock - 1];
      set_free(state, 8, 0);
      add_available_superblock(static_cast<std::uint32_t>(superblock - 1), 8);
    }
    const auto maximum_terminal_extents =
        capacity_ <= kTieredPrimaryBuckets
            ? 0
            : (capacity_ - kTieredPrimaryBuckets +
               kTerminalPrimaryExtentBuckets - 1) /
                  kTerminalPrimaryExtentBuckets;
    primary_directory_slots_per_index_ =
        maximum_terminal_extents <= kInlineTerminalExtentHandles
            ? 0
            : (maximum_terminal_extents - kInlineTerminalExtentHandles +
               kPrimaryDirectoryBlockEntries - 1) /
                  kPrimaryDirectoryBlockEntries;
    // The first external terminal-extent handle requires all six tiered
    // extents and five 64-bucket terminal extents. This bound covers every
    // possible partition of the cell pool across hashes.
    constexpr auto kCellsBeforeExternalDirectory =
        kTieredPrimaryBuckets +
        (kInlineTerminalExtentHandles + 1) * kTerminalPrimaryExtentBuckets;
    directory_block_capacity_ = capacity_ / kCellsBeforeExternalDirectory;
    const auto directory_entries =
        directory_block_capacity_ * kPrimaryDirectoryBlockEntries;
    const auto directory_bytes = directory_entries * sizeof(std::uint32_t);
    directory_storage_ = alloc_page_block(directory_bytes);
    directory_allocated_bytes_ = page_block_alloc_bytes(directory_bytes);
    std::memset(directory_storage_.get(), 0xff, directory_bytes);

    directory_table_capacity_ = directory_block_capacity_;
    const auto directory_table_entries =
        directory_table_capacity_ * primary_directory_slots_per_index_;
    const auto directory_table_bytes =
        directory_table_entries * sizeof(std::uint32_t);
    directory_table_storage_ = alloc_page_block(directory_table_bytes);
    directory_table_allocated_bytes_ =
        page_block_alloc_bytes(directory_table_bytes);
    std::memset(directory_table_storage_.get(), 0xff,
                directory_table_bytes);
  }

  LinearHashArena(const LinearHashArena&) = delete;
  LinearHashArena& operator=(const LinearHashArena&) = delete;

  [[nodiscard]] std::uint32_t allocate_bucket(std::uint32_t owner) {
    const auto handle = allocate_run(0);
    auto* value = std::construct_at(&cell(handle).bucket);
    reset_bucket(*value, owner);
    ++live_cells_;
    return handle;
  }

  [[nodiscard]] std::uint32_t allocate_uninitialized_primary_extent(
      std::size_t buckets) {
    const auto order = extent_order(buckets);
    const auto base = allocate_run(order);
    live_cells_ += buckets;
    live_primary_extent_cells_ += buckets;
    return base;
  }

  void initialize_primary_bucket(std::uint32_t handle,
                                 std::uint32_t owner) noexcept {
    auto* value = std::construct_at(&cell(handle).bucket);
    reset_bucket(*value, owner);
  }

  void release_primary_extent(std::uint32_t base,
                              std::size_t buckets) noexcept {
    release_run(base, extent_order(buckets));
    live_cells_ -= buckets;
    live_primary_extent_cells_ -= buckets;
  }

  void release(std::uint32_t handle) noexcept {
    if (handle == kNoCell) {
      return;
    }
    release_run(handle, 0);
    --live_cells_;
  }

  [[nodiscard]] Bucket& bucket(std::uint32_t handle) noexcept {
    return cell(handle).bucket;
  }
  [[nodiscard]] const Bucket& bucket(std::uint32_t handle) const noexcept {
    return cell(handle).bucket;
  }
  [[nodiscard]] HotBucket& hot_bucket(std::uint32_t handle) noexcept {
    return hot_cell(handle);
  }
  [[nodiscard]] const HotBucket& hot_bucket(
      std::uint32_t handle) const noexcept {
    return hot_cell(handle);
  }
  [[nodiscard]] std::uint32_t allocate_primary_directory_block() {
    std::uint32_t block = kNoCell;
    if (directory_free_head_ != kNoCell) {
      block = directory_free_head_;
      directory_free_head_ = primary_directory_entry(block, 0);
    } else {
      if (directory_next_unused_ >= directory_block_capacity_) {
        throw std::length_error(
            "real-time hash primary-directory pool exhausted; increase "
            "--rt-hash-index-bytes");
      }
      block = static_cast<std::uint32_t>(directory_next_unused_++);
    }
    ++live_directory_blocks_;
    return block;
  }

  void release_primary_directory_block(std::uint32_t block) noexcept {
    if (block == kNoCell) {
      return;
    }
    primary_directory_entry(block, 0) = directory_free_head_;
    directory_free_head_ = block;
    --live_directory_blocks_;
  }

  [[nodiscard]] std::uint32_t allocate_primary_directory_table() {
    std::uint32_t table = kNoCell;
    if (directory_table_free_head_ != kNoCell) {
      table = directory_table_free_head_;
      directory_table_free_head_ = primary_directory_table(table)[0];
    } else {
      if (directory_table_next_unused_ >= directory_table_capacity_) {
        throw std::length_error(
            "real-time hash primary-directory table pool exhausted; increase "
            "--rt-hash-index-bytes");
      }
      table = static_cast<std::uint32_t>(directory_table_next_unused_++);
    }
    ++live_directory_tables_;
    return table;
  }

  void release_primary_directory_table(std::uint32_t table) noexcept {
    if (table == kNoCell) {
      return;
    }
    primary_directory_table(table)[0] = directory_table_free_head_;
    directory_table_free_head_ = table;
    --live_directory_tables_;
  }

  [[nodiscard]] std::uint32_t* primary_directory_table(
      std::uint32_t table) noexcept {
    assert(table < directory_table_capacity_);
    return reinterpret_cast<std::uint32_t*>(directory_table_storage_.get()) +
           static_cast<std::size_t>(table) *
               primary_directory_slots_per_index_;
  }
  [[nodiscard]] const std::uint32_t* primary_directory_table(
      std::uint32_t table) const noexcept {
    assert(table < directory_table_capacity_);
    return reinterpret_cast<const std::uint32_t*>(
               directory_table_storage_.get()) +
           static_cast<std::size_t>(table) *
               primary_directory_slots_per_index_;
  }

  [[nodiscard]] std::uint32_t& primary_directory_entry(
      std::uint32_t block, std::size_t offset) noexcept {
    assert(block < directory_block_capacity_);
    assert(offset < kPrimaryDirectoryBlockEntries);
    return primary_directory_block(block)[offset];
  }
  [[nodiscard]] const std::uint32_t& primary_directory_entry(
      std::uint32_t block, std::size_t offset) const noexcept {
    assert(block < directory_block_capacity_);
    assert(offset < kPrimaryDirectoryBlockEntries);
    return primary_directory_block(block)[offset];
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t live_cells() const noexcept { return live_cells_; }
  [[nodiscard]] std::size_t available_cells() const noexcept {
    return capacity_ - live_cells_;
  }
  [[nodiscard]] bool can_allocate_primary_extent(
      std::size_t buckets) const noexcept {
    const auto wanted = extent_order(buckets);
    for (std::size_t order = wanted; order < kBuddyOrderCount; ++order) {
      if (!available_superblocks_[order].empty()) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] std::size_t directory_block_capacity() const noexcept {
    return directory_block_capacity_;
  }
  [[nodiscard]] std::size_t primary_directory_slots_per_index()
      const noexcept {
    return primary_directory_slots_per_index_;
  }
  [[nodiscard]] std::size_t available_directory_blocks() const noexcept {
    return directory_block_capacity_ - live_directory_blocks_;
  }
  [[nodiscard]] std::size_t available_directory_tables() const noexcept {
    return directory_table_capacity_ - live_directory_tables_;
  }
  [[nodiscard]] std::size_t live_directory_bytes() const noexcept {
    return live_directory_blocks_ * directory_block_bytes() +
           live_directory_tables_ * primary_directory_table_bytes();
  }
  [[nodiscard]] std::size_t live_primary_extent_cells() const noexcept {
    return live_primary_extent_cells_;
  }
  [[nodiscard]] static constexpr std::size_t directory_block_bytes() noexcept {
    return kPrimaryDirectoryBlockEntries * sizeof(std::uint32_t);
  }
  [[nodiscard]] std::size_t primary_directory_table_bytes() const noexcept {
    return primary_directory_slots_per_index_ * sizeof(std::uint32_t);
  }
  [[nodiscard]] std::size_t cell_bytes() const noexcept { return kCellBytes; }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return slabs_.size() *
               (page_block_alloc_bytes(kHotSlabBytes) +
                page_block_alloc_bytes(kColdSlabBytes)) +
           slabs_.capacity() * sizeof(slabs_[0]) +
           hot_slabs_.capacity() * sizeof(hot_slabs_[0]) +
           buddy_.capacity() * sizeof(buddy_[0]) +
           available_superblock_bytes() +
           directory_allocated_bytes_ + directory_table_allocated_bytes_;
  }

  static void reset_bucket(Bucket& bucket, std::uint32_t owner) noexcept {
    bucket.next = kNoCell;
    bucket.previous = kNoCell;
    bucket.owner = owner;
    bucket.occupied = 0;
    bucket.size = 0;
  }

 private:
  [[nodiscard]] static constexpr std::size_t extent_order(
      std::size_t buckets) noexcept {
    assert(buckets >= 1 && buckets <= kSuperblockBuckets &&
           std::has_single_bit(buckets));
    return std::countr_zero(buckets);
  }

  [[nodiscard]] static constexpr std::size_t bitmap_offset(
      std::size_t order) noexcept {
    return order == 0 ? 0 : (order == 1 ? 4 : order + 4);
  }

  [[nodiscard]] static constexpr std::size_t bitmap_words(
      std::size_t order) noexcept {
    return order == 0 ? 4 : (order == 1 ? 2 : 1);
  }

  [[nodiscard]] static bool has_free(const BuddySuperblock& state,
                                     std::size_t order) noexcept {
    const auto offset = bitmap_offset(order);
    for (std::size_t word = 0; word < bitmap_words(order); ++word) {
      if (state.free[offset + word] != 0) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static bool is_free(const BuddySuperblock& state,
                                    std::size_t order,
                                    std::size_t block) noexcept {
    const auto word = block >> 6;
    const auto bit = std::uint64_t{1} << (block & 63);
    return (state.free[bitmap_offset(order) + word] & bit) != 0;
  }

  static void set_free(BuddySuperblock& state, std::size_t order,
                       std::size_t block) noexcept {
    const auto word = block >> 6;
    state.free[bitmap_offset(order) + word] |=
        std::uint64_t{1} << (block & 63);
  }

  static void clear_free(BuddySuperblock& state, std::size_t order,
                         std::size_t block) noexcept {
    const auto word = block >> 6;
    state.free[bitmap_offset(order) + word] &=
        ~(std::uint64_t{1} << (block & 63));
  }

  [[nodiscard]] static std::size_t first_free_block(
      const BuddySuperblock& state, std::size_t order) noexcept {
    const auto offset = bitmap_offset(order);
    for (std::size_t word = 0; word < bitmap_words(order); ++word) {
      const auto bits = state.free[offset + word];
      if (bits != 0) {
        return word * 64 + std::countr_zero(bits);
      }
    }
    assert(false);
    return 0;
  }

  void add_available_superblock(std::uint32_t superblock,
                                std::size_t order) noexcept {
    auto& state = buddy_[superblock];
    assert(state.available_position[order] == kNoCell &&
           has_free(state, order));
    auto& available = available_superblocks_[order];
    state.available_position[order] =
        static_cast<std::uint32_t>(available.size());
    available.push_back(superblock);
  }

  void remove_available_superblock(std::uint32_t superblock,
                                   std::size_t order) noexcept {
    auto& state = buddy_[superblock];
    const auto position = state.available_position[order];
    if (position == kNoCell) {
      return;
    }
    auto& available = available_superblocks_[order];
    const auto moved = available.back();
    available[position] = moved;
    buddy_[moved].available_position[order] = position;
    available.pop_back();
    state.available_position[order] = kNoCell;
  }

  [[nodiscard]] std::uint32_t allocate_run(std::size_t wanted_order) {
    std::size_t source_order = wanted_order;
    while (source_order < kBuddyOrderCount &&
           available_superblocks_[source_order].empty()) {
      ++source_order;
    }
    if (source_order == kBuddyOrderCount) {
      throw std::length_error(
          "real-time hash index arena is fragmented or exhausted; increase "
          "--rt-hash-index-bytes");
    }

    const auto superblock = available_superblocks_[source_order].back();
    auto& state = buddy_[superblock];
    auto block = first_free_block(state, source_order);
    clear_free(state, source_order, block);
    if (!has_free(state, source_order)) {
      remove_available_superblock(superblock, source_order);
    }

    while (source_order > wanted_order) {
      --source_order;
      block <<= 1;
      const bool had_free = has_free(state, source_order);
      set_free(state, source_order, block + 1);
      if (!had_free) {
        add_available_superblock(superblock, source_order);
      }
    }
    return static_cast<std::uint32_t>(
        superblock * kSuperblockBuckets +
        block * (std::size_t{1} << wanted_order));
  }

  void release_run(std::uint32_t handle, std::size_t order) noexcept {
    assert(handle < capacity_);
    const auto superblock = handle / kSuperblockBuckets;
    const auto offset = handle & (kSuperblockBuckets - 1);
    assert((offset & ((std::size_t{1} << order) - 1)) == 0);
    auto block = static_cast<std::size_t>(offset) >> order;
    auto& state = buddy_[superblock];

    while (order + 1 < kBuddyOrderCount) {
      const auto buddy_block = block ^ 1U;
      if (!is_free(state, order, buddy_block)) {
        break;
      }
      clear_free(state, order, buddy_block);
      if (!has_free(state, order)) {
        remove_available_superblock(superblock, order);
      }
      block >>= 1;
      ++order;
    }
    const bool had_free = has_free(state, order);
    assert(!is_free(state, order, block));
    set_free(state, order, block);
    if (!had_free) {
      add_available_superblock(superblock, order);
    }
  }

  [[nodiscard]] std::size_t available_superblock_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& available : available_superblocks_) {
      bytes += available.capacity() * sizeof(available[0]);
    }
    return bytes;
  }

  [[nodiscard]] Cell& cell(std::uint32_t handle) noexcept {
    const auto slab = static_cast<std::size_t>(handle) / kCellsPerSlab;
    const auto offset = static_cast<std::size_t>(handle) % kCellsPerSlab;
    return *(reinterpret_cast<Cell*>(slabs_[slab].get()) + offset);
  }
  [[nodiscard]] const Cell& cell(std::uint32_t handle) const noexcept {
    const auto slab = static_cast<std::size_t>(handle) / kCellsPerSlab;
    const auto offset = static_cast<std::size_t>(handle) % kCellsPerSlab;
    return *(reinterpret_cast<const Cell*>(slabs_[slab].get()) + offset);
  }

  [[nodiscard]] HotBucket& hot_cell(std::uint32_t handle) noexcept {
    const auto slab = static_cast<std::size_t>(handle) / kCellsPerSlab;
    const auto offset = static_cast<std::size_t>(handle) % kCellsPerSlab;
    return *(reinterpret_cast<HotBucket*>(hot_slabs_[slab].get()) + offset);
  }
  [[nodiscard]] const HotBucket& hot_cell(std::uint32_t handle) const noexcept {
    const auto slab = static_cast<std::size_t>(handle) / kCellsPerSlab;
    const auto offset = static_cast<std::size_t>(handle) % kCellsPerSlab;
    return *(reinterpret_cast<const HotBucket*>(hot_slabs_[slab].get()) +
             offset);
  }

  [[nodiscard]] std::uint32_t* primary_directory_block(
      std::uint32_t block) noexcept {
    return reinterpret_cast<std::uint32_t*>(directory_storage_.get()) +
           static_cast<std::size_t>(block) *
               kPrimaryDirectoryBlockEntries;
  }
  [[nodiscard]] const std::uint32_t* primary_directory_block(
      std::uint32_t block) const noexcept {
    return reinterpret_cast<const std::uint32_t*>(directory_storage_.get()) +
           static_cast<std::size_t>(block) *
               kPrimaryDirectoryBlockEntries;
  }

  std::vector<std::shared_ptr<char[]>> hot_slabs_;
  std::vector<std::shared_ptr<char[]>> slabs_;
  std::vector<BuddySuperblock> buddy_;
  std::array<std::vector<std::uint32_t>, kBuddyOrderCount>
      available_superblocks_;
  std::shared_ptr<char[]> directory_storage_;
  std::shared_ptr<char[]> directory_table_storage_;
  std::size_t capacity_{0};
  std::size_t superblock_capacity_{0};
  std::size_t live_cells_{0};
  std::size_t live_primary_extent_cells_{0};
  std::size_t directory_block_capacity_{0};
  std::size_t primary_directory_slots_per_index_{0};
  std::size_t directory_next_unused_{0};
  std::size_t live_directory_blocks_{0};
  std::size_t directory_allocated_bytes_{0};
  std::uint32_t directory_free_head_{kNoCell};
  std::size_t directory_table_capacity_{0};
  std::size_t directory_table_next_unused_{0};
  std::size_t live_directory_tables_{0};
  std::size_t directory_table_allocated_bytes_{0};
  std::uint32_t directory_table_free_head_{kNoCell};
};

}  // namespace goblin::core
