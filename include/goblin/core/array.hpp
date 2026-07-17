#pragma once

// Sparse, index-addressable arrays (Redis-style AR* surface).
//
// Two implementations (same geometry CLI defaults):
//
//   Classic (default, GOBLIN.CLASSIC.AR*, bare AR* unless --realtime-arrays)
//     - Redis-8.8-style sparse arrays: sparse↔dense leaves, auto directory depth
//
//   Realtime (GOBLIN.RT.AR*, or bare AR* with --realtime-arrays)
//     - Fixed geometry and fixed-size dense leaves; index overflow → error
//
// Classic sparse leaves: packed entries (u16 offset + u32 value_id = 6 B).
// Dense leaves: u32 value_ids; hole = ArrayStorage::kEmptyId
// (0xFFFFFFFF). Element bytes live in ArrayStorage (page arena).

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/array_storage.hpp"
#include "goblin/core/page_arena.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

enum class ArrayImplementation : std::uint8_t {
  Classic = 0,
  Realtime = 1,
};

[[nodiscard]] constexpr std::string_view array_implementation_name(
    ArrayImplementation implementation) noexcept {
  switch (implementation) {
    case ArrayImplementation::Classic:
      return "classic";
    case ArrayImplementation::Realtime:
      return "realtime";
  }
  return "unknown";
}

// Packed sparse leaf entry: 6 bytes, no pad.
struct ArraySparseEntry {
  std::uint16_t offset{0};
  std::uint32_t id{ArrayStorage::kEmptyId};
} __attribute__((packed));
static_assert(sizeof(ArraySparseEntry) == 6);

struct ArrayOptions {
  ArrayImplementation implementation{ArrayImplementation::Classic};
  // Logical slots per leaf. Power of two in [2, 65536].
  std::size_t slice_slots{4096};
  // Directory fanout; 0 → same as slice_slots.
  std::size_t dir_fanout{0};
  // Directory depth: start (Classic) or hard fixed (Realtime).
  std::size_t initial_depth{1};
  double sparse_promote_load{0.25};
  double dense_demote_load{0.10};
  std::size_t chunk_bytes{ArrayStorage::kDefaultChunkBytes};
  double arena_growth{kDefaultArenaGrowth};
  // RT favors fewer serving-path relocations. Kept separate so Classic can
  // retain the memory-oriented shared member-index growth policy.
  double realtime_arena_growth{2.0};
  StringEncodingOptions string_encoding{};

  [[nodiscard]] std::size_t effective_dir_fanout() const noexcept {
    return dir_fanout == 0 ? slice_slots : dir_fanout;
  }
};

struct ArrayMemoryStats {
  ArrayImplementation implementation{ArrayImplementation::Classic};
  std::size_t element_count{0};
  std::size_t logical_length{0};
  std::size_t slice_count{0};
  std::size_t sparse_slices{0};
  std::size_t dense_slices{0};
  std::size_t directory_depth{0};
  std::size_t directory_nodes{0};
  std::size_t value_live_bytes{0};
  std::size_t value_dead_bytes{0};
  std::size_t value_allocated_bytes{0};
  std::size_t leaf_table_bytes{0};
  std::size_t total_allocated_bytes{0};
  bool realtime_reserved{false};
  std::uint64_t reserved_max_index{0};
  std::size_t reserved_value_capacity{0};
  std::size_t reserved_value_bytes{0};
};

class Array {
 public:
  using index_type = std::uint64_t;
  using value_id = ArrayStorage::value_id;

  explicit Array(ArrayOptions options = {})
      : options_(normalize(std::move(options))),
        storage_(options_.chunk_bytes,
                 options_.implementation == ArrayImplementation::Realtime
                     ? options_.realtime_arena_growth
                     : options_.arena_growth,
                 options_.string_encoding) {
    slice_shift_ =
        static_cast<std::uint8_t>(std::countr_zero(options_.slice_slots));
    fanout_shift_ = static_cast<std::uint8_t>(
        std::countr_zero(options_.effective_dir_fanout()));
    depth_ = std::max<std::size_t>(1, options_.initial_depth);
    root_ = make_directory(depth_ == 1);
  }

  Array(const Array&) = delete;
  Array& operator=(const Array&) = delete;
  Array(Array&&) noexcept = default;
  Array& operator=(Array&&) noexcept = default;

