#pragma once

// Packed member-byte storage for Redis sets. Members are classified with the
// shared string encoder (compact integers, UUIDs, optional LZ4, raw) before they
// are appended, so the Swiss member index probes encoded bytes. Layout matches
// the other arena types: SoA offsets/lengths plus a page-aligned byte arena
// grown at the same 2^0.25 rate as the zset/hash member indexes.

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
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

class SetStorage {
 public:
  using size_type = std::size_t;

  static constexpr size_type kDefaultChunkBytes = size_type{1} << 21;  // 2 MiB
  // Encoded members are u16-length, so a chunk must hold the largest blob.
  static constexpr size_type kMinChunkBytes = size_type{1} << 16;
  static constexpr size_type kMaxEncodedBytes = kStringMaxEncodedBytes;

  explicit SetStorage(size_type chunk_bytes = kDefaultChunkBytes,
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
  [[nodiscard]] bool empty() const noexcept { return offsets_.empty(); }
  [[nodiscard]] size_type size() const noexcept { return offsets_.size(); }
  [[nodiscard]] size_type used_bytes() const noexcept { return used_bytes_; }
  [[nodiscard]] size_type dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }
  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return committed_bytes_ + offsets_.capacity() * sizeof(std::uint32_t) +
           lengths_.capacity() * sizeof(std::uint16_t) +
           chunks_.capacity() * sizeof(chunks_[0]);
  }

  void reserve(size_type member_count) {
    offsets_.reserve(member_count);
    lengths_.reserve(member_count);
  }

  void reserve_additional(size_type additional) {
    if (additional == 0) {
      return;
    }
    const auto need = size() + additional;
    if (need <= offsets_.capacity()) {
      return;
    }
    // Geometric growth: do not collapse to exact size() + batch.
    auto capacity = offsets_.capacity() == 0 ? size_type{8} : offsets_.capacity();
    while (capacity < need) {
      const auto grown =
          static_cast<size_type>(static_cast<double>(capacity) * growth_);
      capacity = grown > capacity ? grown : capacity + 1;
    }
    offsets_.reserve(capacity);
    lengths_.reserve(capacity);
  }

  // Encode `member` and append. Returns the dense member id. The encoded form
  // is what MemberIndex hashes and compares via view(id).
  [[nodiscard]] std::uint32_t push_back(std::string_view member) {
    const EncodedString encoded(member, string_encoding_);
    return push_back_encoded(encoded);
  }

  [[nodiscard]] std::uint32_t push_back_encoded(const EncodedString& encoded) {
    if (offsets_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("set member id space exhausted");
    }
    if (encoded.size() > kMaxEncodedBytes) {
      throw std::length_error("set member too large");
    }
    const auto id = static_cast<std::uint32_t>(offsets_.size());
    const auto offset = append_encoded(encoded);
    offsets_.push_back(offset);
    lengths_.push_back(static_cast<std::uint16_t>(encoded.size()));
    return id;
  }

  // Encoded bytes for MemberIndex probes (hash + equality).
  [[nodiscard]] std::string_view view(std::uint32_t member_id) const noexcept {
    assert(member_id < offsets_.size());
    const auto length = lengths_[member_id];
    if (length == 0) {
      return {};
    }
    const auto offset = offsets_[member_id];
    const char* data = chunks_[offset >> chunk_shift_].get() +
                       (offset & chunk_mask_);
    return std::string_view(data, length);
  }

  [[nodiscard]] EncodedStringView encoded_view(
      std::uint32_t member_id) const noexcept {
    return EncodedStringView(view(member_id),
                             string_encoding_.encoding_enabled());
  }

  void orphan(std::uint32_t member_id) noexcept {
    assert(member_id < lengths_.size());
    dead_bytes_ += lengths_[member_id];
    assert(dead_bytes_ <= used_bytes_);
  }

  void copy_ref(std::uint32_t dst, std::uint32_t src) noexcept {
    assert(dst < offsets_.size() && src < offsets_.size());
    offsets_[dst] = offsets_[src];
    lengths_[dst] = lengths_[src];
  }

  void pop_back() noexcept {
    assert(!offsets_.empty());
    offsets_.pop_back();
    lengths_.pop_back();
  }

  // Id-stable reclaim: repack live encoded members in id order. Offsets change;
  // the Swiss index stays valid without a rebuild.
  void compact() {
    if (dead_bytes_ == 0) {
      return;
    }
    std::vector<std::shared_ptr<char[]>> fresh_chunks;
    size_type fresh_next = 0;
    size_type fresh_active = 0;
    size_type fresh_committed = 0;
    size_type fresh_used = 0;

    const auto count = offsets_.size();
    for (size_type id = 0; id < count; ++id) {
      const auto member = view(static_cast<std::uint32_t>(id));
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
  }

  void shrink_to_fit() {
    offsets_.shrink_to_fit();
    lengths_.shrink_to_fit();
    chunks_.shrink_to_fit();
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

 private:
  void configure_chunk_bytes(size_type chunk_bytes) {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes ||
        chunk_bytes > (size_type{1} << 31)) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
  }

  [[nodiscard]] std::uint32_t append_encoded(const EncodedString& encoded) {
    const auto n = encoded.size();
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
  std::vector<std::shared_ptr<char[]>> chunks_;
  size_type next_offset_{0};
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type active_bytes_{0};
  size_type committed_bytes_{0};
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{21};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  double growth_{kDefaultArenaGrowth};
  StringEncodingOptions string_encoding_{};
};

}  // namespace goblin::core
