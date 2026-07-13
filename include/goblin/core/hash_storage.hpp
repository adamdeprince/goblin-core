#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/page_arena.hpp"
#include "goblin/core/simd_ops.hpp"

namespace goblin::core {

// Packed field+value storage for a hash. Each entry is addressed by a
// struct-of-arrays reference:
//
//     field_offset (u32) + field_len (u16) + value_len (u16) = 8 bytes / field
//
// Fresh inserts place field and value contiguously (value_offset = field_offset +
// field_len) for HGET locality, so storing value_offset would be redundant. A
// value *grow* re-appends only the value bytes. Relocated offsets live in lazy
// 64-field blocks; a null block and UINT32_MAX within an allocated block both
// mean contiguous. This keeps first-growth work bounded instead of initializing
// one u32 for every field. Same-or-smaller updates overwrite in place.
//
// Both field and value are capped at 64 KiB - 1 (the u16 length, same as zset
// members). Larger values belong in a blob store (goblin-store.dev) with the
// hash holding the key.
//
// The arena chunk size is configurable (--hash-chunk-bytes): a power of two at
// least kMinChunkBytes (large enough to hold the biggest field+value blob).
// Smaller chunks lower the floor for hashes of big blobs; larger chunks reduce
// boundary skips. A blob never straddles a chunk (so field()/value() are
// contiguous).
//
// Fragmentation: a same-or-smaller value update overwrites in place; a larger
// value re-appends the value only and orphans the old value bytes; a removed
// field orphans field+value. Orphaned (dead) bytes are tracked per chunk.
//
// Bounded compaction is **tail-donor**:
//   1. Pick a holey frozen chunk (not the active tail).
//   2. Densify that chunk's live bytes to the front (clear internal holes).
//   3. Fill remaining free capacity by moving live blobs *from the active tail*
//      into the target (SIMD bitset 0/1 knapsack / subset-sum when the free
//      window is modest; first-fit decreasing for multi-MiB free regions).
//   4. Densify and shrink the active tail so only the last block grows/shrinks
//      with demand. Frozen blocks stay full-size (huge-page backed when enabled).
// Fully-dead frozen chunks are released without a densify pass.
class HashStorage {
 public:
  using size_type = std::size_t;

  enum class CompactionPhase : std::uint8_t {
    idle,
    select,
    densify,      // repack live bytes in the target chunk
    donate,       // pull from active tail into free space
    shrink_tail,  // densify + shrink the active block
  };

  struct CompactionProgress {
    CompactionPhase phase{CompactionPhase::idle};
    std::uint32_t victim_chunk{std::numeric_limits<std::uint32_t>::max()};
    size_type fields_total{0};
    size_type fields_scanned{0};
    size_type candidates_remaining{0};
    size_type relocated_fields{0};
    size_type relocated_bytes{0};

    [[nodiscard]] bool active() const noexcept {
      return phase != CompactionPhase::idle;
    }
  };

  struct CompactionStepResult {
    CompactionProgress progress{};
    size_type work_done{0};
    size_type bytes_moved{0};
    size_type bytes_reclaimed{0};
    bool started{false};
    bool completed{false};
  };

  static constexpr size_type kMaxFieldBytes =
      std::numeric_limits<std::uint16_t>::max();
  static constexpr size_type kMaxValueBytes =
      std::numeric_limits<std::uint16_t>::max();
  static constexpr size_type kDefaultChunkBytes = size_type{1} << 21;  // 2 MiB (x86 huge page)
  // Must hold the largest possible blob (field + value = 2 * 64 KiB) in one
  // chunk, rounded to the next power of two: 2^17 = 128 KiB.
  static constexpr size_type kMinChunkBytes = size_type{1} << 17;

  explicit HashStorage(size_type chunk_bytes = kDefaultChunkBytes,
                       double growth = kDefaultArenaGrowth) {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes ||
        chunk_bytes > (size_type{1} << 31)) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
    growth_ = growth > 1.0 ? growth : kDefaultArenaGrowth;
  }

