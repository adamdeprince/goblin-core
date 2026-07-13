#pragma once

// Byte storage for full-form Redis lists. Values are immutable once appended;
// list order lives separately in AdaptivePma. A reference carries a tagged u32
// block, a u32 offset, and a u16 stored length; AdaptivePma persists it in a
// narrow six-byte form until the arena crosses 2 GiB. The block's high bit says
// that a raw value omitted its redundant 0xff prefix. Erase/replace marks old
// bytes dead and List::compact() repacks live values into a fresh arena.

#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/page_arena.hpp"
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

struct ListValueRef {
  static constexpr std::uint32_t kRawMask = std::uint32_t{1} << 31;
  static constexpr std::uint32_t kBlockMask = ~kRawMask;

  std::uint32_t block{0};
  std::uint32_t offset{0};
  std::uint16_t length{0};

  [[nodiscard]] bool raw_prefix_omitted() const noexcept {
    return (block & kRawMask) != 0;
  }
  [[nodiscard]] std::uint32_t block_index() const noexcept {
    return block & kBlockMask;
  }

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
  static constexpr size_type kMaxValueBytes = kStringMaxBytes;

  explicit ListValueArena(size_type chunk_bytes = kDefaultChunkBytes,
                          double growth = kDefaultArenaGrowth,
                          StringEncodingOptions string_encoding = {},
                          StringCompressionMode compression =
                              StringCompressionMode::AllowLz4)
      : growth_(growth > 1.0 && std::isfinite(growth) ? growth
                                                      : kDefaultArenaGrowth),
        string_encoding_(string_encoding),
        compression_(compression) {
    configure_chunk_bytes(chunk_bytes);
  }

  [[nodiscard]] ListValueRef append(std::string_view value) {
    const EncodedString encoded(value, string_encoding_, compression_);
    const bool omit_raw_prefix =
        encoded.encoding_enabled() && encoded.is_raw();
    const auto stored_size =
        encoded.size() - static_cast<size_type>(omit_raw_prefix);
    return append_prepared(
        stored_size, omit_raw_prefix, [&encoded, omit_raw_prefix](char* dst) {
          encoded.write_range_to(static_cast<size_type>(omit_raw_prefix),
                                 encoded.size() - omit_raw_prefix, dst);
        });
  }

  [[nodiscard]] ListValueRef append_encoded(EncodedStringView value) {
    if (!value.valid() ||
        value.encoded_size() > std::numeric_limits<std::uint16_t>::max()) {
      throw std::length_error("invalid encoded list value");
    }
    const bool omit_raw_prefix =
        value.encoding_enabled() && value.is_raw();
    const auto stored_size =
        value.encoded_size() - static_cast<size_type>(omit_raw_prefix);
    return append_prepared(
        stored_size, omit_raw_prefix, [&value, omit_raw_prefix](char* dst) {
          value.copy_encoded_range_to(
              static_cast<size_type>(omit_raw_prefix),
              value.encoded_size() - omit_raw_prefix, dst);
        });
  }

  // Restore an all-raw ordered batch into an empty arena. Block boundaries and
  // final allocation sizes are planned first, avoiding the normal geometric
  // active-tail growth copies when the complete input is already available.
  [[nodiscard]] std::vector<ListValueRef> assign_raw(
      std::span<const std::string_view> values, bool encoding_enabled) {
    assert(blocks_.empty() && next_offset_ == 0 && used_bytes_ == 0);
    std::vector<ListValueRef> refs;
    refs.reserve(values.size());
    std::vector<size_type> block_used;
    size_type block = 0;
    size_type offset = 0;
    size_type total_used = 0;

    for (const auto value : values) {
      if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::length_error("encoded list value too large");
      }
      if (offset == chunk_bytes_) {
        ++block;
        offset = 0;
      }
      if (!value.empty() && value.size() > chunk_bytes_ - offset) {
        ++block;
        offset = 0;
      }
      if (block > static_cast<size_type>(ListValueRef::kBlockMask)) {
        throw std::length_error("list value arena exhausted");
      }
      refs.push_back({
          .block = static_cast<std::uint32_t>(block) |
                   (encoding_enabled ? ListValueRef::kRawMask : 0),
          .offset = static_cast<std::uint32_t>(offset),
          .length = static_cast<std::uint16_t>(value.size()),
      });
      if (!value.empty()) {
        if (block_used.size() <= block) {
          block_used.resize(block + 1, 0);
        }
        offset += value.size();
        block_used[block] = offset;
        if (value.size() > std::numeric_limits<size_type>::max() - total_used) {
          throw std::length_error("list value arena exhausted");
        }
        total_used += value.size();
      }
    }

