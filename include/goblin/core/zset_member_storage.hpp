#pragma once

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

  struct Ref {
    std::uint32_t offset{0};
    std::uint32_t length{0};
    double score{0.0};
  };
  static_assert(sizeof(Ref) == 16);

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

  explicit ZSetMemberStorage(bool score_string_cache)
      : score_string_cache_enabled_(score_string_cache) {}

  [[nodiscard]] bool empty() const noexcept {
    return refs_.empty();
  }

  [[nodiscard]] size_type size() const noexcept {
    return refs_.size();
  }

  [[nodiscard]] size_type byte_size() const noexcept {
    return used_bytes_;
  }

  [[nodiscard]] size_type byte_capacity() const noexcept {
    return chunks_.size() * kChunkBytes;
  }

  [[nodiscard]] size_type ref_capacity() const noexcept {
    return refs_.capacity();
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return byte_capacity() +
           refs_.capacity() * sizeof(Ref) +
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
    return score_text_chunks_.size() * kChunkBytes;
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
    refs_.reserve(member_count);
    if (score_string_cache_enabled_) {
      score_text_refs_.reserve(member_count);
    }
  }

  void reserve_bytes(size_type byte_count) {
    chunks_.reserve((byte_count + kChunkBytes - 1) / kChunkBytes);
  }

  void reserve_score_text_bytes(size_type byte_count) {
    if (score_string_cache_enabled_) {
      score_text_chunks_.reserve((byte_count + kChunkBytes - 1) / kChunkBytes);
    }
  }

  [[nodiscard]] std::uint32_t push_back(std::string_view member, double score) {
    if (refs_.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member id space exhausted");
    }
    if (member.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member too large");
    }
    if (member.size() > kChunkBytes) {
      throw std::length_error("zset member too large for arena chunk");
    }
    if (next_offset_ >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            member.size() - (kChunkBytes - 1)) {
      throw std::length_error("zset member arena exhausted");
    }

    const auto id = static_cast<std::uint32_t>(refs_.size());
    const auto offset = append_bytes(member);
    refs_.push_back(Ref{.offset = offset,
                        .length = static_cast<std::uint32_t>(member.size()),
                        .score = score});
    if (score_string_cache_enabled_) {
      score_text_refs_.push_back(append_score_text(score));
    }
    return id;
  }

  void replace(std::uint32_t member_id, std::string_view member, double score) {
    assert(member_id < refs_.size());
    if (member.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member too large");
    }
    if (member.size() > kChunkBytes) {
      throw std::length_error("zset member too large for arena chunk");
    }
    if (next_offset_ >
        static_cast<size_type>(std::numeric_limits<std::uint32_t>::max()) -
            member.size() - (kChunkBytes - 1)) {
      throw std::length_error("zset member arena exhausted");
    }

    const auto offset = append_bytes(member);
    refs_[member_id] = Ref{.offset = offset,
                           .length = static_cast<std::uint32_t>(member.size()),
                           .score = score};
    set_score_text(member_id, score);
  }

  [[nodiscard]] Ref ref(std::uint32_t member_id) const noexcept {
    assert(member_id < refs_.size());
    return refs_[member_id];
  }

  [[nodiscard]] Snapshot snapshot(std::uint32_t member_id) const noexcept {
    assert(member_id < refs_.size());
    return Snapshot{
        .ref = refs_[member_id],
        .score_text = score_string_cache_enabled_ ? score_text_refs_[member_id]
                                                  : ScoreTextRef{},
    };
  }

  void restore_snapshot(std::uint32_t member_id, Snapshot snapshot) noexcept {
    assert(member_id < refs_.size());
    refs_[member_id] = snapshot.ref;
    if (score_string_cache_enabled_) {
      assert(member_id < score_text_refs_.size());
      score_text_refs_[member_id] = snapshot.score_text;
    }
  }

  void set_ref(std::uint32_t member_id, Ref ref) {
    assert(member_id < refs_.size());
    refs_[member_id] = ref;
    set_score_text(member_id, ref.score);
  }

  void copy_ref(std::uint32_t dst_member_id, std::uint32_t src_member_id) noexcept {
    assert(dst_member_id < refs_.size());
    assert(src_member_id < refs_.size());
    refs_[dst_member_id] = refs_[src_member_id];
    if (score_string_cache_enabled_) {
      assert(dst_member_id < score_text_refs_.size());
      assert(src_member_id < score_text_refs_.size());
      score_text_refs_[dst_member_id] = score_text_refs_[src_member_id];
    }
  }

  [[nodiscard]] double score(std::uint32_t member_id) const noexcept {
    assert(member_id < refs_.size());
    return refs_[member_id].score;
  }

  void set_score(std::uint32_t member_id, double score) {
    assert(member_id < refs_.size());
    refs_[member_id].score = score;
    set_score_text(member_id, score);
  }

  void pop_back() noexcept {
    assert(!refs_.empty());
    refs_.pop_back();
    if (score_string_cache_enabled_) {
      assert(!score_text_refs_.empty());
      score_text_refs_.pop_back();
    }
  }

  [[nodiscard]] std::string_view view(std::uint32_t member_id) const noexcept {
    assert(member_id < refs_.size());
    const auto ref = refs_[member_id];
    const char* data = "";
    if (ref.length != 0) {
      data = chunks_[ref.offset >> kChunkShift].get() + (ref.offset & kChunkMask);
    }
    return std::string_view(data, ref.length);
  }

  [[nodiscard]] std::string_view score_text(std::uint32_t member_id) const noexcept {
    if (!score_string_cache_enabled_) {
      return {};
    }
    assert(member_id < score_text_refs_.size());
    const auto ref = score_text_refs_[member_id];
    const char* data = "";
    if (ref.length != 0) {
      data = score_text_chunks_[ref.offset >> kChunkShift].get() +
             (ref.offset & kChunkMask);
    }
    return std::string_view(data, ref.length);
  }

 private:
  static constexpr size_type kChunkShift = 20;
  static constexpr size_type kChunkBytes = size_type{1} << kChunkShift;
  static constexpr size_type kChunkMask = kChunkBytes - 1;

  [[nodiscard]] std::uint32_t append_bytes(std::string_view member) {
    if (member.empty()) {
      return static_cast<std::uint32_t>(next_offset_);
    }

    auto chunk_offset = next_offset_ & kChunkMask;
    if (chunks_.empty() || chunk_offset + member.size() > kChunkBytes) {
      if (chunk_offset != 0) {
        next_offset_ += kChunkBytes - chunk_offset;
      }
      chunks_.push_back(std::make_unique_for_overwrite<char[]>(kChunkBytes));
      chunk_offset = 0;
    }

    const auto offset = static_cast<std::uint32_t>(next_offset_);
    std::memcpy(chunks_[offset >> kChunkShift].get() + chunk_offset,
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
            text.size() - (kChunkBytes - 1)) {
      throw std::length_error("zset score text arena exhausted");
    }
    return append_score_text_bytes(text);
  }

  [[nodiscard]] ScoreTextRef append_score_text_bytes(std::string_view text) {
    if (text.empty()) {
      return ScoreTextRef{.offset = static_cast<std::uint32_t>(score_text_next_offset_),
                          .length = 0};
    }

    auto chunk_offset = score_text_next_offset_ & kChunkMask;
    if (score_text_chunks_.empty() || chunk_offset + text.size() > kChunkBytes) {
      if (chunk_offset != 0) {
        score_text_next_offset_ += kChunkBytes - chunk_offset;
      }
      score_text_chunks_.push_back(std::make_unique_for_overwrite<char[]>(kChunkBytes));
      chunk_offset = 0;
    }

    const auto offset = static_cast<std::uint32_t>(score_text_next_offset_);
    std::memcpy(score_text_chunks_[offset >> kChunkShift].get() + chunk_offset,
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
  std::vector<Ref> refs_;
  std::vector<std::unique_ptr<char[]>> score_text_chunks_;
  std::vector<ScoreTextRef> score_text_refs_;
  size_type next_offset_{0};
  size_type used_bytes_{0};
  size_type score_text_next_offset_{0};
  size_type score_text_used_bytes_{0};
  bool score_string_cache_enabled_{false};
};

}  // namespace goblin::core