  [[nodiscard]] size_type chunk_bytes() const noexcept { return chunk_bytes_; }
  [[nodiscard]] bool empty() const noexcept { return field_offsets_.empty(); }
  [[nodiscard]] size_type size() const noexcept { return field_offsets_.size(); }
  [[nodiscard]] size_type byte_size() const noexcept { return used_bytes_; }
  [[nodiscard]] size_type dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }
  [[nodiscard]] size_type byte_capacity() const noexcept {
    return committed_bytes_;
  }
  [[nodiscard]] size_type ref_capacity() const noexcept {
    return field_offsets_.capacity();
  }
  [[nodiscard]] size_type relocation_capacity() const noexcept {
    return relocation_block_count_ == 0
               ? 0
               : relocation_blocks_.size() * kRelocationBlockEntries;
  }
  [[nodiscard]] size_type chunk_slot_count() const noexcept {
    return chunks_.size();
  }
  [[nodiscard]] size_type recycled_chunk_count() const noexcept {
    return free_chunks_.size();
  }

  [[nodiscard]] CompactionProgress compaction_progress() const noexcept {
    CompactionProgress result;
    result.phase = compaction_.phase;
    result.victim_chunk = compaction_.victim_chunk;
    if (compaction_.phase == CompactionPhase::select) {
      result.candidates_remaining =
          compaction_.selection_limit - compaction_.selection_cursor;
    } else if (compaction_.phase == CompactionPhase::densify ||
               compaction_.phase == CompactionPhase::donate ||
               compaction_.phase == CompactionPhase::shrink_tail) {
      result.fields_total = compaction_.fields_total;
      result.fields_scanned = compaction_.fields_scanned;
    }
    result.relocated_fields = compaction_.relocated_fields;
    result.relocated_bytes = compaction_.relocated_bytes;
    return result;
  }

  [[nodiscard]] bool compaction_active() const noexcept {
    return compaction_.phase != CompactionPhase::idle;
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return byte_capacity() +
           field_offsets_.capacity() * sizeof(std::uint32_t) +
           relocation_blocks_.capacity() * sizeof(RelocationBlockPtr) +
           relocation_block_count_ * sizeof(RelocationBlock) +
           field_lengths_.capacity() * sizeof(std::uint16_t) +
           value_lengths_.capacity() * sizeof(std::uint16_t) +
           chunks_.capacity() * sizeof(std::shared_ptr<char[]>) +
           chunk_usage_.capacity() * sizeof(ChunkUsage) +
           free_chunks_.capacity() * sizeof(std::uint32_t);
  }

  void reserve(size_type field_count) {
    field_offsets_.reserve(field_count);
    relocation_blocks_.reserve(relocation_directory_entries(field_count));
    field_lengths_.reserve(field_count);
    value_lengths_.reserve(field_count);
  }

  // Reserve for a growing batch without defeating amortized growth. Calling
  // reserve(size() + batch) for every batch makes the base vectors (and an
  // relocation directory) reallocate at every step; retain the configured
  // growth slack once it exceeds the incoming batch instead.
  void reserve_additional(size_type additional) {
    if (additional == 0) {
      return;
    }
    if (additional > std::numeric_limits<size_type>::max() - size()) {
      throw std::length_error("hash field reference capacity exhausted");
    }

    const auto required = size() + additional;
    auto target = required;
    const auto current = ref_capacity();
    if (current != 0 && current < required) {
      const auto grown = grow_ref_capacity(current);
      if (grown > target) {
        target = grown;
      }
    }
    reserve(target);
  }

  void reserve_bytes(size_type byte_count) {
    const auto chunks = (byte_count + chunk_bytes_ - 1) / chunk_bytes_;
    chunks_.reserve(chunks);
    chunk_usage_.reserve(chunks);
  }

  // Drop every field ref and arena byte; keep chunk/growth config. Used when
  // recycling a Hash object through the keyspace freelist.
  void clear() noexcept {
    chunks_.clear();
    field_offsets_.clear();
    relocation_blocks_.clear();
    relocation_block_count_ = 0;
    field_lengths_.clear();
    value_lengths_.clear();
    chunk_usage_.clear();
    free_chunks_.clear();
    compaction_ = {};
    active_chunk_ = kNoChunk;
    active_offset_ = 0;
    used_bytes_ = 0;
    dead_bytes_ = 0;
    committed_bytes_ = 0;
  }

  // Append a new field with its value; returns its field id (0-based, dense).
  // Places field||value contiguously for cache-local HGET.
  [[nodiscard]] std::uint32_t push_back(std::string_view field,
                                        std::string_view value) {
    if (field_offsets_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("hash field id space exhausted");
    }
    // Make every post-append metadata push non-allocating. If reserve throws,
    // sizes and arena accounting are still unchanged. Refs grow in lockstep via
    // reserve()/reserve_additional(), so one capacity check is enough.
    const auto required = field_offsets_.size() + 1;
    if (field_offsets_.capacity() < required) {
      reserve_additional(1);
    }
    assert(field_lengths_.capacity() >= required);
    assert(value_lengths_.capacity() >= required);
    assert(relocation_blocks_.capacity() >=
           relocation_directory_entries(required));
    const auto field_off = append_bytes(field, value);
    if ((field_offsets_.size() & kRelocationBlockMask) == 0) {
      relocation_blocks_.push_back({});
    }
    field_offsets_.push_back(field_off);
    field_lengths_.push_back(static_cast<std::uint16_t>(field.size()));
    value_lengths_.push_back(static_cast<std::uint16_t>(value.size()));
    return static_cast<std::uint32_t>(field_offsets_.size() - 1);
  }

  // Replace an existing field's value (the field bytes are never moved). Fits in
  // place when the new value is no longer than the old one; otherwise only the
  // value is re-appended and the old value bytes are orphaned.
  void set_value(std::uint32_t field_id, std::string_view value) {
    assert(field_id < field_offsets_.size());
    if (value.size() > kMaxValueBytes) {
      throw std::length_error(
          "hash value too large (max 64 KiB; use a blob store for larger)");
    }
    const size_type old_value_len = value_lengths_[field_id];
    const auto old_value_offset = value_offset(field_id);
    if (value.size() == old_value_len) {
      // Same-width HSET: overwrite only; no dead-byte accounting.
      if (!value.empty()) {
        std::memcpy(chunk_ptr(old_value_offset), value.data(), value.size());
      }
      return;
    }
    if (value.size() < old_value_len) {
      if (!value.empty()) {
        std::memcpy(chunk_ptr(old_value_offset), value.data(), value.size());
      }
      mark_dead(old_value_offset + static_cast<std::uint32_t>(value.size()),
                old_value_len - value.size());
      value_lengths_[field_id] = static_cast<std::uint16_t>(value.size());
      return;
    }
    // Grow: re-append value only; field stays put.
    auto& relocation_block = ensure_relocation_block(field_id);
    const auto new_off = append_bytes({}, value);
    mark_dead(old_value_offset, old_value_len);
    relocation_block[field_id & kRelocationBlockMask] = new_off;
    value_lengths_[field_id] = static_cast<std::uint16_t>(value.size());
  }

  // The field bytes (also what the swiss index compares against).
  [[nodiscard]] std::string_view view(std::uint32_t field_id) const noexcept {
    assert(field_id < field_offsets_.size());
    const auto length = field_lengths_[field_id];
    return length == 0
               ? std::string_view{}
               : std::string_view(chunk_ptr(field_offsets_[field_id]), length);
  }

  // The value bytes (may be contiguous with the field after insert, or relocated
  // after a grow).
  [[nodiscard]] std::string_view value(std::uint32_t field_id) const noexcept {
    assert(field_id < field_offsets_.size());
    const auto value_len = value_lengths_[field_id];
    return value_len == 0
               ? std::string_view{}
               : std::string_view(chunk_ptr(value_offset(field_id)), value_len);
  }

  // Account a to-be-removed field's field+value bytes as dead (the Hash
  // swap-removes it out of the index; bytes stay until compact).
  void orphan(std::uint32_t field_id) noexcept {
    assert(field_id < field_offsets_.size());
    mark_dead(field_offsets_[field_id], field_lengths_[field_id]);
    mark_dead(value_offset(field_id), value_lengths_[field_id]);
  }

  // Copy a field's reference over another's (swap-remove moves the last field
  // into a removed slot).
  void prepare_copy_ref(std::uint32_t dst, std::uint32_t src) {
    assert(dst < field_offsets_.size() && src < field_offsets_.size());
    if (relocated_value_offset(src) != kContiguousValueOffset) {
      (void)ensure_relocation_block(dst);
    }
  }

  void copy_ref(std::uint32_t dst, std::uint32_t src) {
    assert(dst < field_offsets_.size() && src < field_offsets_.size());
    copy_relocated_value_offset(dst, src);
    field_offsets_[dst] = field_offsets_[src];
    field_lengths_[dst] = field_lengths_[src];
    value_lengths_[dst] = value_lengths_[src];
    // Tail-donor densify/donate re-scans field ids when a swap-remove lands a
    // reference that still touches the target or the active donor.
    if (compaction_active()) {
      compaction_.rescan_required = true;
    }
  }

  void pop_back() noexcept {
    assert(!field_offsets_.empty());
    const auto removed_id =
        static_cast<std::uint32_t>(field_offsets_.size() - 1);
    clear_relocated_value_offset(removed_id);
    field_offsets_.pop_back();
    field_lengths_.pop_back();
    value_lengths_.pop_back();
    if ((field_offsets_.size() & kRelocationBlockMask) == 0) {
      assert(!relocation_blocks_.empty());
      assert(!relocation_blocks_.back());
      relocation_blocks_.pop_back();
    }
  }

  // Bounded tail-donor maintenance. `work_budget` counts selection candidates
  // and field visits during densify/donate; `byte_budget` bounds bytes copied
  // from the tail into the target (one field/value unit may exceed it). Safe to
  // interleave with mutations; a swap-remove sets rescan_required.
  [[nodiscard]] CompactionStepResult compact_step(
      size_type work_budget = 256,
      size_type byte_budget = size_type{64} << 10) {
    CompactionStepResult result;
    if (work_budget == 0) {
      result.progress = compaction_progress();
      return result;
    }
    if (!compaction_active()) {
      begin_compaction();
      result.started = true;
    }

    while (result.work_done < work_budget && compaction_active()) {
      if (compaction_.phase == CompactionPhase::select) {
        if (compaction_.selection_cursor < compaction_.selection_limit) {
          inspect_compaction_candidate(compaction_.selection_cursor++);
          ++result.work_done;
          continue;
        }
        if (!finish_compaction_selection()) {
          result.completed = true;
          break;
        }
        continue;
      }

      if (compaction_.phase == CompactionPhase::densify) {
        const auto target = compaction_.victim_chunk;
        auto& usage = chunk_usage_[target];
        if (usage.written == usage.dead) {
          // No live bytes: free the frozen chunk (returns a huge page to the pool).
          if (target != active_chunk_) {
            result.bytes_reclaimed = release_chunk(target);
          } else {
            // Empty active: collapse accounting; keep a tiny active cursor.
            result.bytes_reclaimed = shrink_active_after_densify();
          }
          compaction_ = {};
          result.completed = true;
          break;
        }
        densify_chunk(target);
        ++result.work_done;
        compaction_.fields_scanned = size();
        compaction_.fields_total = size();
        if (target == active_chunk_) {
          // Active-only fragmentation: densify + shrink, no donor.
          result.bytes_reclaimed = shrink_active_after_densify();
          compaction_ = {};
          result.completed = true;
          break;
        }
        compaction_.phase = CompactionPhase::donate;
        compaction_.rescan_required = false;
        continue;
      }

      if (compaction_.phase == CompactionPhase::donate) {
        if (compaction_.rescan_required) {
          compaction_.rescan_required = false;
        }
        const auto target = compaction_.victim_chunk;
        const auto free_space =
            chunk_usage_[target].capacity - chunk_usage_[target].written;
        if (free_space == 0 || active_chunk_ == kNoChunk ||
            active_chunk_ == target) {
          compaction_.phase = CompactionPhase::shrink_tail;
          continue;
        }
        const auto moved =
            donate_from_tail(target, free_space, byte_budget - result.bytes_moved,
                             work_budget - result.work_done, result);
        if (moved == 0 && result.work_done < work_budget) {
          compaction_.phase = CompactionPhase::shrink_tail;
          continue;
        }
        if (result.bytes_moved >= byte_budget ||
            result.work_done >= work_budget) {
          break;  // resume donate/shrink on the next step
        }
        compaction_.phase = CompactionPhase::shrink_tail;
        continue;
      }

      assert(compaction_.phase == CompactionPhase::shrink_tail);
      result.bytes_reclaimed += shrink_active_after_densify();
      ++result.work_done;
      compaction_ = {};
      result.completed = true;
    }
    result.progress = compaction_progress();
    return result;
  }

  // Drain every fragmented chunk in place. Explicit optimization is allowed to
  // do unbounded work, but it must not rebuild the arena: frozen blocks may be
  // backed by a finite HugeTLB pool and replacing them while they are still live
  // forces the replacement blocks onto base pages.
  void compact() {
    constexpr auto kUnlimited = std::numeric_limits<size_type>::max();
    while (compaction_active() || dead_bytes_ != 0) {
      const auto dead_before = dead_bytes_;
      const auto result = compact_step(kUnlimited, kUnlimited);
      if (!result.progress.active() && dead_bytes_ == dead_before) {
        // No candidate despite non-zero dead accounting would otherwise spin
        // forever in a release build.
        assert(dead_bytes_ == 0);
        break;
      }
    }
  }

 private:
  static constexpr std::uint32_t kContiguousValueOffset =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr std::uint32_t kNoChunk =
      std::numeric_limits<std::uint32_t>::max();
  static constexpr size_type kRelocationBlockEntries = 64;
  static constexpr size_type kRelocationBlockMask =
      kRelocationBlockEntries - 1;

  using RelocationBlock =
      std::array<std::uint32_t, kRelocationBlockEntries>;
  using RelocationBlockPtr = std::unique_ptr<RelocationBlock>;

  struct ChunkUsage {
    size_type capacity{0};
    size_type written{0};
    size_type dead{0};
  };

  struct CompactionState {
    CompactionPhase phase{CompactionPhase::idle};
    std::uint32_t victim_chunk{kNoChunk};
    size_type selection_cursor{0};
    size_type selection_limit{0};
    size_type best_dead{0};
    size_type best_written{1};
    size_type fields_total{0};
    size_type fields_scanned{0};
    size_type relocated_fields{0};
    size_type relocated_bytes{0};
    bool rescan_required{false};
  };

  // One movable unit from the active tail into a target chunk.
  struct DonorItem {
    std::uint32_t field_id{0};
    size_type bytes{0};
    bool move_field{false};
    bool move_value{false};
    bool as_contiguous_pair{false};
  };

  [[nodiscard]] std::uint32_t value_offset(
      std::uint32_t field_id) const noexcept {
    const auto offset = relocated_value_offset(field_id);
    if (offset != kContiguousValueOffset) {
      return offset;
    }
    return field_offsets_[field_id] + field_lengths_[field_id];
  }

  [[nodiscard]] bool value_is_contiguous(
      std::uint32_t field_id) const noexcept {
    return relocated_value_offset(field_id) == kContiguousValueOffset;
  }

  [[nodiscard]] static constexpr size_type relocation_directory_entries(
      size_type fields) noexcept {
    return fields / kRelocationBlockEntries +
           (fields % kRelocationBlockEntries != 0 ? 1 : 0);
  }

  [[nodiscard]] std::uint32_t relocated_value_offset(
      std::uint32_t field_id) const noexcept {
    const auto block_index = field_id / kRelocationBlockEntries;
    assert(block_index < relocation_blocks_.size());
    const auto& block = relocation_blocks_[block_index];
    return block ? (*block)[field_id & kRelocationBlockMask]
                 : kContiguousValueOffset;
  }

  [[nodiscard]] RelocationBlock& ensure_relocation_block(
      std::uint32_t field_id) {
    const auto block_index = field_id / kRelocationBlockEntries;
    assert(block_index < relocation_blocks_.size());
    auto& block = relocation_blocks_[block_index];
    if (!block) {
      auto fresh = std::make_unique<RelocationBlock>();
      fresh->fill(kContiguousValueOffset);
      block = std::move(fresh);
      ++relocation_block_count_;
    }
    return *block;
  }

  void set_relocated_value_offset(std::uint32_t field_id,
                                  std::uint32_t offset) {
    ensure_relocation_block(field_id)[field_id & kRelocationBlockMask] = offset;
  }

  void clear_relocated_value_offset(std::uint32_t field_id) noexcept {
    const auto block_index = field_id / kRelocationBlockEntries;
    assert(block_index < relocation_blocks_.size());
    auto& block = relocation_blocks_[block_index];
    if (block) {
      auto& offset = (*block)[field_id & kRelocationBlockMask];
      if (offset != kContiguousValueOffset) {
        offset = kContiguousValueOffset;
        bool any_relocated = false;
        for (const auto candidate : *block) {
          if (candidate != kContiguousValueOffset) {
            any_relocated = true;
            break;
          }
        }
        if (!any_relocated) {
          block.reset();
          --relocation_block_count_;
        }
      }
    }
  }

  void copy_relocated_value_offset(std::uint32_t dst,
                                   std::uint32_t src) {
    const auto offset = relocated_value_offset(src);
    if (offset != kContiguousValueOffset) {
      set_relocated_value_offset(dst, offset);
      return;
    }
    clear_relocated_value_offset(dst);
  }

  [[nodiscard]] std::uint32_t chunk_index(std::uint32_t offset) const noexcept {
    return offset >> chunk_shift_;
  }

  [[nodiscard]] bool offset_references_chunk(
      std::uint32_t offset, size_type length,
      std::uint32_t chunk) const noexcept {
    return length != 0 && chunk_index(offset) == chunk;
  }

  [[nodiscard]] bool references_chunk(std::uint32_t field_id,
                                      std::uint32_t chunk) const noexcept {
    if (field_id >= size()) {
      return false;
    }
    return offset_references_chunk(field_offsets_[field_id],
                                   field_lengths_[field_id], chunk) ||
           offset_references_chunk(value_offset(field_id),
                                   value_lengths_[field_id], chunk);
  }

  void mark_dead(std::uint32_t offset, size_type bytes) noexcept {
    if (bytes == 0) {
      return;
    }
    const auto chunk = chunk_index(offset);
    assert(chunk < chunk_usage_.size());
    assert(chunk_usage_[chunk].dead + bytes <= chunk_usage_[chunk].written);
    chunk_usage_[chunk].dead += bytes;
    dead_bytes_ += bytes;
  }

  [[nodiscard]] const char* chunk_ptr(std::uint32_t offset) const noexcept {
    return chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_);
  }
  [[nodiscard]] char* chunk_ptr(std::uint32_t offset) noexcept {
    return chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_);
  }

  [[nodiscard]] std::uint32_t encode_offset(std::uint32_t chunk,
                                            size_type offset) const {
    const auto max_chunk =
        std::numeric_limits<std::uint32_t>::max() >> chunk_shift_;
    if (chunk > max_chunk || offset > chunk_mask_) {
      throw std::length_error("hash arena exhausted");
    }
    const auto encoded = static_cast<std::uint32_t>(
        (static_cast<size_type>(chunk) << chunk_shift_) | offset);
    // The optional value-offset sidecar reserves UINT32_MAX as its contiguous
    // sentinel. Sacrifice that final arena byte rather than make it ambiguous.
    if (encoded == kContiguousValueOffset) {
      throw std::length_error("hash arena exhausted");
    }
    return encoded;
  }

  void make_active_chunk(size_type first_run_bytes) {
    const auto wanted =
        std::min(chunk_bytes_, std::max(kInitialArenaBytes, first_run_bytes));
    auto block = alloc_page_block(wanted);
    std::uint32_t chunk;
    if (!free_chunks_.empty()) {
      chunk = free_chunks_.back();
      free_chunks_.pop_back();
      assert(chunk < chunks_.size() && !chunks_[chunk]);
    } else {
      const auto max_chunk =
          std::numeric_limits<std::uint32_t>::max() >> chunk_shift_;
      if (chunks_.size() > max_chunk) {
        throw std::length_error("hash arena exhausted");
      }
      chunks_.reserve(chunks_.size() + 1);
      chunk_usage_.reserve(chunk_usage_.size() + 1);
      chunk = static_cast<std::uint32_t>(chunks_.size());
      chunks_.push_back({});
      chunk_usage_.push_back({});
    }

    chunks_[chunk] = std::move(block);
    auto& usage = chunk_usage_[chunk];
    usage = ChunkUsage{.capacity = page_block_alloc_bytes(wanted)};
    committed_bytes_ += usage.capacity;
    active_chunk_ = chunk;
    active_offset_ = 0;
  }

  void ensure_active_capacity(size_type needed) {
    assert(active_chunk_ != kNoChunk);
    auto& usage = chunk_usage_[active_chunk_];
    if (needed <= usage.capacity && chunks_[active_chunk_].use_count() == 1) {
      return;
    }
    const auto wanted = needed <= usage.capacity
                            ? usage.capacity
                            : std::min(chunk_bytes_,
                                       next_grow_bytes(usage.capacity, needed,
                                                       growth_));
    chunks_[active_chunk_] = grow_page_block(
        chunks_[active_chunk_], active_offset_, wanted);
    const auto committed = page_block_alloc_bytes(wanted);
    committed_bytes_ += committed - usage.capacity;
    usage.capacity = committed;
  }

  // Rotate the append cursor onto a fresh active chunk. The full block freezes;
  // with huge pages enabled it is re-homed onto one huge page (eat the memcpy
  // once). The new active starts small and grows with demand -- only the tail
  // is allowed to grow/shrink.
  void rotate_active_chunk(size_type first_run_bytes) {
    if (active_chunk_ != kNoChunk) {
      auto& usage = chunk_usage_[active_chunk_];
      if (maybe_freeze_block_to_huge(chunks_[active_chunk_], active_offset_,
                                     chunk_bytes_, usage.capacity,
                                     committed_bytes_)) {
        usage.capacity = chunk_bytes_;
      }
    }
    make_active_chunk(first_run_bytes);
  }

  // Append `field` then `value` (either may be empty) as one contiguous run and
  // return the global offset of the first byte written. Used for fresh inserts
  // (both non-empty) and value-grow (field empty).
  [[nodiscard]] std::uint32_t append_bytes(std::string_view field,
                                           std::string_view value) {
    if (field.size() > kMaxFieldBytes) {
      throw std::length_error("hash field too large (max 64 KiB)");
    }
    if (value.size() > kMaxValueBytes) {
      throw std::length_error(
          "hash value too large (max 64 KiB; use a blob store for larger)");
    }
    const auto total = field.size() + value.size();
    if (total == 0) {
      // Empty field and empty value: no arena bytes; offset is unused for reads
      // of zero length, but must be a valid slot index into chunks_ on a later
      // in-place grow -- use the active cursor without advancing it.
      return active_chunk_ == kNoChunk
                 ? 0
                 : encode_offset(active_chunk_, active_offset_);
    }
    if (active_chunk_ == kNoChunk || active_offset_ + total > chunk_bytes_) {
      rotate_active_chunk(total);
    }
    ensure_active_capacity(active_offset_ + total);
    const auto offset = encode_offset(active_chunk_, active_offset_);
    auto* dst = chunks_[active_chunk_].get() + active_offset_;
    // Empty string_view::data() may be nullptr; memcpy of size 0 with a null
    // source is UB, so gate each half.
    if (!field.empty()) {
      std::memcpy(dst, field.data(), field.size());
    }
    if (!value.empty()) {
      std::memcpy(dst + field.size(), value.data(), value.size());
    }
    active_offset_ += total;
    chunk_usage_[active_chunk_].written += total;
    used_bytes_ += total;
    return offset;
  }

  void begin_compaction() noexcept {
    compaction_ = {};
    compaction_.phase = CompactionPhase::select;
    compaction_.selection_limit = chunk_usage_.size();
  }

  // Prefer holey *frozen* chunks (not the active tail). Fully-dead frozen chunks
  // win so we can free a huge page without densify. Active is only chosen when
  // it is the sole fragmented chunk (densify + shrink, no donor).
  void inspect_compaction_candidate(size_type candidate) noexcept {
    assert(candidate < compaction_.selection_limit);
    const auto chunk = static_cast<std::uint32_t>(candidate);
    const auto& usage = chunk_usage_[chunk];
    if (!chunks_[chunk] || usage.dead == 0) {
      return;
    }

    const bool fully_dead = usage.written == usage.dead;
    const bool have = compaction_.victim_chunk != kNoChunk;
    const bool best_fully_dead =
        have && compaction_.best_dead != 0 &&
        compaction_.best_dead == compaction_.best_written;
    const bool is_active = chunk == active_chunk_;
    const bool best_is_active =
        have && compaction_.victim_chunk == active_chunk_;

    if (fully_dead) {
      if (is_active && have && !best_is_active) {
        return;  // keep a freeable frozen chunk over an empty-able active
      }
      if (!have || !best_fully_dead || usage.dead > compaction_.best_dead ||
          (usage.dead == compaction_.best_dead && !is_active && best_is_active)) {
        compaction_.victim_chunk = chunk;
        compaction_.best_dead = usage.dead;
        compaction_.best_written = usage.written;
      }
      return;
    }
    if (best_fully_dead) {
      return;
    }
    if (is_active && have && !best_is_active) {
      return;  // prefer any holey frozen target over the tail
    }
    if (!have ||
        usage.dead * compaction_.best_written >
            compaction_.best_dead * usage.written ||
        (usage.dead * compaction_.best_written ==
             compaction_.best_dead * usage.written &&
         usage.dead > compaction_.best_dead) ||
        (best_is_active && !is_active)) {
      compaction_.victim_chunk = chunk;
      compaction_.best_dead = usage.dead;
      compaction_.best_written = usage.written;
    }
  }

  [[nodiscard]] bool finish_compaction_selection() {
    if (compaction_.victim_chunk == kNoChunk) {
      compaction_ = {};
      return false;
    }
    compaction_.phase = CompactionPhase::densify;
    compaction_.fields_total = size();
    compaction_.fields_scanned = 0;
    return true;
  }

  // Repack every live blob that lives on `chunk` to a dense prefix. Dead holes
  // disappear; free capacity becomes capacity - live at the end of the block.
  void densify_chunk(std::uint32_t chunk) {
    auto& usage = chunk_usage_[chunk];
    const size_type live = usage.written - usage.dead;
    if (usage.dead == 0) {
      if (chunk == active_chunk_) {
        active_offset_ = usage.written;
      }
      return;
    }
    if (live == 0) {
      // All bytes dead: clear accounting. Caller releases frozen chunks; active
      // keeps the slot and shrinks to the floor.
      used_bytes_ -= usage.written;
      dead_bytes_ -= usage.dead;
      usage.written = 0;
      usage.dead = 0;
      if (chunk == active_chunk_) {
        active_offset_ = 0;
      }
      return;
    }

    // Snapshot live field/value bytes that reside on this chunk, then rewrite.
    // Temp holds a packed image; refs are updated to the new dense layout.
    std::vector<char> packed;
    packed.reserve(live);
    struct RefUpdate {
      std::uint32_t field_id;
      size_type packed_off;
      size_type field_len;
      size_type value_len;
      bool both;  // field||value contiguous in packed
      bool field_only;
      bool value_only;
    };
    std::vector<RefUpdate> updates;
    updates.reserve(size());

    for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(size()); ++id) {
      const auto flen = static_cast<size_type>(field_lengths_[id]);
      const auto vlen = static_cast<size_type>(value_lengths_[id]);
      const auto f_off = field_offsets_[id];
      const auto v_off = value_offset(id);
      const bool field_here = offset_references_chunk(f_off, flen, chunk);
      const bool value_here = offset_references_chunk(v_off, vlen, chunk);
      if (!field_here && !value_here) {
        continue;
      }
      const size_type pack_at = packed.size();
      if (field_here && value_here && value_is_contiguous(id)) {
        const auto field = view(id);
        const auto val = value(id);
        packed.insert(packed.end(), field.begin(), field.end());
        packed.insert(packed.end(), val.begin(), val.end());
        updates.push_back({id, pack_at, flen, vlen, true, false, false});
      } else {
        if (field_here) {
          const auto field = view(id);
          const size_type at = packed.size();
          packed.insert(packed.end(), field.begin(), field.end());
          updates.push_back({id, at, flen, 0, false, true, false});
        }
        if (value_here) {
          const auto val = value(id);
          const size_type at = packed.size();
          packed.insert(packed.end(), val.begin(), val.end());
          updates.push_back({id, at, 0, vlen, false, false, true});
        }
      }
    }
    assert(packed.size() == live);

    // COW if the frozen block is shared (fork), then write the dense prefix.
    if (chunks_[chunk].use_count() > 1) {
      chunks_[chunk] =
          grow_page_block(chunks_[chunk], 0, usage.capacity);
    }
    if (live != 0) {
      std::memcpy(chunks_[chunk].get(), packed.data(), live);
    }

    for (const auto& u : updates) {
      if (u.both) {
        field_offsets_[u.field_id] =
            encode_offset(chunk, u.packed_off);
        clear_relocated_value_offset(u.field_id);
      } else if (u.field_only) {
        field_offsets_[u.field_id] =
            encode_offset(chunk, u.packed_off);
      } else if (u.value_only) {
        set_relocated_value_offset(u.field_id,
                                   encode_offset(chunk, u.packed_off));
      }
    }

    // Drop dead accounting for this chunk: used/dead both lose the hole bytes.
    used_bytes_ -= usage.dead;
    dead_bytes_ -= usage.dead;
    usage.written = live;
    usage.dead = 0;
    if (chunk == active_chunk_) {
      active_offset_ = live;
    }
  }

  // Write field||value at the end of a densified target's live prefix.
  [[nodiscard]] std::uint32_t append_into_chunk(std::uint32_t chunk,
                                                std::string_view field,
                                                std::string_view value) {
    const auto total = field.size() + value.size();
    auto& usage = chunk_usage_[chunk];
    assert(total == 0 || usage.written + total <= usage.capacity);
    if (total == 0) {
      return encode_offset(chunk, usage.written);
    }
    if (chunks_[chunk].use_count() > 1) {
      chunks_[chunk] =
          grow_page_block(chunks_[chunk], usage.written, usage.capacity);
    }
    const auto offset = encode_offset(chunk, usage.written);
    auto* dst = chunks_[chunk].get() + usage.written;
    if (!field.empty()) {
      std::memcpy(dst, field.data(), field.size());
    }
    if (!value.empty()) {
      std::memcpy(dst + field.size(), value.data(), value.size());
    }
    usage.written += total;
    used_bytes_ += total;
    return offset;
  }

  void collect_donor_items(std::vector<DonorItem>& out) const {
    out.clear();
    if (active_chunk_ == kNoChunk) {
      return;
    }
    const auto tail = active_chunk_;
    for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(size()); ++id) {
      const auto flen = static_cast<size_type>(field_lengths_[id]);
      const auto vlen = static_cast<size_type>(value_lengths_[id]);
      const bool field_on =
          offset_references_chunk(field_offsets_[id], flen, tail);
      const bool value_on =
          offset_references_chunk(value_offset(id), vlen, tail);
      if (!field_on && !value_on) {
        continue;
      }
      if (field_on && value_on && value_is_contiguous(id)) {
        out.push_back({id, flen + vlen, true, true, true});
      } else {
        // Prefer contiguous pairs; split pieces are separate knapsack items.
        if (field_on && flen != 0) {
          out.push_back({id, flen, true, false, false});
        }
        if (value_on && vlen != 0) {
          out.push_back({id, vlen, false, true, false});
        }
        if (field_on && flen == 0 && value_on && vlen == 0) {
          out.push_back({id, 0, true, true, true});
        }
      }
    }
  }

  // 0/1 knapsack / subset-sum: maximize total bytes ≤ capacity (the single
  // trailing free region after densify). Bitset DP with SIMD word-ORs when the
  // free window fits in memory; first-fit decreasing beyond that.
  void select_donor_subset(std::vector<DonorItem>& items, size_type capacity,
                           std::vector<std::size_t>& chosen) const {
    chosen.clear();
    if (items.empty() || capacity == 0) {
      return;
    }
    // Drop items that cannot fit alone.
    std::vector<DonorItem> fit;
    fit.reserve(items.size());
    for (const auto& item : items) {
      if (item.bytes > 0 && item.bytes <= capacity) {
        fit.push_back(item);
      }
    }
    if (fit.empty()) {
      return;
    }

    // Bitset of (capacity+1) bits ≈ capacity/8 bytes, plus a same-sized
    // workspace for `bits |= bits << w`, plus who[] for reconstruction.
    // Cap keeps the temporary tables reasonable on the compaction path.
    constexpr size_type kDpCapacityCap = size_type{256} << 10;
    if (capacity <= kDpCapacityCap && fit.size() <= 8192) {
      const std::size_t nbits = static_cast<std::size_t>(capacity) + 1;
      const std::size_t nwords = (nbits + 63) / 64;
      std::vector<std::uint64_t> bits(nwords, 0);
      std::vector<std::uint64_t> workspace(nwords, 0);
      // who[s] = fit index that first made sum s reachable; npos = unset.
      std::vector<std::uint32_t> who(nbits, std::numeric_limits<std::uint32_t>::max());
      bits[0] = 1;

      for (std::size_t i = 0; i < fit.size(); ++i) {
        const auto w = static_cast<std::size_t>(fit[i].bytes);
        // workspace = bits << w, then bits |= workspace (SIMD OR of words).
        simd::shl_u64_bitset(workspace.data(), bits.data(), nwords, w);
        // Record newly set sums before merging (for reconstruction).
        for (std::size_t wi = 0; wi < nwords; ++wi) {
          std::uint64_t newly = workspace[wi] & ~bits[wi];
          // Mask off bits above capacity in the last word.
          if (wi + 1 == nwords && (nbits % 64) != 0) {
            const std::uint64_t mask =
                (std::uint64_t{1} << (nbits % 64)) - 1u;
            newly &= mask;
          }
          while (newly != 0) {
            const auto bit = static_cast<std::size_t>(std::countr_zero(newly));
            newly &= newly - 1;
            const std::size_t s = wi * 64 + bit;
            if (s <= static_cast<std::size_t>(capacity) &&
                who[s] == std::numeric_limits<std::uint32_t>::max()) {
              who[s] = static_cast<std::uint32_t>(i);
            }
          }
        }
        simd::or_u64_words(bits.data(), workspace.data(), nwords);
        // Clear any bits above capacity that shift may have set in the last word.
        if ((nbits % 64) != 0) {
          const std::uint64_t mask =
              (std::uint64_t{1} << (nbits % 64)) - 1u;
          bits[nwords - 1] &= mask;
        }
      }

      const std::size_t best =
          simd::bitset_msb_u64(bits.data(), static_cast<std::size_t>(capacity));
      if (best == static_cast<std::size_t>(-1) || best == 0) {
        items.clear();
        return;
      }

      std::vector<DonorItem> picked;
      picked.reserve(fit.size());
      std::size_t s = best;
      while (s > 0) {
        const auto i = who[s];
        assert(i != std::numeric_limits<std::uint32_t>::max());
        assert(i < fit.size());
        assert(fit[i].bytes <= s);
        picked.push_back(fit[i]);
        s -= fit[i].bytes;
      }
      items.swap(picked);
      chosen.clear();
      for (std::size_t i = 0; i < items.size(); ++i) {
        chosen.push_back(i);
      }
      return;
    }

    // First-fit decreasing (free window larger than the DP table budget).
    std::sort(fit.begin(), fit.end(),
              [](const DonorItem& a, const DonorItem& b) {
                return a.bytes > b.bytes;
              });
    size_type left = capacity;
    items.clear();
    for (const auto& item : fit) {
      if (item.bytes <= left) {
        items.push_back(item);
        chosen.push_back(items.size() - 1);
        left -= item.bytes;
      }
    }
  }

  // Move selected tail items into the densified target. Returns bytes moved.
  [[nodiscard]] size_type donate_from_tail(std::uint32_t target,
                                           size_type free_space,
                                           size_type byte_budget,
                                           size_type work_budget,
                                           CompactionStepResult& result) {
    if (free_space == 0 || byte_budget == 0 || work_budget == 0) {
      return 0;
    }
    std::vector<DonorItem> items;
    collect_donor_items(items);
    result.work_done += 1;  // collection pass

    std::vector<std::size_t> chosen;
    select_donor_subset(items, std::min(free_space, byte_budget), chosen);

    size_type moved = 0;
    for (const auto idx : chosen) {
      if (result.work_done >= work_budget && moved != 0) {
        break;
      }
      const auto& item = items[idx];
      if (moved + item.bytes > byte_budget && moved != 0) {
        break;
      }
      // Re-validate the item still lives on the active tail (mutations).
      if (active_chunk_ == kNoChunk) {
        break;
      }
      const auto flen = static_cast<size_type>(field_lengths_[item.field_id]);
      const auto vlen = static_cast<size_type>(value_lengths_[item.field_id]);
      const auto f_off = field_offsets_[item.field_id];
      const auto v_off = value_offset(item.field_id);
      const bool field_on =
          offset_references_chunk(f_off, flen, active_chunk_);
      const bool value_on =
          offset_references_chunk(v_off, vlen, active_chunk_);
      if (item.as_contiguous_pair) {
        if (!(field_on && value_on && value_is_contiguous(item.field_id))) {
          ++result.work_done;
          continue;
        }
        if (chunk_usage_[target].written + flen + vlen >
            chunk_usage_[target].capacity) {
          break;
        }
        const auto field = view(item.field_id);
        const auto val = value(item.field_id);
        const auto new_off = append_into_chunk(target, field, val);
        mark_dead(f_off, flen);
        mark_dead(v_off, vlen);
        field_offsets_[item.field_id] = new_off;
        clear_relocated_value_offset(item.field_id);
        moved += flen + vlen;
        ++compaction_.relocated_fields;
        compaction_.relocated_bytes += flen + vlen;
      } else if (item.move_field && !item.move_value) {
        if (!field_on) {
          ++result.work_done;
          continue;
        }
        if (chunk_usage_[target].written + flen >
            chunk_usage_[target].capacity) {
          break;
        }
        const auto field = view(item.field_id);
        const auto new_off = append_into_chunk(target, field, {});
        mark_dead(f_off, flen);
        field_offsets_[item.field_id] = new_off;
        moved += flen;
        ++compaction_.relocated_fields;
        compaction_.relocated_bytes += flen;
      } else if (item.move_value && !item.move_field) {
        if (!value_on) {
          ++result.work_done;
          continue;
        }
        if (chunk_usage_[target].written + vlen >
            chunk_usage_[target].capacity) {
          break;
        }
        const auto val = value(item.field_id);
        const auto new_off = append_into_chunk(target, {}, val);
        mark_dead(v_off, vlen);
        set_relocated_value_offset(item.field_id, new_off);
        moved += vlen;
        ++compaction_.relocated_fields;
        compaction_.relocated_bytes += vlen;
      }
      result.bytes_moved += item.bytes;
      ++result.work_done;
      ++compaction_.fields_scanned;
    }
    return moved;
  }

  // Densify the active tail and shrink its physical allocation toward live size.
  [[nodiscard]] size_type shrink_active_after_densify() {
    if (active_chunk_ == kNoChunk) {
      return 0;
    }
    densify_chunk(active_chunk_);
    auto& usage = chunk_usage_[active_chunk_];
    const size_type live = usage.written;  // dead cleared by densify
    active_offset_ = live;
    const size_type want =
        live == 0 ? kInitialArenaBytes
                  : std::min(chunk_bytes_,
                             std::max(kInitialArenaBytes,
                                      page_block_alloc_bytes(live)));
    // Never grow here; only shrink (or COW to unique) when capacity is larger.
    if (want >= usage.capacity && chunks_[active_chunk_].use_count() == 1) {
      return 0;
    }
    const size_type target_cap = std::min(usage.capacity, want);
    if (target_cap == usage.capacity &&
        chunks_[active_chunk_].use_count() == 1) {
      return 0;
    }
    auto fresh = grow_page_block(chunks_[active_chunk_], live, target_cap);
    const size_type new_cap = page_block_alloc_bytes(target_cap);
    const size_type reclaimed =
        usage.capacity > new_cap ? usage.capacity - new_cap : 0;
    committed_bytes_ -= usage.capacity;
    committed_bytes_ += new_cap;
    usage.capacity = new_cap;
    chunks_[active_chunk_] = std::move(fresh);
    return reclaimed;
  }

  [[nodiscard]] size_type release_chunk(std::uint32_t chunk) {
    assert(chunk != active_chunk_ && chunk < chunks_.size());
    auto& usage = chunk_usage_[chunk];
    assert(usage.written == usage.dead);
    const auto reclaimed = usage.capacity;
    // Grow the recycle list before changing accounting or releasing memory, so
    // allocation failure leaves the chunk and every reference intact.
    free_chunks_.push_back(chunk);
    committed_bytes_ -= usage.capacity;
    used_bytes_ -= usage.written;
    dead_bytes_ -= usage.dead;
    chunks_[chunk].reset();
    usage = {};
    return reclaimed;
  }

  [[nodiscard]] size_type grow_ref_capacity(size_type capacity) const noexcept {
    const auto scaled =
        static_cast<size_type>(static_cast<double>(capacity) * growth_);
    return scaled > capacity ? scaled : capacity + 1;
  }

  std::vector<std::shared_ptr<char[]>> chunks_;
  std::vector<ChunkUsage> chunk_usage_;
  std::vector<std::uint32_t> free_chunks_;
  std::vector<std::uint32_t> field_offsets_;
  std::vector<RelocationBlockPtr> relocation_blocks_;
  size_type relocation_block_count_{0};
  std::vector<std::uint16_t> field_lengths_;
  std::vector<std::uint16_t> value_lengths_;
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type committed_bytes_{0};  // sum of block physical sizes
  std::uint32_t active_chunk_{kNoChunk};
  size_type active_offset_{0};
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{20};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  double growth_{kDefaultArenaGrowth};
  CompactionState compaction_;
};

}  // namespace goblin::core
