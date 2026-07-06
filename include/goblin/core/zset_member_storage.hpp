#pragma once

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

#include "goblin/core/score_format.hpp"

namespace goblin::core {

class ZSetMemberStorage {
 public:
  using size_type = std::size_t;

  static constexpr size_type kDefaultChunkBytes = size_type{1} << 20;  // 1 MiB
  // A member (<= 64 KiB) must fit one chunk: 2^16 = 64 KiB.
  static constexpr size_type kMinChunkBytes = size_type{1} << 16;

  // Value view of a member's location and score. Storage is kept
  // struct-of-arrays internally (see offsets_/lengths_/scores_); Ref is only a
  // transient carrier for snapshot/restore and the *_ref helpers.
  struct Ref {
    std::uint32_t offset{0};
    std::uint32_t length{0};
    double score{0.0};
  };

  struct ScoreTextRef {
    std::uint32_t offset{0};
    std::uint32_t length{0};
  };
  static_assert(sizeof(ScoreTextRef) == 8);

  struct Snapshot {
    Ref ref;
    ScoreTextRef score_text;
  };

  ZSetMemberStorage() = default;

  explicit ZSetMemberStorage(bool score_string_cache,
                             size_type chunk_bytes = kDefaultChunkBytes)
      : score_string_cache_enabled_(score_string_cache) {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
  }

  [[nodiscard]] size_type chunk_bytes() const noexcept { return chunk_bytes_; }

  [[nodiscard]] bool empty() const noexcept {
    return offsets_.empty();
  }

  [[nodiscard]] size_type size() const noexcept {
    return offsets_.size();
  }

  [[nodiscard]] size_type byte_size() const noexcept {
    return used_bytes_;
  }

  // Member bytes orphaned by a removal, reclaimable by compact. dead/live drives
  // the arena auto-compaction after ZREM (scores update in place, so only
  // removals orphan member bytes).
  [[nodiscard]] size_type dead_bytes() const noexcept {
    return dead_bytes_;
  }

  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }

  // Account a to-be-removed member's bytes as dead (the ZSet swap-removes the id
  // out of the indexes; the bytes stay in the arena until compact). Call before
  // copy_ref overwrites the slot.
  void orphan(std::uint32_t member_id) noexcept {
    assert(member_id < lengths_.size());
    dead_bytes_ += lengths_[member_id];
  }

  [[nodiscard]] size_type byte_capacity() const noexcept {
    return chunks_.size() * chunk_bytes_;
  }

  [[nodiscard]] size_type ref_capacity() const noexcept {
    return offsets_.capacity();
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return byte_capacity() +
           offsets_.capacity() * sizeof(std::uint32_t) +
           lengths_.capacity() * sizeof(std::uint16_t) +
           scores_.capacity() * sizeof(double) +
           chunks_.capacity() * sizeof(std::unique_ptr<char[]>) +
           score_text_allocated_bytes();
  }

  [[nodiscard]] bool score_string_cache_enabled() const noexcept {
    return score_string_cache_enabled_;
  }

  [[nodiscard]] size_type score_text_byte_size() const noexcept {
    return score_text_used_bytes_;
  }

  [[nodiscard]] size_type score_text_byte_capacity() const noexcept {
    return score_text_chunks_.size() * chunk_bytes_;
  }

  [[nodiscard]] size_type score_text_ref_capacity() const noexcept {
    return score_text_refs_.capacity();
  }

  [[nodiscard]] size_type score_text_allocated_bytes() const noexcept {
    return score_text_byte_capacity() +
           score_text_refs_.capacity() * sizeof(ScoreTextRef) +
           score_text_chunks_.capacity() * sizeof(std::unique_ptr<char[]>);
  }

  void reserve(size_type member_count) {
    offsets_.reserve(member_count);
    lengths_.reserve(member_count);
    scores_.reserve(member_count);
    if (score_string_cache_enabled_) {
      score_text_refs_.reserve(member_count);
    }
  }

  void reserve_bytes(size_type byte_count) {
    chunks_.reserve((byte_count + chunk_bytes_ - 1) / chunk_bytes_);
  }

  void reserve_score_text_bytes(size_type byte_count) {
    if (score_string_cache_enabled_) {
      score_text_chunks_.reserve((byte_count + chunk_bytes_ - 1) / chunk_bytes_);
    }
  }

  [[nodiscard]] std::uint32_t push_back(std::string_view member, double score) {
    if (offsets_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member id space exhausted");
    }
    if (member.size() > kMaxMemberBytes) {
      throw std::length_error("zset member too large");
    }
    if (next_offset_ >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            member.size() - (chunk_bytes_ - 1)) {
      throw std::length_error("zset member arena exhausted");
    }

    const auto id = static_cast<std::uint32_t>(offsets_.size());
    const auto offset = append_bytes(member);
    offsets_.push_back(offset);
    lengths_.push_back(static_cast<std::uint16_t>(member.size()));
    scores_.push_back(score);
    if (score_string_cache_enabled_) {
      score_text_refs_.push_back(append_score_text(score));
    }
    return id;
  }

