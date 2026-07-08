#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "goblin/core/page_arena.hpp"

namespace goblin::core {

// Packed field+value storage for a hash. Each entry stores its field bytes
// immediately followed by its value bytes as one contiguous blob in a chunked
// arena, addressed by a single struct-of-arrays reference:
//
//     offset (u32) + field_len (u16) + value_len (u16)  = 8 bytes / field
//
// (Leaner than the zset's 14-byte member reference: one offset locates both the
// field and the value, no f64 score, no score-text cache.) The field bytes back
// the field->id swiss index (view(id)); the value bytes back HGET.
//
// Both field and value are capped at 64 KiB - 1 (the u16 length, same as zset
// members). Larger values belong in a blob store (goblin-store.dev) with the
// hash holding the key.
//
// The arena chunk size is configurable (--hash-chunk-bytes): a power of two at
// least kMinChunkBytes (large enough to hold the biggest field+value blob).
// Smaller chunks lower the floor for hashes of big blobs; larger chunks reduce
// boundary skips. A blob never straddles a chunk (so value() is contiguous).
//
// Fragmentation: a same-or-smaller value update overwrites in place (no arena
// growth); a larger value re-appends the blob and orphans the old bytes; a
// removed field orphans its blob. Orphaned (dead) bytes are tracked so the Hash
// can auto-compact once they exceed the live bytes.
class HashStorage {
 public:
  using size_type = std::size_t;

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
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
    growth_ = growth > 1.0 ? growth : kDefaultArenaGrowth;
  }

  [[nodiscard]] size_type chunk_bytes() const noexcept { return chunk_bytes_; }
  [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
  [[nodiscard]] size_type size() const noexcept { return offsets_.size(); }
  [[nodiscard]] size_type byte_size() const noexcept { return used_bytes_; }
  [[nodiscard]] size_type dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }
  [[nodiscard]] size_type byte_capacity() const noexcept {
    return committed_bytes_;
  }
  [[nodiscard]] size_type ref_capacity() const noexcept {
    return offsets_.capacity();
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return byte_capacity() + offsets_.capacity() * sizeof(std::uint32_t) +
           field_lengths_.capacity() * sizeof(std::uint16_t) +
           value_lengths_.capacity() * sizeof(std::uint16_t) +
           chunks_.capacity() * sizeof(std::shared_ptr<char[]>);
  }

  void reserve(size_type field_count) {
    offsets_.reserve(field_count);
    field_lengths_.reserve(field_count);
    value_lengths_.reserve(field_count);
  }

  void reserve_bytes(size_type byte_count) {
    chunks_.reserve((byte_count + chunk_bytes_ - 1) / chunk_bytes_);
  }

  // Append a new field with its value; returns its field id (0-based, dense).
  [[nodiscard]] std::uint32_t push_back(std::string_view field,
                                        std::string_view value) {
    if (offsets_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("hash field id space exhausted");
    }
    const auto offset = append_blob(field, value);
    offsets_.push_back(offset);
    field_lengths_.push_back(static_cast<std::uint16_t>(field.size()));
    value_lengths_.push_back(static_cast<std::uint16_t>(value.size()));
    return static_cast<std::uint32_t>(offsets_.size() - 1);
  }

  // Replace an existing field's value (the field bytes are unchanged). Fits in
  // place when the new value is no longer than the old one -- no arena growth;
  // otherwise the blob is re-appended and the old blob orphaned.
  void set_value(std::uint32_t field_id, std::string_view value) {
    assert(field_id < offsets_.size());
    if (value.size() > kMaxValueBytes) {
      throw std::length_error(
          "hash value too large (max 64 KiB; use a blob store for larger)");
    }
    const size_type old_value_len = value_lengths_[field_id];
    if (value.size() <= old_value_len) {
      const auto value_offset = offsets_[field_id] + field_lengths_[field_id];
      if (!value.empty()) {
        std::memcpy(chunk_ptr(value_offset), value.data(), value.size());
      }
      dead_bytes_ += old_value_len - value.size();
      value_lengths_[field_id] = static_cast<std::uint16_t>(value.size());
      return;
    }
    const size_type old_blob =
        static_cast<size_type>(field_lengths_[field_id]) + old_value_len;
    const auto field = view(field_id);
    const auto offset = append_blob(field, value);
    offsets_[field_id] = offset;
    value_lengths_[field_id] = static_cast<std::uint16_t>(value.size());
    dead_bytes_ += old_blob;
  }