  [[nodiscard]] const ArrayOptions& options() const noexcept { return options_; }
  [[nodiscard]] ArrayImplementation implementation() const noexcept {
    return options_.implementation;
  }
  [[nodiscard]] bool is_realtime() const noexcept {
    return options_.implementation == ArrayImplementation::Realtime;
  }
  [[nodiscard]] const ArrayStorage& storage() const noexcept { return storage_; }
  [[nodiscard]] ArrayStorage& storage() noexcept { return storage_; }
  void compact_storage() { storage_.compact(); }

  [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
  [[nodiscard]] std::size_t count() const noexcept { return element_count_; }
  [[nodiscard]] std::uint64_t length() const noexcept {
    return element_count_ == 0 ? 0 : max_index_ + 1;
  }
  [[nodiscard]] std::uint64_t next_insert() const noexcept {
    return insert_cursor_;
  }
  [[nodiscard]] index_type max_index_capacity() const noexcept {
    return max_representable_index();
  }
  [[nodiscard]] bool realtime_reserved() const noexcept {
    return realtime_reserved_;
  }
  [[nodiscard]] index_type reserved_max_index() const noexcept {
    return reserved_max_index_;
  }

  // Provision an empty RT array before latency-sensitive writes begin. Every
  // leaf through max_index and all value metadata/bytes are allocated and
  // touched here. A reserved array fails closed instead of allocating beyond
  // these bounds from ARSET/ARMSET/ARINSERT.
  void reserve_realtime(index_type max_index, std::size_t value_capacity,
                        std::size_t value_bytes) {
    if (!is_realtime()) {
      throw std::length_error("ERR ARRESERVE requires an RT array");
    }
    if (realtime_reserved_) {
      throw std::length_error("ERR ARRESERVE requires an unreserved RT array");
    }
    if (max_index > max_representable_index() ||
        (element_count_ != 0 && max_index < max_index_)) {
      throw std::length_error(
          "ERR RT array reservation exceeds fixed index capacity");
    }

    Array prepared(options_);
    prepared.reserve_realtime_in_place(max_index, value_capacity, value_bytes);
    if (element_count_ != 0) {
      scan(0, max_index_, [&](index_type index, std::string value) {
        (void)prepared.set(index, value);
      });
    }
    prepared.insert_cursor_ = insert_cursor_;
    *this = std::move(prepared);
  }

  // Returns true if a new element was created. Throws length_error on RT
  // overflow / leaf full / Classic capacity exhaustion.
  bool set(index_type index, std::string_view value) {
    if (realtime_reserved_ && index > reserved_max_index_) {
      throw std::length_error("ERR RT array reserved index range exhausted");
    }
    ensure_capacity_for(index);
    auto& slice = slice_for_write(index);
    if (is_realtime()) {
      slice.prepare_realtime(options_.slice_slots);
    }
    const auto offset = static_cast<std::uint16_t>(index & slice_mask());
    const auto current_id = slice.get(offset);
    if (current_id != ArrayStorage::kEmptyId) {
      storage_.replace(current_id, value);
      maybe_compact_storage();
      return false;
    }
    const auto new_id = storage_.push(value);
    value_id old_id = ArrayStorage::kEmptyId;
    const bool created =
        slice.set(offset, new_id, old_id, options_, is_realtime());
    if (old_id != ArrayStorage::kEmptyId) {
      storage_.orphan(old_id);
    }
    if (created) {
      ++element_count_;
    }
    if (element_count_ == 1 || index > max_index_) {
      max_index_ = index;
    }
    maybe_compact_storage();
    return created;
  }

  std::size_t set_range(index_type index,
                        std::span<const std::string_view> values) {
    std::size_t written = 0;
    for (const auto value : values) {
      (void)set(index + written, value);
      ++written;
    }
    return written;
  }

  std::size_t mset(
      std::span<const std::pair<index_type, std::string_view>> pairs) {
    std::size_t n = 0;
    for (const auto& [index, value] : pairs) {
      (void)set(index, value);
      ++n;
    }
    return n;
  }

  [[nodiscard]] std::optional<std::string> get(index_type index) const {
    const auto* slice = slice_for_read(index);
    if (slice == nullptr) {
      return std::nullopt;
    }
    const auto offset = static_cast<std::uint16_t>(index & slice_mask());
    const auto id = slice->get(offset);
    if (id == ArrayStorage::kEmptyId) {
      return std::nullopt;
    }
    return storage_.to_string(id);
  }

  bool del(index_type index) {
    auto* slice = slice_for_write_if_present(index);
    if (slice == nullptr) {
      return false;
    }
    const auto offset = static_cast<std::uint16_t>(index & slice_mask());
    value_id old_id = ArrayStorage::kEmptyId;
    if (!slice->del(offset, old_id, options_, is_realtime())) {
      return false;
    }
    storage_.orphan(old_id);
    --element_count_;
    if (element_count_ == 0) {
      max_index_ = 0;
    } else if (index == max_index_) {
      recompute_max_index();
    }
    maybe_compact_storage();
    return true;
  }

  std::size_t del_many(std::span<const index_type> indexes) {
    std::size_t removed = 0;
    for (const auto index : indexes) {
      removed += del(index) ? 1 : 0;
    }
    return removed;
  }

  index_type insert(std::string_view value) {
    const auto index = insert_cursor_;
    (void)set(index, value);
    if (insert_cursor_ < std::numeric_limits<index_type>::max()) {
      ++insert_cursor_;
    }
    return index;
  }

  bool seek(index_type index) {
    insert_cursor_ = index;
    return true;
  }

  template <class Fn>
  void scan(index_type start, index_type end, Fn&& fn) const {
    if (element_count_ == 0 || start > end) {
      return;
    }
    for_each_existing(start, std::min(end, max_index_), std::forward<Fn>(fn));
  }

  [[nodiscard]] ArrayMemoryStats memory_stats() const noexcept {
    ArrayMemoryStats stats;
    stats.implementation = options_.implementation;
    stats.element_count = element_count_;
    stats.logical_length = length();
    stats.directory_depth = depth_;
    stats.value_live_bytes = storage_.live_bytes();
    stats.value_dead_bytes = storage_.dead_bytes();
    stats.value_allocated_bytes = storage_.allocated_bytes();
    stats.realtime_reserved = realtime_reserved_;
    stats.reserved_max_index = reserved_max_index_;
    stats.reserved_value_capacity = storage_.reserved_value_capacity();
    stats.reserved_value_bytes = storage_.reserved_arena_bytes();
    if (root_) {
      root_->accumulate(stats);
    }
    stats.total_allocated_bytes =
        stats.value_allocated_bytes + stats.leaf_table_bytes + sizeof(Array) +
        stats.directory_nodes * sizeof(Directory) +
        stats.slice_count * sizeof(Slice);
    return stats;
  }

  // Canonical snapshot: geometry + implementation + live (index, logical value)
  // pairs in ascending index order. Leaf tables and arena layout rebuild on load.
  void save(snapshot::Writer& writer) const {
    writer.u8(static_cast<std::uint8_t>(options_.implementation));
    writer.u64(static_cast<std::uint64_t>(options_.slice_slots));
    writer.u64(static_cast<std::uint64_t>(options_.effective_dir_fanout()));
    // Classic: recorded depth is a start hint (avoids re-promote thrash).
    // Realtime: hard capacity depth.
    writer.u64(static_cast<std::uint64_t>(depth_));
    writer.f64(options_.sparse_promote_load);
    writer.f64(options_.dense_demote_load);
    writer.u64(insert_cursor_);
    writer.u64(static_cast<std::uint64_t>(element_count_));
    if (element_count_ == 0) {
      return;
    }
    scan(0, max_index_, [&](index_type index, std::string value) {
      writer.u64(index);
      writer.str(value);
    });
  }

  [[nodiscard]] static Array load(snapshot::Reader& reader,
                                  ArrayOptions base_options = {}) {
    ArrayOptions options = normalize(std::move(base_options));
    const auto impl_byte = reader.u8();
    if (impl_byte > static_cast<std::uint8_t>(ArrayImplementation::Realtime)) {
      throw snapshot::snapshot_error("snapshot array implementation unknown");
    }
    options.implementation = static_cast<ArrayImplementation>(impl_byte);
    options.slice_slots = static_cast<std::size_t>(reader.u64());
    options.dir_fanout = static_cast<std::size_t>(reader.u64());
    options.initial_depth = static_cast<std::size_t>(reader.u64());
    options.sparse_promote_load = reader.f64();
    options.dense_demote_load = reader.f64();
    const auto insert_cursor = reader.u64();
    const auto count = reader.u64();
    if (count > std::numeric_limits<std::uint32_t>::max()) {
      throw snapshot::snapshot_error("snapshot array too large");
    }

    Array array(options);
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto index = reader.u64();
      const auto value = reader.str();
      try {
        (void)array.set(index, value);
      } catch (const std::length_error& ex) {
        throw snapshot::snapshot_error(
            std::string("snapshot array load failed: ") + ex.what());
      }
    }
    (void)array.seek(insert_cursor);
    if (array.count() != static_cast<std::size_t>(count)) {
      throw snapshot::snapshot_error("snapshot array element count mismatch");
    }
    return array;
  }

