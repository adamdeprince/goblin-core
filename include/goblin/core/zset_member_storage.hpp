#pragma once

#include <algorithm>
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
#include "goblin/core/score_format.hpp"
#include "goblin/core/score_width.hpp"

namespace goblin::core {

inline constexpr std::size_t kZSetMemberDefaultChunkBytes = std::size_t{1} << 21;  // 2 MiB (x86 huge page)

// Immutable member/score-text byte chunks shared across copy-on-write splits.
// SoA metadata (offsets, lengths, scores) lives in ZSetMemberStorage and forks
// independently; the arena forks only before a structural append.
struct ZSetMemberByteArena {
  using size_type = std::size_t;

  std::vector<std::shared_ptr<char[]>> member_chunks;
  std::vector<std::shared_ptr<char[]>> score_text_chunks;
  size_type member_next_offset{0};
  size_type member_used_bytes{0};
  size_type member_dead_bytes{0};
  size_type member_active_bytes{0};     // physical size of the last member block
  size_type member_committed_bytes{0};  // sum of member block physical sizes
  size_type score_text_next_offset{0};
  size_type score_text_used_bytes{0};
  size_type score_text_active_bytes{0};
  size_type score_text_committed_bytes{0};
  size_type chunk_bytes{kZSetMemberDefaultChunkBytes};
  size_type chunk_shift{21};  // re-derived by configure_chunk_bytes; matches the 2 MiB default
  size_type chunk_mask{kZSetMemberDefaultChunkBytes - 1};
  double growth{kDefaultArenaGrowth};

  [[nodiscard]] std::shared_ptr<ZSetMemberByteArena> fork() const {
    ensure_memory_growth(sizeof(ZSetMemberByteArena) +
                         member_chunks.capacity() * sizeof(member_chunks[0]) +
                         score_text_chunks.capacity() *
                             sizeof(score_text_chunks[0]));
    return std::make_shared<ZSetMemberByteArena>(*this);
  }
};

class ZSetMemberStorage {
 public:
  using size_type = std::size_t;

  static constexpr size_type kDefaultChunkBytes = kZSetMemberDefaultChunkBytes;
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
                             size_type chunk_bytes = kDefaultChunkBytes,
                             double growth = kDefaultArenaGrowth)
      : arena_(std::make_shared<ZSetMemberByteArena>()),
        score_string_cache_enabled_(score_string_cache) {
    configure_chunk_bytes(chunk_bytes);
    arena_->growth = growth > 1.0 ? growth : kDefaultArenaGrowth;
  }

  [[nodiscard]] size_type chunk_bytes() const noexcept { return arena_->chunk_bytes; }

  [[nodiscard]] bool empty() const noexcept {
    return offsets_.empty();
  }

  [[nodiscard]] size_type size() const noexcept {
    return offsets_.size();
  }

  [[nodiscard]] size_type byte_size() const noexcept {
    return arena_->member_used_bytes;
  }

  // Member bytes orphaned by a removal, reclaimable by compact. dead/live drives
  // the arena auto-compaction after ZREM (scores update in place, so only
  // removals orphan member bytes).
  [[nodiscard]] size_type dead_bytes() const noexcept {
    return arena_->member_dead_bytes;
  }

  [[nodiscard]] size_type live_bytes() const noexcept {
    return arena_->member_used_bytes - arena_->member_dead_bytes;
  }

  // Account a to-be-removed member's bytes as dead (the ZSet swap-removes the id
  // out of the indexes; the bytes stay in the arena until compact). Call before
  // copy_ref overwrites the slot.
  void orphan(std::uint32_t member_id) noexcept {
    assert(member_id < lengths_.size());
    arena_->member_dead_bytes += lengths_[member_id];
  }

  [[nodiscard]] size_type byte_capacity() const noexcept {
    return arena_->member_committed_bytes;
  }

  [[nodiscard]] size_type ref_capacity() const noexcept {
    return offsets_.capacity();
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    const auto arena_shares = std::max<long>(1, arena_.use_count());
    const auto arena_bytes =
        arena_->member_committed_bytes + arena_->score_text_committed_bytes +
        arena_->member_chunks.capacity() * sizeof(arena_->member_chunks[0]) +
        arena_->score_text_chunks.capacity() *
            sizeof(arena_->score_text_chunks[0]) +
        sizeof(ZSetMemberByteArena);
    const auto shared_arena_bytes =
        arena_bytes / static_cast<size_type>(arena_shares) +
        static_cast<size_type>(arena_bytes % arena_shares != 0);
    return shared_arena_bytes +
           offsets_.capacity() * sizeof(std::uint32_t) +
           lengths_.capacity() * sizeof(std::uint16_t) +
           scores_.allocated_bytes() +
           score_text_refs_.capacity() * sizeof(ScoreTextRef);
  }