  // The field bytes (also what the swiss index compares against).
  [[nodiscard]] std::string_view view(std::uint32_t field_id) const noexcept {
    assert(field_id < offsets_.size());
    const auto length = field_lengths_[field_id];
    return length == 0 ? std::string_view{}
                       : std::string_view(chunk_ptr(offsets_[field_id]), length);
  }

  // The value bytes (stored immediately after the field bytes).
  [[nodiscard]] std::string_view value(std::uint32_t field_id) const noexcept {
    assert(field_id < offsets_.size());
    const auto value_len = value_lengths_[field_id];
    return value_len == 0
               ? std::string_view{}
               : std::string_view(
                     chunk_ptr(offsets_[field_id] + field_lengths_[field_id]),
                     value_len);
  }

  // Account a to-be-removed field's blob as dead (the Hash swap-removes it out
  // of the index; its bytes stay in the arena until compact).
  void orphan(std::uint32_t field_id) noexcept {
    assert(field_id < offsets_.size());
    dead_bytes_ += static_cast<size_type>(field_lengths_[field_id]) +
                   value_lengths_[field_id];
  }

  // Copy a field's reference over another's (swap-remove moves the last field
  // into a removed slot).
  void copy_ref(std::uint32_t dst, std::uint32_t src) noexcept {
    assert(dst < offsets_.size() && src < offsets_.size());
    offsets_[dst] = offsets_[src];
    field_lengths_[dst] = field_lengths_[src];
    value_lengths_[dst] = value_lengths_[src];
  }

  void pop_back() noexcept {
    assert(!offsets_.empty());
    offsets_.pop_back();
    field_lengths_.pop_back();
    value_lengths_.pop_back();
  }

 private:
  [[nodiscard]] const char* chunk_ptr(std::uint32_t offset) const noexcept {
    return chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_);
  }
  [[nodiscard]] char* chunk_ptr(std::uint32_t offset) noexcept {
    return chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_);
  }

  // Append (field || value) contiguously and return the blob's global offset.
  [[nodiscard]] std::uint32_t append_blob(std::string_view field,
                                          std::string_view value) {
    if (field.size() > kMaxFieldBytes) {
      throw std::length_error("hash field too large (max 64 KiB)");
    }
    if (value.size() > kMaxValueBytes) {
      throw std::length_error(
          "hash value too large (max 64 KiB; use a blob store for larger)");
    }
    const auto total = field.size() + value.size();
    if (next_offset_ >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            total - (chunk_bytes_ - 1)) {
      throw std::length_error("hash arena exhausted");
    }
    // Reserve the blob's bytes in the growable, page-aligned arena (each block
    // owns a chunk_bytes logical slot, so chunk_ptr's shift/mask addressing is
    // unchanged; the active block grows to fit and a blob never straddles).
    const auto r = reserve_run_bytes(chunks_, next_offset_, active_bytes_,
                                     committed_bytes_, chunk_bytes_, chunk_shift_,
                                     chunk_mask_, growth_, kInitialArenaBytes,
                                     total);
    if (r.dst != nullptr) {
      if (!field.empty()) {
        std::memcpy(r.dst, field.data(), field.size());
      }
      if (!value.empty()) {
        std::memcpy(r.dst + field.size(), value.data(), value.size());
      }
      used_bytes_ += total;
    }
    return r.offset;
  }

  std::vector<std::shared_ptr<char[]>> chunks_;
  std::vector<std::uint32_t> offsets_;
  std::vector<std::uint16_t> field_lengths_;
  std::vector<std::uint16_t> value_lengths_;
  size_type next_offset_{0};
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type active_bytes_{0};     // physical size of the last block
  size_type committed_bytes_{0};  // sum of block physical sizes
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{20};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  double growth_{kDefaultArenaGrowth};
};

}  // namespace goblin::core
