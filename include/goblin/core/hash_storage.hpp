#pragma once

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
// field orphans field+value. Orphaned (dead) bytes are tracked per chunk. Bounded
// compaction evacuates one chunk over multiple maintenance steps, then releases
// and recycles its logical chunk number. It scans the dense field ids directly,
// so the steady-state field reference remains eight bytes and compaction needs
// no per-field reverse map.
class HashStorage {
 public:
  using size_type = std::size_t;

  enum class CompactionPhase : std::uint8_t {
    idle,
    select,
    scan,
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
  static constexpr size_type kDefaultChunkBytes = size_type{1} << 20;  // 1 MiB
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
    } else if (compaction_.phase == CompactionPhase::scan) {
      result.fields_total = compaction_.scan_limit;
      result.fields_scanned = compaction_.scan_cursor;
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
    // sizes and arena accounting are still unchanged.
    const auto required = field_offsets_.size() + 1;
    if (field_offsets_.capacity() < required ||
        field_lengths_.capacity() < required ||
        value_lengths_.capacity() < required ||
        relocation_blocks_.capacity() <
            relocation_directory_entries(required)) {
      reserve_additional(1);
    }
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
    if (value.size() <= old_value_len) {
      if (!value.empty()) {
        std::memcpy(chunk_ptr(old_value_offset), value.data(), value.size());
      }
      mark_dead(old_value_offset, old_value_len - value.size());
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
    if (compaction_active() && dst < compaction_.scan_cursor &&
        references_chunk(dst, compaction_.victim_chunk)) {
      // A swap-remove can move a victim reference behind an incremental scan's
      // cursor. The bounded scan repeats when this flag is observed.
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

  // Perform bounded maintenance on one fragmented arena chunk. `work_budget`
  // counts chunk candidates during victim selection and field references during
  // evacuation; `byte_budget` bounds copied bytes, except that one field/value
  // pair may exceed it (an individual value is atomic and capped at 64 KiB).
  // Calls may be interleaved with normal mutations. A swap-remove behind the
  // cursor requests another bounded pass; per-chunk accounting prevents release
  // while any live byte remains.
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
      assert(compaction_.phase == CompactionPhase::scan);
      if (compaction_.scan_cursor < compaction_.scan_limit) {
        const auto id = static_cast<std::uint32_t>(compaction_.scan_cursor);
        const auto bytes = relocation_bytes(id, compaction_.victim_chunk);
        if (bytes != 0 && result.bytes_moved != 0 &&
            bytes > byte_budget - std::min(byte_budget, result.bytes_moved)) {
          break;
        }
        ++compaction_.scan_cursor;
        const auto moved = relocate_from_chunk(id, compaction_.victim_chunk);
        result.bytes_moved += moved;
        ++result.work_done;
        if (moved != 0) {
          ++compaction_.relocated_fields;
          compaction_.relocated_bytes += moved;
        }
        continue;
      }
      if (compaction_.rescan_required) {
        compaction_.rescan_required = false;
        compaction_.scan_cursor = 0;
        compaction_.scan_limit = size();
        continue;
      }

      const auto victim = compaction_.victim_chunk;
      auto& usage = chunk_usage_[victim];
      if (usage.written != usage.dead) {
        // Accounting says a live byte remains. Retain the chunk and scan again
        // instead of risking a dangling global offset.
        compaction_.scan_cursor = 0;
        compaction_.scan_limit = size();
        continue;
      }
      result.bytes_reclaimed = release_chunk(victim);
      compaction_ = {};
      result.completed = true;
    }
    result.progress = compaction_progress();
    return result;
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
    size_type scan_cursor{0};
    size_type scan_limit{0};
    size_type relocated_fields{0};
    size_type relocated_bytes{0};
    bool rescan_required{false};
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

  void rotate_active_chunk(size_type first_run_bytes) {
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

  void inspect_compaction_candidate(size_type candidate) noexcept {
    assert(candidate < compaction_.selection_limit);
    const auto chunk = static_cast<std::uint32_t>(candidate);
    const auto& usage = chunk_usage_[chunk];
    if (chunks_[chunk] && usage.dead != 0 &&
        (compaction_.victim_chunk == kNoChunk ||
         usage.dead * compaction_.best_written >
             compaction_.best_dead * usage.written ||
         (usage.dead * compaction_.best_written ==
              compaction_.best_dead * usage.written &&
          usage.dead > compaction_.best_dead))) {
      compaction_.victim_chunk = chunk;
      compaction_.best_dead = usage.dead;
      compaction_.best_written = usage.written;
    }
  }

  [[nodiscard]] bool finish_compaction_selection() {
    const auto victim = compaction_.victim_chunk;
    if (victim == kNoChunk) {
      compaction_ = {};
      return false;
    }
    if (victim == active_chunk_) {
      // Evacuation must never append back into its victim.
      rotate_active_chunk(0);
    }
    compaction_.phase = CompactionPhase::scan;
    compaction_.victim_chunk = victim;
    compaction_.scan_limit = size();
    return true;
  }

  [[nodiscard]] size_type relocation_bytes(std::uint32_t field_id,
                                            std::uint32_t victim) const noexcept {
    if (field_id >= size()) {
      return 0;
    }
    size_type bytes = 0;
    if (offset_references_chunk(field_offsets_[field_id],
                                field_lengths_[field_id], victim)) {
      bytes += field_lengths_[field_id];
    }
    if (offset_references_chunk(value_offset(field_id),
                                value_lengths_[field_id], victim)) {
      bytes += value_lengths_[field_id];
    }
    return bytes;
  }

  [[nodiscard]] size_type relocate_from_chunk(std::uint32_t field_id,
                                               std::uint32_t victim) {
    if (field_id >= size()) {
      return 0;
    }
    const auto old_field_offset = field_offsets_[field_id];
    const auto old_value_offset = value_offset(field_id);
    const auto field_len = static_cast<size_type>(field_lengths_[field_id]);
    const auto value_len = static_cast<size_type>(value_lengths_[field_id]);
    const bool move_field =
        offset_references_chunk(old_field_offset, field_len, victim);
    const bool move_value =
        offset_references_chunk(old_value_offset, value_len, victim);
    if (!move_field && !move_value) {
      return 0;
    }

    const auto field = view(field_id);
    const auto old_value = value(field_id);
    // A contiguous value moves with its field even when that field is empty.
    // Updating the (otherwise unused) empty-field offset keeps the pair
    // contiguous and lets an empty relocation block be reclaimed.
    if (move_value && value_is_contiguous(field_id)) {
      field_offsets_[field_id] = append_bytes(field, old_value);
      clear_relocated_value_offset(field_id);
    } else {
      auto new_field_offset = kContiguousValueOffset;
      auto new_value_offset = kContiguousValueOffset;
      if (move_field) {
        new_field_offset = append_bytes(field, {});
      }
      try {
        if (move_value) {
          assert(!value_is_contiguous(field_id));
          new_value_offset = append_bytes({}, old_value);
        }
      } catch (...) {
        // The first copy is not published yet. Account it as dead so arena
        // invariants remain exact while the original references stay live.
        if (new_field_offset != kContiguousValueOffset) {
          mark_dead(new_field_offset, field_len);
        }
        throw;
      }
      if (new_field_offset != kContiguousValueOffset) {
        field_offsets_[field_id] = new_field_offset;
      }
      if (new_value_offset != kContiguousValueOffset) {
        set_relocated_value_offset(field_id, new_value_offset);
      }
    }
    if (move_field) {
      mark_dead(old_field_offset, field_len);
    }
    if (move_value) {
      mark_dead(old_value_offset, value_len);
    }
    return (move_field ? field_len : 0) + (move_value ? value_len : 0);
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