 private:
  static constexpr std::size_t kMinSliceSlots = 2;
  static constexpr std::size_t kMaxSliceSlots = std::size_t{1} << 16;

  static ArrayOptions normalize(ArrayOptions options) {
    if (options.slice_slots < kMinSliceSlots ||
        options.slice_slots > kMaxSliceSlots ||
        !std::has_single_bit(options.slice_slots)) {
      options.slice_slots = 4096;
    }
    const auto fanout = options.effective_dir_fanout();
    if (fanout < 2 || fanout > kMaxSliceSlots || !std::has_single_bit(fanout)) {
      options.dir_fanout = options.slice_slots;
    }
    if (options.initial_depth == 0) {
      options.initial_depth = 1;
    }
    if (options.initial_depth > 16) {
      options.initial_depth = 16;
    }
    if (!(options.sparse_promote_load > 0.0) ||
        options.sparse_promote_load >= 1.0) {
      options.sparse_promote_load = 0.25;
    }
    if (!(options.dense_demote_load > 0.0) ||
        options.dense_demote_load >= options.sparse_promote_load) {
      options.dense_demote_load = options.sparse_promote_load * 0.4;
    }
    if (!std::has_single_bit(options.chunk_bytes) ||
        options.chunk_bytes < ArrayStorage::kMinChunkBytes ||
        options.chunk_bytes > (std::size_t{1} << 31)) {
      options.chunk_bytes = ArrayStorage::kDefaultChunkBytes;
    }
    if (!(options.arena_growth > 1.0)) {
      options.arena_growth = kDefaultArenaGrowth;
    }
    if (!(options.realtime_arena_growth > 1.0)) {
      options.realtime_arena_growth = 2.0;
    }
    return options;
  }

