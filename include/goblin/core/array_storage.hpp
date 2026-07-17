#pragma once

// Packed element-byte storage for sparse arrays (AR*). Values use the shared
// string encoder and live in a page-aligned arena (HugeTLB via page_arena).
// Leaves hold value ids only; compact() is id-stable so slice tables need no
// rewrite. kEmptyId (0xFFFFFFFF) is Redis-style "absent" and is never allocated.

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "goblin/core/page_arena.hpp"
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

class ArrayStorage {
 public:
  using size_type = std::size_t;
  using value_id = std::uint32_t;

  // Redis-style absent / dense hole. Never a live storage id.
  static constexpr value_id kEmptyId = std::numeric_limits<value_id>::max();
  static constexpr std::uint32_t kFreeOffset =
      std::numeric_limits<std::uint32_t>::max();

  static constexpr size_type kDefaultChunkBytes = size_type{1} << 21;
  static constexpr size_type kMinChunkBytes = size_type{1} << 16;
  static constexpr size_type kMaxEncodedBytes = kStringMaxEncodedBytes;

  explicit ArrayStorage(size_type chunk_bytes = kDefaultChunkBytes,
                        double growth = kDefaultArenaGrowth,
                        StringEncodingOptions string_encoding = {})
      : growth_(growth > 1.0 ? growth : kDefaultArenaGrowth),
        string_encoding_(string_encoding) {
    configure_chunk_bytes(chunk_bytes);
  }