  void replace(std::uint32_t member_id, std::string_view member, double score) {
    assert(member_id < offsets_.size());
    if (member.size() > kMaxMemberBytes) {
      throw std::length_error("zset member too large");
    }
    if (next_offset_ >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            member.size() - (chunk_bytes_ - 1)) {
      throw std::length_error("zset member arena exhausted");
    }

    const auto offset = append_bytes(member);
    offsets_[member_id] = offset;
    lengths_[member_id] = static_cast<std::uint16_t>(member.size());
    scores_[member_id] = score;
    set_score_text(member_id, score);
  }

  [[nodiscard]] Ref ref(std::uint32_t member_id) const noexcept {
    assert(member_id < offsets_.size());
    return Ref{.offset = offsets_[member_id],
               .length = lengths_[member_id],
               .score = scores_[member_id]};
  }

  [[nodiscard]] Snapshot snapshot(std::uint32_t member_id) const noexcept {
    assert(member_id < offsets_.size());
    return Snapshot{
        .ref = ref(member_id),
        .score_text = score_string_cache_enabled_ ? score_text_refs_[member_id]
                                                  : ScoreTextRef{},
    };
  }

  void restore_snapshot(std::uint32_t member_id, Snapshot snapshot) noexcept {
    assert(member_id < offsets_.size());
    offsets_[member_id] = snapshot.ref.offset;
    lengths_[member_id] = static_cast<std::uint16_t>(snapshot.ref.length);
    scores_[member_id] = snapshot.ref.score;
    if (score_string_cache_enabled_) {
      assert(member_id < score_text_refs_.size());
      score_text_refs_[member_id] = snapshot.score_text;
    }
  }

  void set_ref(std::uint32_t member_id, Ref ref) {
    assert(member_id < offsets_.size());
    offsets_[member_id] = ref.offset;
    lengths_[member_id] = static_cast<std::uint16_t>(ref.length);
    scores_[member_id] = ref.score;
    set_score_text(member_id, ref.score);
  }

  void copy_ref(std::uint32_t dst_member_id, std::uint32_t src_member_id) noexcept {
    assert(dst_member_id < offsets_.size());
    assert(src_member_id < offsets_.size());
    offsets_[dst_member_id] = offsets_[src_member_id];
    lengths_[dst_member_id] = lengths_[src_member_id];
    scores_[dst_member_id] = scores_[src_member_id];
    if (score_string_cache_enabled_) {
      assert(dst_member_id < score_text_refs_.size());
      assert(src_member_id < score_text_refs_.size());
      score_text_refs_[dst_member_id] = score_text_refs_[src_member_id];
    }
  }

  [[nodiscard]] double score(std::uint32_t member_id) const noexcept {
    assert(member_id < scores_.size());
    return scores_[member_id];
  }

  void set_score(std::uint32_t member_id, double score) {
    assert(member_id < scores_.size());
    scores_[member_id] = score;
    set_score_text(member_id, score);
  }

  void pop_back() noexcept {
    assert(!offsets_.empty());
    offsets_.pop_back();
    lengths_.pop_back();
    scores_.pop_back();
    if (score_string_cache_enabled_) {
      assert(!score_text_refs_.empty());
      score_text_refs_.pop_back();
    }
  }