  void reserve_realtime_in_place(index_type max_index,
                                 std::size_t value_capacity,
                                 std::size_t value_bytes) {
    storage_.reserve_realtime(value_capacity, value_bytes);
    const auto last_slice = slice_id_of(max_index);
    for (index_type slice_id = 0; slice_id <= last_slice; ++slice_id) {
      auto& slice = slice_for_write(slice_id << slice_shift_);
      slice.prepare_realtime(options_.slice_slots);
    }
    reserved_max_index_ = max_index;
    realtime_reserved_ = true;
  }

  [[nodiscard]] std::size_t slice_slots() const noexcept {
    return options_.slice_slots;
  }
  [[nodiscard]] std::size_t dir_fanout() const noexcept {
    return options_.effective_dir_fanout();
  }
  [[nodiscard]] index_type slice_mask() const noexcept {
    return static_cast<index_type>(slice_slots() - 1);
  }
  [[nodiscard]] index_type slice_id_of(index_type index) const noexcept {
    return index >> slice_shift_;
  }

  [[nodiscard]] index_type max_representable_index() const noexcept {
    index_type capacity = static_cast<index_type>(slice_slots());
    for (std::size_t level = 0; level < depth_; ++level) {
      if (capacity > std::numeric_limits<index_type>::max() / dir_fanout()) {
        return std::numeric_limits<index_type>::max();
      }
      capacity *= static_cast<index_type>(dir_fanout());
    }
    return capacity - 1;
  }

  void ensure_capacity_for(index_type index) {
    if (index <= max_representable_index()) {
      return;
    }
    if (is_realtime()) {
      throw std::length_error(
          "ERR array index exceeds fixed RT capacity "
          "(raise --array-slice-slots / --array-initial-depth)");
    }
    while (index > max_representable_index() && depth_ < 32) {
      promote_directory();
    }
    if (index > max_representable_index()) {
      throw std::length_error("array index exceeds addressable range");
    }
  }

  void promote_directory() {
    assert(!is_realtime());
    auto new_root = std::make_unique<Directory>(/*leaf=*/false);
    new_root->dirs.resize(1);
    new_root->dirs[0] = std::move(root_);
    root_ = std::move(new_root);
    ++depth_;
  }

  void maybe_compact_storage() {
    if (storage_.should_compact()) {
      storage_.compact();
    }
  }

