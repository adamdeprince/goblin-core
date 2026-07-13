#pragma once

// Byte storage for full-form Redis lists. Values are immutable once appended;
// list order lives separately in AdaptivePma, whose entries carry the split
// {u32 block, u32 offset} address and a u16 length. Erase/replace marks old bytes
// dead and List::compact() repacks live values into a fresh arena.

#include <bit>
#include <cassert>
#include <cmath>
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

struct ListValueRef {
  std::uint32_t block{0};
  std::uint32_t offset{0};
  std::uint16_t length{0};

  friend bool operator==(const ListValueRef&, const ListValueRef&) = default;
};
static_assert(sizeof(ListValueRef) == 12);

class ListValueArena {
 public:
  using size_type = std::size_t;

  static constexpr size_type kDefaultChunkBytes = size_type{1} << 21;  // 2 MiB (x86 huge page)
  static constexpr size_type kMinChunkBytes = size_type{1} << 16;
  static constexpr size_type kMaxChunkBytes = static_cast<size_type>(
      sizeof(size_type) > 4 ? (std::uint64_t{1} << 32)
                            : (std::uint64_t{1} << 31));
  static constexpr size_type kMaxValueBytes =
      std::numeric_limits<std::uint16_t>::max();

  explicit ListValueArena(size_type chunk_bytes = kDefaultChunkBytes,
                          double growth = kDefaultArenaGrowth)
      : growth_(growth > 1.0 && std::isfinite(growth) ? growth
                                                      : kDefaultArenaGrowth) {
    configure_chunk_bytes(chunk_bytes);
  }

  [[nodiscard]] ListValueRef append(std::string_view value) {
    if (value.size() > kMaxValueBytes) {
      throw std::length_error("list value too large");
    }
    if (value.empty()) {
      return {};
    }
    const auto block = next_offset_ >> chunk_shift_;
    const auto offset = next_offset_ & chunk_mask_;
    const auto last_block =
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max());
    if (block > last_block ||
        (block == last_block && value.size() > chunk_bytes_ - offset)) {
      throw std::length_error("list value arena exhausted");
    }
    const auto r = reserve_run_bytes_split(
        blocks_, next_offset_, active_bytes_, committed_bytes_, chunk_bytes_,
        chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, value.size());
    if (r.dst != nullptr) {
      std::memcpy(r.dst, value.data(), value.size());
    }
    used_bytes_ += value.size();
    return {.block = r.block,
            .offset = r.offset,
            .length = static_cast<std::uint16_t>(value.size())};
  }

  [[nodiscard]] std::string_view view(ListValueRef ref) const noexcept {
    if (ref.length == 0) {
      return {};
    }
    assert(ref.block < blocks_.size());
    assert(static_cast<size_type>(ref.offset) + ref.length <= chunk_bytes_);
    return std::string_view(blocks_[ref.block].get() + ref.offset, ref.length);
  }

  void orphan(ListValueRef ref) noexcept {
    dead_bytes_ += ref.length;
    assert(dead_bytes_ <= used_bytes_);
  }

  [[nodiscard]] bool should_compact() const noexcept {
    return dead_bytes_ >= kCompactFloorBytes && dead_bytes_ >= live_bytes();
  }

  [[nodiscard]] size_type chunk_bytes() const noexcept { return chunk_bytes_; }
  [[nodiscard]] double growth() const noexcept { return growth_; }
  [[nodiscard]] size_type used_bytes() const noexcept { return used_bytes_; }
  [[nodiscard]] size_type dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }
  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return committed_bytes_ + blocks_.capacity() * sizeof(blocks_[0]);
  }

 private:
  static constexpr size_type kCompactFloorBytes = size_type{1} << 16;

  void configure_chunk_bytes(size_type chunk_bytes) noexcept {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes ||
        chunk_bytes > kMaxChunkBytes) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
  }

  std::vector<std::shared_ptr<char[]>> blocks_;
  size_type next_offset_{0};
  size_type active_bytes_{0};
  size_type committed_bytes_{0};
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{20};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  double growth_{kDefaultArenaGrowth};
};

}  // namespace goblin::core