  [[nodiscard]] std::string_view view(std::uint32_t member_id) const noexcept {
    assert(member_id < offsets_.size());
    const auto length = lengths_[member_id];
    const char* data = "";
    if (length != 0) {
      const auto offset = offsets_[member_id];
      data = chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_);
    }
    return std::string_view(data, length);
  }

  // Warm the struct-of-arrays reference (offset + length) for a member id.
  // Range iteration visits ids in score order, which is scattered relative to
  // id order, so these are cache misses worth prefetching ahead.
  void prefetch_location(std::uint32_t member_id) const noexcept {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(offsets_.data() + member_id);
    __builtin_prefetch(lengths_.data() + member_id);
#else
    (void)member_id;
#endif
  }

  // Warm the packed member bytes. The offset read here is expected to already
  // be resident from an earlier prefetch_location() at a longer distance.
  void prefetch_bytes(std::uint32_t member_id) const noexcept {
#if defined(__GNUC__) || defined(__clang__)
    if (lengths_[member_id] == 0) {
      return;
    }
    const auto offset = offsets_[member_id];
    __builtin_prefetch(chunks_[offset >> chunk_shift_].get() + (offset & chunk_mask_));
#else
    (void)member_id;
#endif
  }

  [[nodiscard]] std::string_view score_text(std::uint32_t member_id) const noexcept {
    if (!score_string_cache_enabled_) {
      return {};
    }
    assert(member_id < score_text_refs_.size());
    const auto ref = score_text_refs_[member_id];
    const char* data = "";
    if (ref.length != 0) {
      data = score_text_chunks_[ref.offset >> chunk_shift_].get() +
             (ref.offset & chunk_mask_);
    }
    return std::string_view(data, ref.length);
  }

 private:
  // Members are addressed by a 16-bit length, so a single member is capped at
  // 64 KiB - 1. This keeps the per-member reference at 14 bytes (u32 offset +
  // u16 length + f64 score, struct-of-arrays) instead of 16, and is far above
  // any realistic sorted-set member.
  static constexpr size_type kMaxMemberBytes =
      std::numeric_limits<std::uint16_t>::max();

  [[nodiscard]] std::uint32_t append_bytes(std::string_view member) {
    if (member.empty()) {
      return static_cast<std::uint32_t>(next_offset_);
    }

    auto chunk_offset = next_offset_ & chunk_mask_;
    // If the member would straddle a chunk boundary, skip to the next chunk.
    if (chunk_offset + member.size() > chunk_bytes_) {
      next_offset_ += chunk_bytes_ - chunk_offset;
      chunk_offset = 0;
    }
    // Ensure the chunk holding next_offset_ exists. Keying the allocation on the
    // chunk index (not the fit check) covers the case where the previous member
    // filled a chunk exactly: next_offset_ then sits on a boundary with
    // chunk_offset == 0, the fit check passes, but that chunk is not yet
    // allocated.
    const size_type chunk_index = next_offset_ >> chunk_shift_;
    while (chunks_.size() <= chunk_index) {
      chunks_.push_back(std::make_unique_for_overwrite<char[]>(chunk_bytes_));
    }

    const auto offset = static_cast<std::uint32_t>(next_offset_);
    std::memcpy(chunks_[chunk_index].get() + chunk_offset,
                member.data(),
                member.size());
    next_offset_ += member.size();
    used_bytes_ += member.size();
    return offset;
  }

  [[nodiscard]] ScoreTextRef append_score_text(double score) {
    const auto text = score_format::format(score);
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset score text too large");
    }
    if (score_text_next_offset_ >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            text.size() - (chunk_bytes_ - 1)) {
      throw std::length_error("zset score text arena exhausted");
    }
    return append_score_text_bytes(text);
  }

  [[nodiscard]] ScoreTextRef append_score_text_bytes(std::string_view text) {
    if (text.empty()) {
      return ScoreTextRef{.offset = static_cast<std::uint32_t>(score_text_next_offset_),
                          .length = 0};
    }

    auto chunk_offset = score_text_next_offset_ & chunk_mask_;
    // If the text would straddle a chunk boundary, skip to the next chunk.
    if (chunk_offset + text.size() > chunk_bytes_) {
      score_text_next_offset_ += chunk_bytes_ - chunk_offset;
      chunk_offset = 0;
    }
    // Same exact-fill guard as append_bytes: allocate by chunk index so a
    // chunk filled to the byte does not leave next_offset_ pointing at an
    // unallocated chunk.
    const size_type chunk_index = score_text_next_offset_ >> chunk_shift_;
    while (score_text_chunks_.size() <= chunk_index) {
      score_text_chunks_.push_back(std::make_unique_for_overwrite<char[]>(chunk_bytes_));
    }

    const auto offset = static_cast<std::uint32_t>(score_text_next_offset_);
    std::memcpy(score_text_chunks_[chunk_index].get() + chunk_offset,
                text.data(),
                text.size());
    score_text_next_offset_ += text.size();
    score_text_used_bytes_ += text.size();
    return ScoreTextRef{.offset = offset,
                        .length = static_cast<std::uint32_t>(text.size())};
  }

  void set_score_text(std::uint32_t member_id, double score) {
    if (!score_string_cache_enabled_) {
      return;
    }
    assert(member_id < score_text_refs_.size());
    score_text_refs_[member_id] = append_score_text(score);
  }

  std::vector<std::unique_ptr<char[]>> chunks_;
  std::vector<std::uint32_t> offsets_;
  std::vector<std::uint16_t> lengths_;
  std::vector<double> scores_;
  std::vector<std::unique_ptr<char[]>> score_text_chunks_;
  std::vector<ScoreTextRef> score_text_refs_;
  size_type next_offset_{0};
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type score_text_next_offset_{0};
  size_type score_text_used_bytes_{0};
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{20};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  bool score_string_cache_enabled_{false};
};

}  // namespace goblin::core