  // -------------------------------------------------------------------------
  // Leaf: sparse/dense adaptive for Classic; one fixed dense table for RT.
  // Sparse payload: ArraySparseEntry[size] in a page_arena block.
  // Dense payload: value_id[size] with kEmptyId holes.
  // -------------------------------------------------------------------------
  struct Slice {
    enum class Kind : std::uint8_t { Sparse, Dense };

    Kind kind{Kind::Sparse};
    std::uint16_t dense_base{0};
    std::uint32_t live{0};
    // Owned table: page_arena (mmap ≥ page, malloc otherwise).
    std::shared_ptr<char[]> block{};
    std::uint32_t size{0};      // elements in use
    std::uint32_t capacity{0};  // elements allocated

    [[nodiscard]] value_id get(std::uint16_t offset) const {
      if (size == 0 || !block) {
        return ArrayStorage::kEmptyId;
      }
      if (kind == Kind::Sparse) {
        const auto* entries =
            reinterpret_cast<const ArraySparseEntry*>(block.get());
        const auto it = std::lower_bound(
            entries, entries + size, offset,
            [](const ArraySparseEntry& e, std::uint16_t key) {
              return e.offset < key;
            });
        if (it == entries + size || it->offset != offset) {
          return ArrayStorage::kEmptyId;
        }
        return it->id;
      }
      if (offset < dense_base) {
        return ArrayStorage::kEmptyId;
      }
      const auto pos = static_cast<std::uint32_t>(offset - dense_base);
      if (pos >= size) {
        return ArrayStorage::kEmptyId;
      }
      return reinterpret_cast<const value_id*>(block.get())[pos];
    }

    bool set(std::uint16_t offset, value_id new_id, value_id& old_id,
             const ArrayOptions& options, bool realtime) {
      old_id = ArrayStorage::kEmptyId;
      if (realtime) {
        prepare_realtime(options.slice_slots);
        return dense_set(offset, new_id, old_id);
      }
      if (kind == Kind::Sparse) {
        return sparse_set(offset, new_id, old_id, options, realtime);
      }
      return dense_set(offset, new_id, old_id);
    }

    bool del(std::uint16_t offset, value_id& old_id, const ArrayOptions& options,
             bool realtime) {
      old_id = ArrayStorage::kEmptyId;
      if (kind == Kind::Sparse) {
        return sparse_del(offset, old_id);
      }
      return dense_del(offset, old_id, options, realtime);
    }

    void prepare_realtime(std::size_t slots) {
      if (block) {
        return;
      }
      const auto count = static_cast<std::uint32_t>(slots);
      reallocate_dense(count, count, 0);
      live = 0;
    }

    template <class Fn>
    void for_each(std::uint16_t min_off, std::uint16_t max_off, Fn&& fn) const {
      if (size == 0 || !block) {
        return;
      }
      if (kind == Kind::Sparse) {
        const auto* entries =
            reinterpret_cast<const ArraySparseEntry*>(block.get());
        for (std::uint32_t i = 0; i < size; ++i) {
          if (entries[i].offset < min_off) {
            continue;
          }
          if (entries[i].offset > max_off) {
            break;
          }
          fn(entries[i].offset, entries[i].id);
        }
        return;
      }
      const auto* ids = reinterpret_cast<const value_id*>(block.get());
      const auto start = std::max<std::uint32_t>(min_off, dense_base);
      const auto end_ex = static_cast<std::uint32_t>(dense_base) + size;
      const auto stop =
          std::min<std::uint32_t>(static_cast<std::uint32_t>(max_off) + 1,
                                  end_ex);
      for (std::uint32_t off = start; off < stop; ++off) {
        const auto id = ids[off - dense_base];
        if (id != ArrayStorage::kEmptyId) {
          fn(static_cast<std::uint16_t>(off), id);
        }
      }
    }

    void accumulate(ArrayMemoryStats& stats) const {
      ++stats.slice_count;
      if (kind == Kind::Sparse) {
        ++stats.sparse_slices;
        stats.leaf_table_bytes +=
            page_block_alloc_bytes(static_cast<std::size_t>(capacity) *
                                   sizeof(ArraySparseEntry));
      } else {
        ++stats.dense_slices;
        stats.leaf_table_bytes += page_block_alloc_bytes(
            static_cast<std::size_t>(capacity) * sizeof(value_id));
      }
    }