  [[nodiscard]] bool score_string_cache_enabled() const noexcept {
    return score_string_cache_enabled_;
  }

  [[nodiscard]] size_type score_text_byte_size() const noexcept {
    return arena_->score_text_used_bytes;
  }

  [[nodiscard]] size_type score_text_byte_capacity() const noexcept {
    return arena_->score_text_committed_bytes;
  }

  [[nodiscard]] size_type score_text_ref_capacity() const noexcept {
    return score_text_refs_.capacity();
  }

  [[nodiscard]] size_type score_text_allocated_bytes() const noexcept {
    const auto arena_shares = std::max<long>(1, arena_.use_count());
    const auto arena_bytes =
        score_text_byte_capacity() +
        arena_->score_text_chunks.capacity() *
            sizeof(arena_->score_text_chunks[0]);
    return arena_bytes / static_cast<size_type>(arena_shares) +
           static_cast<size_type>(arena_bytes % arena_shares != 0) +
           score_text_refs_.capacity() * sizeof(ScoreTextRef);
  }

  // Share immutable byte chunks; copy SoA metadata. Used on per-key score updates
  // when the member layer is still shared.
  [[nodiscard]] ZSetMemberStorage clone_shallow() const {
    ZSetMemberStorage copy;
    copy.arena_ = arena_;
    copy.score_string_cache_enabled_ = score_string_cache_enabled_;
    copy.offsets_ = offsets_;
    copy.lengths_ = lengths_;
    copy.scores_ = scores_;
    copy.score_text_refs_ = score_text_refs_;
    return copy;
  }

  // Fork the byte arena before a structural append when multiple storages share it.
  void ensure_unique_arena() {
    if (arena_.use_count() > 1) {
      arena_ = arena_->fork();
    }
  }

  void reserve(size_type member_count) {
    reserve_memory_vector(offsets_, member_count);
    reserve_memory_vector(lengths_, member_count);
    scores_.reserve(member_count);
    if (score_string_cache_enabled_) {
      reserve_memory_vector(score_text_refs_, member_count);
    }
  }

  void reserve_bytes(size_type byte_count) {
    reserve_memory_vector(
        arena_->member_chunks,
        (byte_count + arena_->chunk_bytes - 1) / arena_->chunk_bytes);
  }

  void reserve_score_text_bytes(size_type byte_count) {
    if (score_string_cache_enabled_) {
      reserve_memory_vector(
          arena_->score_text_chunks,
          (byte_count + arena_->chunk_bytes - 1) / arena_->chunk_bytes);
    }
  }

  [[nodiscard]] std::uint32_t push_back(std::string_view member, double score) {
    ensure_unique_arena();
    if (offsets_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member id space exhausted");
    }
    if (member.size() > kMaxMemberBytes) {
      throw std::length_error("zset member too large");
    }
    if (arena_->member_next_offset >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            member.size() - (arena_->chunk_bytes - 1)) {
      throw std::length_error("zset member arena exhausted");
    }

    const auto id = static_cast<std::uint32_t>(offsets_.size());
    if (offsets_.size() == offsets_.capacity()) {
      const auto current = offsets_.capacity();
      reserve(current == 0 ? size_type{8} : current * 2);
    }
    scores_.reserve_push(score);
    const auto offset = append_bytes(member);
    ScoreTextRef score_text{};
    try {
      if (score_string_cache_enabled_) {
        score_text = append_score_text(score);
      }
    } catch (...) {
      arena_->member_dead_bytes += member.size();
      throw;
    }
    offsets_.push_back(offset);
    lengths_.push_back(static_cast<std::uint16_t>(member.size()));
    scores_.push_back(score);
    if (score_string_cache_enabled_) {
      score_text_refs_.push_back(score_text);
    }
    return id;
  }