    blocks_.reserve(block_used.size());
    committed_bytes_ = 0;
    for (const auto bytes : block_used) {
      blocks_.push_back(alloc_page_block(bytes));
      committed_bytes_ += page_block_alloc_bytes(bytes);
    }
    active_bytes_ = block_used.empty()
                        ? 0
                        : page_block_alloc_bytes(block_used.back());
    next_offset_ = block * chunk_bytes_ + offset;
    used_bytes_ = total_used;
    dead_bytes_ = 0;

    for (std::size_t index = 0; index < values.size(); ++index) {
      if (!values[index].empty()) {
        const auto& ref = refs[index];
        std::memcpy(blocks_[ref.block_index()].get() + ref.offset,
                    values[index].data(), values[index].size());
      }
    }
    return refs;
  }

  [[nodiscard]] EncodedStringView view(ListValueRef ref) const noexcept {
    const auto block = ref.block_index();
    assert(block < blocks_.size() || ref.length == 0);
    assert(static_cast<size_type>(ref.offset) + ref.length <= chunk_bytes_);
    const auto stored = ref.length == 0
                            ? std::string_view{}
                            : std::string_view(blocks_[block].get() + ref.offset,
                                               ref.length);
    if (ref.raw_prefix_omitted()) {
      return EncodedStringView(std::string_view(&kRawPrefix, 1), stored, true);
    }
    return EncodedStringView(stored, string_encoding_.encoding_enabled());
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

  // Tighten the active tail to exactly the pages needed by its written bytes.
  // Frozen blocks stay untouched. This intentionally demotes a partial HugeTLB
  // tail to ordinary pages; only full frozen blocks should retain huge pages.
  void shrink_to_fit() {
    blocks_.shrink_to_fit();
    if (blocks_.empty()) {
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
    blocks_.back() = grow_page_block(blocks_.back(), used, used);
    committed_bytes_ -= active_bytes_ - desired;
    active_bytes_ = desired;
  }

 private:
  static constexpr size_type kCompactFloorBytes = size_type{1} << 16;

  template <class Write>
  [[nodiscard]] ListValueRef append_prepared(size_type stored_size,
                                              bool raw_prefix_omitted,
                                              Write&& write) {
    if (stored_size > std::numeric_limits<std::uint16_t>::max()) {
      throw std::length_error("encoded list value too large");
    }
    const auto block = next_offset_ >> chunk_shift_;
    const auto offset = next_offset_ & chunk_mask_;
    const auto last_block =
        static_cast<size_type>(ListValueRef::kBlockMask);
    if (block > last_block ||
        (block == last_block && stored_size > chunk_bytes_ - offset)) {
      throw std::length_error("list value arena exhausted");
    }
    const auto r = reserve_run_bytes_split(
        blocks_, next_offset_, active_bytes_, committed_bytes_, chunk_bytes_,
        chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, stored_size);
    if (r.dst != nullptr) {
      std::forward<Write>(write)(r.dst);
    }
    used_bytes_ += stored_size;
    return {.block = r.block |
                     (raw_prefix_omitted ? ListValueRef::kRawMask : 0),
            .offset = r.offset,
            .length = static_cast<std::uint16_t>(stored_size)};
  }

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
  StringEncodingOptions string_encoding_{};
  StringCompressionMode compression_{StringCompressionMode::AllowLz4};
  static inline constexpr char kRawPrefix = static_cast<char>(kStringRaw);
};

}  // namespace goblin::core