   private:
    void ensure_sparse_cap(std::uint32_t need, bool realtime,
                           std::size_t max_slots) {
      if (need > max_slots) {
        throw std::length_error(
            realtime ? "ERR RT array leaf full "
                       "(raise --array-slice-slots)"
                     : "array leaf capacity exceeded");
      }
      if (need <= capacity) {
        return;
      }
      std::uint32_t new_cap = capacity == 0 ? 4u : capacity;
      while (new_cap < need) {
        const auto grown = new_cap + (new_cap >> 2) + 1;
        new_cap = grown > new_cap ? grown : new_cap + 1;
        if (new_cap > max_slots) {
          new_cap = static_cast<std::uint32_t>(max_slots);
          break;
        }
      }
      if (new_cap < need) {
        throw std::length_error(realtime ? "ERR RT array leaf full "
                                           "(raise --array-slice-slots)"
                                         : "array leaf capacity exceeded");
      }
      reallocate_sparse(new_cap);
    }

    void reallocate_sparse(std::uint32_t new_cap) {
      const auto bytes =
          static_cast<std::size_t>(new_cap) * sizeof(ArraySparseEntry);
      auto fresh = alloc_page_block(bytes);
      if (size != 0 && block) {
        std::memcpy(fresh.get(), block.get(),
                    static_cast<std::size_t>(size) * sizeof(ArraySparseEntry));
      }
      block = std::move(fresh);
      capacity = new_cap;
    }

    void reallocate_dense(std::uint32_t new_cap, std::uint32_t new_size,
                          std::uint16_t new_base) {
      const auto bytes = static_cast<std::size_t>(new_cap) * sizeof(value_id);
      auto fresh = alloc_page_block(bytes);
      auto* ids = reinterpret_cast<value_id*>(fresh.get());
      std::fill(ids, ids + new_cap, ArrayStorage::kEmptyId);
      if (size != 0 && block && kind == Kind::Dense) {
        const auto* old = reinterpret_cast<const value_id*>(block.get());
        for (std::uint32_t i = 0; i < size; ++i) {
          const auto off = static_cast<std::uint32_t>(dense_base) + i;
          if (off >= new_base && off < new_base + new_size) {
            ids[off - new_base] = old[i];
          }
        }
      }
      block = std::move(fresh);
      capacity = new_cap;
      size = new_size;
      dense_base = new_base;
      kind = Kind::Dense;
    }

    bool sparse_set(std::uint16_t offset, value_id new_id, value_id& old_id,
                    const ArrayOptions& options, bool realtime) {
      auto* entries = reinterpret_cast<ArraySparseEntry*>(
          block ? block.get() : nullptr);
      std::uint32_t lo = 0;
      std::uint32_t hi = size;
      while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (entries[mid].offset < offset) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (lo < size && entries[lo].offset == offset) {
        old_id = entries[lo].id;
        entries[lo].id = new_id;
        return false;
      }
      ensure_sparse_cap(size + 1, realtime, options.slice_slots);
      entries = reinterpret_cast<ArraySparseEntry*>(block.get());
      if (lo < size) {
        std::memmove(entries + lo + 1, entries + lo,
                     (size - lo) * sizeof(ArraySparseEntry));
      }
      entries[lo] = ArraySparseEntry{offset, new_id};
      ++size;
      ++live;
      if (!realtime) {
        maybe_promote(options);
      }
      return true;
    }

    bool sparse_del(std::uint16_t offset, value_id& old_id) {
      if (size == 0 || !block) {
        return false;
      }
      auto* entries = reinterpret_cast<ArraySparseEntry*>(block.get());
      std::uint32_t lo = 0;
      std::uint32_t hi = size;
      while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        if (entries[mid].offset < offset) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      if (lo >= size || entries[lo].offset != offset) {
        return false;
      }
      old_id = entries[lo].id;
      if (lo + 1 < size) {
        std::memmove(entries + lo, entries + lo + 1,
                     (size - lo - 1) * sizeof(ArraySparseEntry));
      }
      --size;
      --live;
      if (size == 0) {
        block.reset();
        capacity = 0;
      }
      return true;
    }

    bool dense_set(std::uint16_t offset, value_id new_id, value_id& old_id) {
      ensure_dense_covers(offset);
      auto* ids = reinterpret_cast<value_id*>(block.get());
      const auto pos = static_cast<std::uint32_t>(offset - dense_base);
      old_id = ids[pos];
      const bool was_empty = old_id == ArrayStorage::kEmptyId;
      ids[pos] = new_id;
      if (was_empty) {
        ++live;
        return true;
      }
      return false;
    }