  void replace(std::uint32_t member_id, std::string_view member, double score) {
    ensure_unique_arena();
    assert(member_id < offsets_.size());
    if (member.size() > kMaxMemberBytes) {
      throw std::length_error("zset member too large");
    }
    if (arena_->member_next_offset >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            member.size() - (arena_->chunk_bytes - 1)) {
      throw std::length_error("zset member arena exhausted");
    }

    const auto offset = append_bytes(member);
    offsets_[member_id] = offset;
    lengths_[member_id] = static_cast<std::uint16_t>(member.size());
    scores_.set(member_id, score);
    set_score_text(member_id, score);
  }

  [[nodiscard]] Ref ref(std::uint32_t member_id) const noexcept {
    assert(member_id < offsets_.size());
    return Ref{.offset = offsets_[member_id],
               .length = lengths_[member_id],
               .score = scores_.at(member_id)};
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
    scores_.set(member_id, snapshot.ref.score);
    if (score_string_cache_enabled_) {
      assert(member_id < score_text_refs_.size());
      score_text_refs_[member_id] = snapshot.score_text;
    }
  }

  void set_ref(std::uint32_t member_id, Ref ref) {
    assert(member_id < offsets_.size());
    offsets_[member_id] = ref.offset;
    lengths_[member_id] = static_cast<std::uint16_t>(ref.length);
    scores_.set(member_id, ref.score);
    set_score_text(member_id, ref.score);
  }

  void copy_ref(std::uint32_t dst_member_id, std::uint32_t src_member_id) noexcept {
    assert(dst_member_id < offsets_.size());
    assert(src_member_id < offsets_.size());
    offsets_[dst_member_id] = offsets_[src_member_id];
    lengths_[dst_member_id] = lengths_[src_member_id];
    scores_.copy_within(dst_member_id, src_member_id);
    if (score_string_cache_enabled_) {
      assert(dst_member_id < score_text_refs_.size());
      assert(src_member_id < score_text_refs_.size());
      score_text_refs_[dst_member_id] = score_text_refs_[src_member_id];
    }
  }

  [[nodiscard]] double score(std::uint32_t member_id) const noexcept {
    assert(member_id < scores_.size());
    return scores_.at(member_id);
  }

  void set_score(std::uint32_t member_id, double score) {
    assert(member_id < scores_.size());
    scores_.set(member_id, score);
    set_score_text(member_id, score);
  }

  // Width the scores are currently stored at (i16/i32/f64). Widens automatically
  // on set/push_back; rebuild_scores_to is the explicit OPTIMIZE demote path.
  [[nodiscard]] ScoreWidth score_width() const noexcept {
    return scores_.width();
  }

  void rebuild_scores_to(ScoreWidth width) {
    scores_.rebuild_to(width);
  }