  [[nodiscard]] size_type chunk_bytes() const noexcept { return chunk_bytes_; }
  [[nodiscard]] StringEncodingOptions string_encoding() const noexcept {
    return string_encoding_;
  }
  [[nodiscard]] bool empty() const noexcept { return live_count_ == 0; }
  [[nodiscard]] size_type size() const noexcept { return offsets_.size(); }
  [[nodiscard]] size_type live_count() const noexcept { return live_count_; }
  [[nodiscard]] size_type free_count() const noexcept {
    return free_ids_.size();
  }
  [[nodiscard]] size_type used_bytes() const noexcept { return used_bytes_; }
  [[nodiscard]] size_type dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }
  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return committed_bytes_ + offsets_.capacity() * sizeof(std::uint32_t) +
           lengths_.capacity() * sizeof(std::uint16_t) +
           free_ids_.capacity() * sizeof(value_id) +
           chunks_.capacity() * sizeof(chunks_[0]);
  }
  [[nodiscard]] bool realtime_reserved() const noexcept {
    return realtime_reserved_;
  }
  [[nodiscard]] size_type reserved_value_capacity() const noexcept {
    return reserved_value_capacity_;
  }
  [[nodiscard]] size_type reserved_arena_bytes() const noexcept {
    return reserved_arena_bytes_;
  }

  // Prepare an empty RT value store outside the serving path. The metadata
  // vectors are resized once to fault their pages, then cleared while retaining
  // capacity. Arena chunks are allocated and touched in full. Once reserved,
  // append exhaustion is an error rather than a fallback to mmap/reallocation.
  void reserve_realtime(size_type value_capacity, size_type arena_bytes) {
    if (realtime_reserved_ || !offsets_.empty() || used_bytes_ != 0 ||
        live_count_ != 0) {
      throw std::length_error(
          "ERR RT array reserve requires an empty, unreserved array");
    }
    if (value_capacity == 0 ||
        value_capacity >= static_cast<size_type>(kEmptyId)) {
      throw std::length_error("ERR RT array value reservation is out of range");
    }
    if (arena_bytes == 0 ||
        arena_bytes > static_cast<size_type>(
                          std::numeric_limits<std::uint32_t>::max())) {
      throw std::length_error("ERR RT array byte reservation is out of range");
    }

    std::vector<std::uint32_t> offsets;
    std::vector<std::uint16_t> lengths;
    std::vector<value_id> free_ids;
    offsets.resize(value_capacity, kFreeOffset);
    lengths.resize(value_capacity, 0);
    free_ids.resize(value_capacity, kEmptyId);
    offsets.clear();
    lengths.clear();
    free_ids.clear();

    const auto chunk_count =
        (arena_bytes + chunk_bytes_ - 1) / chunk_bytes_;
    std::vector<std::shared_ptr<char[]>> chunks;
    chunks.reserve(chunk_count);
    for (size_type i = 0; i < chunk_count; ++i) {
      auto block = alloc_page_block(chunk_bytes_);
      std::memset(block.get(), 0, chunk_bytes_);
      chunks.push_back(std::move(block));
    }

    offsets_ = std::move(offsets);
    lengths_ = std::move(lengths);
    free_ids_ = std::move(free_ids);
    chunks_ = std::move(chunks);
    active_bytes_ = chunk_bytes_;
    committed_bytes_ = chunk_count * page_block_alloc_bytes(chunk_bytes_);
    reserved_value_capacity_ = value_capacity;
    reserved_arena_bytes_ = arena_bytes;
    realtime_reserved_ = true;
  }

  [[nodiscard]] bool is_live(value_id id) const noexcept {
    return id != kEmptyId && id < offsets_.size() &&
           offsets_[id] != kFreeOffset;
  }

  [[nodiscard]] value_id push(std::string_view value) {
    const EncodedString encoded(value, string_encoding_);
    return push_encoded(encoded);
  }

  [[nodiscard]] value_id push_encoded(const EncodedString& encoded) {
    if (encoded.size() > kMaxEncodedBytes) {
      throw std::length_error("array value too large");
    }
    if (realtime_reserved_ && free_ids_.empty() &&
        offsets_.size() >= reserved_value_capacity_) {
      throw std::length_error("ERR RT array reserved value capacity exhausted");
    }
    const auto offset = append_encoded(encoded);
    const auto length = static_cast<std::uint16_t>(encoded.size());
    if (!free_ids_.empty()) {
      const auto id = free_ids_.back();
      free_ids_.pop_back();
      assert(offsets_[id] == kFreeOffset);
      offsets_[id] = offset;
      lengths_[id] = length;
      ++live_count_;
      return id;
    }
    // Reserve kEmptyId as sentinel — never allocate it.
    if (offsets_.size() >= static_cast<size_type>(kEmptyId)) {
      throw std::length_error("array value id space exhausted");
    }
    const auto id = static_cast<value_id>(offsets_.size());
    offsets_.push_back(offset);
    lengths_.push_back(length);
    ++live_count_;
    return id;
  }

  // Replace one live value without allocating another value id. The new bytes
  // are committed before the old location is retired, so reservation failures
  // leave both the value-id table and its leaf reference unchanged.
  void replace(value_id id, std::string_view value) {
    const EncodedString encoded(value, string_encoding_);
    replace_encoded(id, encoded);
  }

  void replace_encoded(value_id id, const EncodedString& encoded) {
    assert(is_live(id));
    if (encoded.size() > kMaxEncodedBytes) {
      throw std::length_error("array value too large");
    }
    const auto new_offset = append_encoded(encoded);
    const auto old_length = lengths_[id];
    offsets_[id] = new_offset;
    lengths_[id] = static_cast<std::uint16_t>(encoded.size());
    dead_bytes_ += old_length;
    assert(dead_bytes_ <= used_bytes_);
  }

  [[nodiscard]] std::string_view view(value_id id) const noexcept {
    assert(is_live(id));
    const auto length = lengths_[id];
    if (length == 0) {
      return {};
    }
    const auto offset = offsets_[id];
    const char* data =
        chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_);
    return std::string_view(data, length);
  }

  [[nodiscard]] EncodedStringView encoded_view(value_id id) const noexcept {
    return EncodedStringView(view(id), string_encoding_.encoding_enabled());
  }

  [[nodiscard]] std::string to_string(value_id id) const {
    assert(is_live(id));
    return encoded_view(id).to_string();
  }

  void orphan(value_id id) noexcept {
    if (id == kEmptyId) {
      return;
    }
    assert(is_live(id));
    dead_bytes_ += lengths_[id];
    assert(dead_bytes_ <= used_bytes_);
    offsets_[id] = kFreeOffset;
    lengths_[id] = 0;
    free_ids_.push_back(id);
    assert(live_count_ > 0);
    --live_count_;
  }

  // Id-stable reclaim: leaf-held ids stay valid.
  void compact() {
    if (dead_bytes_ == 0 || realtime_reserved_) {
      return;
    }
    std::vector<std::shared_ptr<char[]>> fresh_chunks;
    size_type fresh_next = 0;
    size_type fresh_active = 0;
    size_type fresh_committed = 0;
    size_type fresh_used = 0;

    const auto count = offsets_.size();
    for (size_type raw = 0; raw < count; ++raw) {
      const auto id = static_cast<value_id>(raw);
      if (!is_live(id)) {
        offsets_[id] = kFreeOffset;
        lengths_[id] = 0;
        continue;
      }
      const auto member = view(id);
      const auto r = reserve_run_bytes(
          fresh_chunks, fresh_next, fresh_active, fresh_committed, chunk_bytes_,
          chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, member.size());
      if (r.dst != nullptr && !member.empty()) {
        std::memcpy(r.dst, member.data(), member.size());
        fresh_used += member.size();
      }
      offsets_[id] = r.offset;
    }

    chunks_ = std::move(fresh_chunks);
    next_offset_ = fresh_next;
    active_bytes_ = fresh_active;
    committed_bytes_ = fresh_committed;
    used_bytes_ = fresh_used;
    dead_bytes_ = 0;
    shrink_to_fit_tail();
  }

  [[nodiscard]] bool should_compact() const noexcept {
    return !realtime_reserved_ && dead_bytes_ >= kCompactFloorBytes &&
           dead_bytes_ >= live_bytes();
  }

 private:
  static constexpr size_type kCompactFloorBytes = 4096;

  void configure_chunk_bytes(size_type chunk_bytes) {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes ||
        chunk_bytes > (size_type{1} << 31)) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
  }

  void shrink_to_fit_tail() {
    if (chunks_.empty()) {
      return;
    }
    auto used = next_offset_ & chunk_mask_;
    if (used == 0 && next_offset_ != 0) {
      used = chunk_bytes_;
    }
    const auto desired = page_block_alloc_bytes(used);
    if (desired >= active_bytes_) {
      return;
    }
    chunks_.back() = grow_page_block(chunks_.back(), used, used);
    committed_bytes_ -= active_bytes_ - desired;
    active_bytes_ = desired;
  }

  [[nodiscard]] std::uint32_t append_encoded(const EncodedString& encoded) {
    const auto n = encoded.size();
    if (realtime_reserved_) {
      auto offset = next_offset_ & chunk_mask_;
      if (offset + n > chunk_bytes_) {
        next_offset_ += chunk_bytes_ - offset;
        offset = 0;
      }
      if (next_offset_ > reserved_arena_bytes_ ||
          n > reserved_arena_bytes_ - next_offset_) {
        throw std::length_error("ERR RT array reserved byte arena exhausted");
      }
      const auto block_index = next_offset_ >> chunk_shift_;
      assert(block_index < chunks_.size());
      const auto result = static_cast<std::uint32_t>(next_offset_);
      if (n != 0) {
        encoded.write_to(chunks_[block_index].get() + offset);
        used_bytes_ += n;
      }
      next_offset_ += n;
      return result;
    }
    const auto r = reserve_run_bytes(
        chunks_, next_offset_, active_bytes_, committed_bytes_, chunk_bytes_,
        chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, n);
    if (r.dst != nullptr && n != 0) {
      encoded.write_to(r.dst);
      used_bytes_ += n;
    }
    return r.offset;
  }

  std::vector<std::uint32_t> offsets_;
  std::vector<std::uint16_t> lengths_;
  std::vector<value_id> free_ids_;
  std::vector<std::shared_ptr<char[]>> chunks_;
  size_type next_offset_{0};
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type active_bytes_{0};
  size_type committed_bytes_{0};
  size_type live_count_{0};
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{21};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  double growth_{kDefaultArenaGrowth};
  StringEncodingOptions string_encoding_{};
  size_type reserved_value_capacity_{0};
  size_type reserved_arena_bytes_{0};
  bool realtime_reserved_{false};
};

}  // namespace goblin::core