    bool dense_del(std::uint16_t offset, value_id& old_id,
                   const ArrayOptions& options, bool realtime) {
      if (offset < dense_base || size == 0 || !block) {
        return false;
      }
      const auto pos = static_cast<std::uint32_t>(offset - dense_base);
      if (pos >= size) {
        return false;
      }
      auto* ids = reinterpret_cast<value_id*>(block.get());
      if (ids[pos] == ArrayStorage::kEmptyId) {
        return false;
      }
      old_id = ids[pos];
      ids[pos] = ArrayStorage::kEmptyId;
      --live;
      if (!realtime) {
        maybe_demote(options);
      }
      return true;
    }

    void ensure_dense_covers(std::uint16_t offset) {
      if (size == 0 || !block || kind != Kind::Dense) {
        reallocate_dense(1, 1, offset);
        return;
      }
      std::uint16_t new_base = dense_base;
      std::uint32_t new_size = size;
      if (offset < dense_base) {
        const auto grow = static_cast<std::uint32_t>(dense_base - offset);
        new_base = offset;
        new_size = size + grow;
      } else {
        const auto position = static_cast<std::uint32_t>(offset - dense_base);
        if (position < size) {
          return;
        }
        new_size = position + 1;
        if (new_size <= capacity) {
          // reallocate_dense() initializes the full allocation to kEmptyId, so
          // extending the logical window into reserved tail capacity is free.
          size = new_size;
          return;
        }
      }
      std::uint32_t new_cap = capacity;
      while (new_cap < new_size) {
        const auto grown = new_cap + (new_cap >> 2) + 1;
        new_cap = grown > new_cap ? grown : new_cap + 1;
      }
      reallocate_dense(new_cap, new_size, new_base);
    }

    void maybe_promote(const ArrayOptions& options) {
      if (kind != Kind::Sparse || size == 0) {
        return;
      }
      const auto threshold = static_cast<std::uint32_t>(
          options.sparse_promote_load *
          static_cast<double>(options.slice_slots));
      if (live <= std::max<std::uint32_t>(4, threshold)) {
        return;
      }
      const auto* entries =
          reinterpret_cast<const ArraySparseEntry*>(block.get());
      const auto lo = entries[0].offset;
      const auto hi = entries[size - 1].offset;
      const auto win = static_cast<std::uint32_t>(hi - lo) + 1;
      auto fresh =
          alloc_page_block(static_cast<std::size_t>(win) * sizeof(value_id));
      auto* ids = reinterpret_cast<value_id*>(fresh.get());
      std::fill(ids, ids + win, ArrayStorage::kEmptyId);
      for (std::uint32_t i = 0; i < size; ++i) {
        ids[entries[i].offset - lo] = entries[i].id;
      }
      block = std::move(fresh);
      dense_base = lo;
      size = win;
      capacity = win;
      kind = Kind::Dense;
    }

    void maybe_demote(const ArrayOptions& options) {
      if (kind != Kind::Dense) {
        return;
      }
      const auto threshold = static_cast<std::uint32_t>(
          options.dense_demote_load *
          static_cast<double>(options.slice_slots));
      if (live >= std::max<std::uint32_t>(2, threshold)) {
        return;
      }
      const auto* ids = reinterpret_cast<const value_id*>(block.get());
      std::vector<ArraySparseEntry> tmp;
      tmp.reserve(live);
      for (std::uint32_t i = 0; i < size; ++i) {
        if (ids[i] != ArrayStorage::kEmptyId) {
          tmp.push_back(ArraySparseEntry{
              static_cast<std::uint16_t>(dense_base + i), ids[i]});
        }
      }
      const auto n = static_cast<std::uint32_t>(tmp.size());
      if (n == 0) {
        block.reset();
        size = 0;
        capacity = 0;
        live = 0;
        kind = Kind::Sparse;
        return;
      }
      auto fresh = alloc_page_block(static_cast<std::size_t>(n) *
                                    sizeof(ArraySparseEntry));
      std::memcpy(fresh.get(), tmp.data(), n * sizeof(ArraySparseEntry));
      block = std::move(fresh);
      size = n;
      capacity = n;
      live = n;
      dense_base = 0;
      kind = Kind::Sparse;
    }
  };

  struct Directory {
    bool leaf{true};
    std::vector<std::unique_ptr<Slice>> slices;
    std::vector<std::unique_ptr<Directory>> dirs;