  // Reclaim dead arena bytes by repacking every live member (and its score text)
  // into a fresh page-aligned arena in member-id order, updating only offsets.
  // Member ids, lengths, scores, and both indexes are untouched -- so no index
  // rebuild -- and the old arena's blocks are munmap'd when this returns (the
  // fresh arena keeps every block but the last full; only the last is partial).
  // The caller must hold a unique arena (post-fork), as after a structural write.
  void compact() {
    if (arena_->member_dead_bytes == 0) {
      return;
    }
    ensure_memory_growth(sizeof(ZSetMemberByteArena));
    auto fresh = std::make_shared<ZSetMemberByteArena>();
    fresh->chunk_bytes = arena_->chunk_bytes;
    fresh->chunk_shift = arena_->chunk_shift;
    fresh->chunk_mask = arena_->chunk_mask;
    fresh->growth = arena_->growth;

    const auto count = offsets_.size();
    std::vector<std::uint32_t> fresh_offsets;
    reserve_memory_vector(fresh_offsets, count);
    fresh_offsets.resize(count);
    std::vector<ScoreTextRef> fresh_score_text_refs;
    if (score_string_cache_enabled_) {
      reserve_memory_vector(fresh_score_text_refs, count);
      fresh_score_text_refs.resize(count);
    }
    for (std::size_t id = 0; id < count; ++id) {
      const auto member_id = static_cast<std::uint32_t>(id);
      const auto member = view(member_id);  // reads the OLD arena
      const auto r = reserve_run_bytes(
          fresh->member_chunks, fresh->member_next_offset,
          fresh->member_active_bytes, fresh->member_committed_bytes,
          fresh->chunk_bytes, fresh->chunk_shift, fresh->chunk_mask, fresh->growth,
          kInitialArenaBytes, member.size());
      if (r.dst != nullptr) {
        std::memcpy(r.dst, member.data(), member.size());
        fresh->member_used_bytes += member.size();
      }
      fresh_offsets[id] = r.offset;

      if (score_string_cache_enabled_) {
        const auto text = score_text(member_id);  // reads the OLD score-text arena
        const auto tr = reserve_run_bytes(
            fresh->score_text_chunks, fresh->score_text_next_offset,
            fresh->score_text_active_bytes, fresh->score_text_committed_bytes,
            fresh->chunk_bytes, fresh->chunk_shift, fresh->chunk_mask,
            fresh->growth, kInitialArenaBytes, text.size());
        if (tr.dst != nullptr) {
          std::memcpy(tr.dst, text.data(), text.size());
          fresh->score_text_used_bytes += text.size();
        }
        fresh_score_text_refs[id] = ScoreTextRef{
            .offset = tr.offset,
            .length = static_cast<std::uint32_t>(text.size())};
      }
    }

    offsets_ = std::move(fresh_offsets);
    if (score_string_cache_enabled_) {
      score_text_refs_ = std::move(fresh_score_text_refs);
    }
    arena_ = std::move(fresh);  // old arena's blocks are freed/munmap'd here
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
      data = arena_->member_chunks[offset >> arena_->chunk_shift].get() +
             (offset & arena_->chunk_mask);
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
    __builtin_prefetch(arena_->member_chunks[offset >> arena_->chunk_shift].get() +
                       (offset & arena_->chunk_mask));
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
      data = arena_->score_text_chunks[ref.offset >> arena_->chunk_shift].get() +
             (ref.offset & arena_->chunk_mask);
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

  void configure_chunk_bytes(size_type chunk_bytes) {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes) {
      chunk_bytes = kDefaultChunkBytes;
    }
    arena_->chunk_bytes = chunk_bytes;
    arena_->chunk_shift = static_cast<size_type>(std::countr_zero(chunk_bytes));
    arena_->chunk_mask = chunk_bytes - 1;
  }

  [[nodiscard]] std::uint32_t append_bytes(std::string_view member) {
    const auto r = reserve_run_bytes(
        arena_->member_chunks, arena_->member_next_offset,
        arena_->member_active_bytes, arena_->member_committed_bytes,
        arena_->chunk_bytes, arena_->chunk_shift, arena_->chunk_mask,
        arena_->growth, kInitialArenaBytes, member.size());
    if (r.dst != nullptr) {
      std::memcpy(r.dst, member.data(), member.size());
      arena_->member_used_bytes += member.size();
    }
    return r.offset;
  }

  [[nodiscard]] ScoreTextRef append_score_text(double score) {
    const auto text = score_format::format(score);
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset score text too large");
    }
    if (arena_->score_text_next_offset >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            text.size() - (arena_->chunk_bytes - 1)) {
      throw std::length_error("zset score text arena exhausted");
    }
    return append_score_text_bytes(text);
  }

  [[nodiscard]] ScoreTextRef append_score_text_bytes(std::string_view text) {
    const auto r = reserve_run_bytes(
        arena_->score_text_chunks, arena_->score_text_next_offset,
        arena_->score_text_active_bytes, arena_->score_text_committed_bytes,
        arena_->chunk_bytes, arena_->chunk_shift, arena_->chunk_mask,
        arena_->growth, kInitialArenaBytes, text.size());
    if (r.dst != nullptr) {
      std::memcpy(r.dst, text.data(), text.size());
      arena_->score_text_used_bytes += text.size();
    }
    return ScoreTextRef{.offset = r.offset,
                        .length = static_cast<std::uint32_t>(text.size())};
  }

  void set_score_text(std::uint32_t member_id, double score) {
    if (!score_string_cache_enabled_) {
      return;
    }
    assert(member_id < score_text_refs_.size());
    score_text_refs_[member_id] = append_score_text(score);
  }

  std::shared_ptr<ZSetMemberByteArena> arena_{std::make_shared<ZSetMemberByteArena>()};
  bool score_string_cache_enabled_{false};
  std::vector<std::uint32_t> offsets_;
  std::vector<std::uint16_t> lengths_;
  ScoreArray scores_;
  std::vector<ScoreTextRef> score_text_refs_;
};

}  // namespace goblin::core