    explicit Directory(bool is_leaf) : leaf(is_leaf) {}

    void accumulate(ArrayMemoryStats& stats) const {
      ++stats.directory_nodes;
      if (leaf) {
        for (const auto& slice : slices) {
          if (slice) {
            slice->accumulate(stats);
          }
        }
      } else {
        for (const auto& dir : dirs) {
          if (dir) {
            dir->accumulate(stats);
          }
        }
      }
    }
  };

  std::unique_ptr<Directory> make_directory(bool leaf) const {
    return std::make_unique<Directory>(leaf);
  }

  Slice& slice_for_write(index_type index) {
    const auto sid = slice_id_of(index);
    Directory* dir = root_.get();
    index_type rest = sid;
    for (std::size_t level = depth_; level > 1; --level) {
      const auto shift = fanout_shift_ * (level - 1);
      const auto digit =
          static_cast<std::size_t>((rest >> shift) & (dir_fanout() - 1));
      rest &= (index_type{1} << shift) - 1;
      if (dir->dirs.size() <= digit) {
        dir->dirs.resize(digit + 1);
      }
      if (!dir->dirs[digit]) {
        dir->dirs[digit] = make_directory(level == 2);
      }
      dir = dir->dirs[digit].get();
    }
    const auto leaf_digit = static_cast<std::size_t>(rest);
    if (dir->slices.size() <= leaf_digit) {
      dir->slices.resize(leaf_digit + 1);
    }
    if (!dir->slices[leaf_digit]) {
      dir->slices[leaf_digit] = std::make_unique<Slice>();
    }
    return *dir->slices[leaf_digit];
  }

  Slice* slice_for_write_if_present(index_type index) {
    if (slice_for_read(index) == nullptr) {
      return nullptr;
    }
    return &slice_for_write(index);
  }

  [[nodiscard]] const Slice* slice_for_read(index_type index) const {
    if (index > max_representable_index()) {
      return nullptr;
    }
    const auto sid = slice_id_of(index);
    const Directory* dir = root_.get();
    index_type rest = sid;
    for (std::size_t level = depth_; level > 1; --level) {
      const auto shift = fanout_shift_ * (level - 1);
      const auto digit =
          static_cast<std::size_t>((rest >> shift) & (dir_fanout() - 1));
      rest &= (index_type{1} << shift) - 1;
      if (digit >= dir->dirs.size() || !dir->dirs[digit]) {
        return nullptr;
      }
      dir = dir->dirs[digit].get();
    }
    const auto leaf_digit = static_cast<std::size_t>(rest);
    if (leaf_digit >= dir->slices.size() || !dir->slices[leaf_digit]) {
      return nullptr;
    }
    return dir->slices[leaf_digit].get();
  }

  template <class Fn>
  void for_each_existing(index_type start, index_type end, Fn&& fn) const {
    const auto first_sid = slice_id_of(start);
    const auto last_sid = slice_id_of(end);
    for (index_type sid = first_sid; sid <= last_sid; ++sid) {
      const auto* slice = slice_for_read(sid << slice_shift_);
      if (slice == nullptr) {
        continue;
      }
      const std::uint16_t min_off =
          sid == first_sid ? static_cast<std::uint16_t>(start & slice_mask())
                           : std::uint16_t{0};
      const std::uint16_t max_off =
          sid == last_sid
              ? static_cast<std::uint16_t>(end & slice_mask())
              : static_cast<std::uint16_t>(slice_mask());
      slice->for_each(min_off, max_off, [&](std::uint16_t off, value_id id) {
        const index_type idx =
            (sid << slice_shift_) | static_cast<index_type>(off);
        fn(idx, storage_.to_string(id));
      });
    }
  }

  void recompute_max_index() {
    max_index_ = 0;
    if (element_count_ == 0) {
      return;
    }
    for_each_existing(0, std::numeric_limits<index_type>::max(),
                      [&](index_type index, const std::string&) {
                        if (index > max_index_) {
                          max_index_ = index;
                        }
                      });
  }

  ArrayOptions options_;
  ArrayStorage storage_;
  std::uint8_t slice_shift_{12};
  std::uint8_t fanout_shift_{12};
  std::size_t depth_{1};
  std::unique_ptr<Directory> root_;
  std::size_t element_count_{0};
  index_type max_index_{0};
  index_type insert_cursor_{0};
  index_type reserved_max_index_{0};
  bool realtime_reserved_{false};
};

}  // namespace goblin::core
