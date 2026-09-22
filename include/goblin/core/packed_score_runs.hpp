#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "goblin/core/memory_limit.hpp"

namespace goblin::core::detail {

// The sorted half of an RLE leaf has one key per logical slot and a score
// stream of literals or [NaN, integer count, score]. Counts occupy one score
// word, so runs of four or more save space for both binary32 and binary64.
// The separately reserved dirty tail retains ordinary Entry/NaN tombstones.
template <class Entry, std::size_t Capacity>
struct PackedScoreRuns {
  using Score = decltype(Entry::score);
  using Key = decltype(Entry::key);
  using Word = std::conditional_t<sizeof(Score) == 4, std::uint32_t,
                                 std::uint64_t>;
  static constexpr std::size_t kStride = 32;
  static_assert(Capacity < std::numeric_limits<std::uint16_t>::max());

  struct Checkpoint {
    std::uint16_t word{0};
    std::uint16_t first{0};
  };

  std::vector<Key> keys;
  std::vector<Word> scores;
  std::vector<Entry> dirty;
  std::array<Checkpoint, (Capacity + kStride - 1) / kStride> checkpoints{};
  bool encoded{false};

  PackedScoreRuns() = default;
  PackedScoreRuns(const PackedScoreRuns& other)
      : checkpoints(other.checkpoints), encoded(other.encoded) {
    // A copied dirty leaf must retain room for the next mutation/merge.
    reserve_memory_vector(keys, other.keys.capacity());
    reserve_memory_vector(scores, other.scores.capacity());
    reserve_memory_vector(dirty, other.dirty.capacity());
    keys.assign(other.keys.begin(), other.keys.end());
    scores.assign(other.scores.begin(), other.scores.end());
    dirty.assign(other.dirty.begin(), other.dirty.end());
  }
  PackedScoreRuns& operator=(const PackedScoreRuns& other) {
    if (this != &other) {
      PackedScoreRuns copy(other);
      *this = std::move(copy);
    }
    return *this;
  }
  PackedScoreRuns(PackedScoreRuns&&) noexcept = default;
  PackedScoreRuns& operator=(PackedScoreRuns&&) noexcept = default;

  void reserve(std::size_t threshold) {
    reserve_memory_vector(keys, Capacity);
    reserve_memory_vector(dirty, threshold);
    reserve_memory_vector(scores, std::min(Capacity, threshold + 3));
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return keys.capacity() * sizeof(Key) + scores.capacity() * sizeof(Word) +
           dirty.capacity() * sizeof(Entry);
  }

  void clear() noexcept {
    keys.clear();
    scores.clear();
    dirty.clear();
    encoded = false;
  }

  // Removing scores cannot increase this encoding's length. Each addition
  // adds at most one word, including the three-literal -> four-item run case.
  // Reserve before publishing a mutation; subsequent compaction cannot fail.
  void prepare_merge(std::size_t additions) {
    reserve_words(std::min(Capacity, scores.size() + additions));
  }

  void reserve_words(std::size_t count) {
    if (count > scores.capacity()) {
      reserve_memory_vector(scores,
          std::min(Capacity, std::max(count, scores.capacity() * 2)));
    }
  }

  [[nodiscard]] static std::size_t word_count(std::span<const Entry> values) {
    std::size_t words = 0;
    for (std::size_t first = 0; first < values.size();) {
      auto end = first + 1;
      while (end < values.size() && values[end].score == values[first].score) {
        ++end;
      }
      words += std::min<std::size_t>(3, end - first);
      first = end;
    }
    return words;
  }

  void assign(std::span<const Entry> values) noexcept {
    const auto words = word_count(values);
    assert(values.size() <= keys.capacity() && words <= scores.capacity());
    keys.resize(values.size());
    scores.resize(words);
    encoded = words < values.size();
    std::size_t word = 0;
    for (std::size_t first = 0; first < values.size();) {
      auto end = first + 1;
      while (end < values.size() && values[end].score == values[first].score) {
        ++end;
      }
      for (auto i = first; i < end; ++i) keys[i] = values[i].key;
      if (end - first >= 4) {
        checkpoint_run(first, end, word);
        scores[word++] = std::bit_cast<Word>(
            std::numeric_limits<Score>::quiet_NaN());
        scores[word++] = static_cast<Word>(end - first);
        scores[word++] = std::bit_cast<Word>(values[first].score);
      } else {
        for (auto i = first; i < end; ++i) {
          checkpoint_run(i, i + 1, word);
          scores[word++] = std::bit_cast<Word>(values[i].score);
        }
      }
      first = end;
    }
    assert(word == words);
    dirty.clear();
  }

  // Reclaim a formerly diverse score stream after it becomes compressible.
  // Shrinking is optional maintenance and cannot reject a committed write.
  void maybe_shrink(std::size_t threshold) noexcept {
    const auto target = std::min(Capacity, scores.size() + threshold);
    if (target >= scores.capacity() / 2) return;
    try {
      std::vector<Word> replacement;
      reserve_memory_vector(replacement, target);
      replacement.assign(scores.begin(), scores.end());
      scores.swap(replacement);
    } catch (const std::bad_alloc&) {
    }
  }

  [[nodiscard]] Entry entry(std::size_t slot) const noexcept {
    assert(slot < keys.size());
    if (!encoded) return {std::bit_cast<Score>(scores[slot]), keys[slot]};
    const auto checkpoint = checkpoints[slot / kStride];
    std::size_t word = checkpoint.word;
    std::size_t first = checkpoint.first;
    for (;;) {
      const auto value = std::bit_cast<Score>(scores[word]);
      if (std::isnan(value)) {
        const auto count = static_cast<std::size_t>(scores[word + 1]);
        if (slot < first + count) {
          return {std::bit_cast<Score>(scores[word + 2]), keys[slot]};
        }
        first += count;
        word += 3;
      } else {
        if (first == slot) return {value, keys[slot]};
        ++first;
        ++word;
      }
    }
  }

  // Sequential scans decode each run once, including when invalidation bits
  // skip members in its middle. Point probes use the checkpoints above.
  struct Cursor {
    const PackedScoreRuns& base;
    std::size_t word{0};
    std::size_t end{0};
    Score score{};

    [[nodiscard]] Entry entry(std::size_t slot) noexcept {
      if (!base.encoded) return base.entry(slot);
      while (end <= slot) {
        score = std::bit_cast<Score>(base.scores[word++]);
        if (std::isnan(score)) {
          end += static_cast<std::size_t>(base.scores[word++]);
          score = std::bit_cast<Score>(base.scores[word++]);
        } else {
          ++end;
        }
      }
      return {score, base.keys[slot]};
    }
  };

  [[nodiscard]] bool check_invariants(std::size_t size) const noexcept {
    if (keys.size() != size || size > Capacity) return false;
    std::size_t slot = 0;
    for (std::size_t word = 0; word < scores.size();) {
      const auto start_word = word;
      const auto value = std::bit_cast<Score>(scores[word++]);
      std::size_t count = 1;
      if (std::isnan(value)) {
        if (!encoded || word + 1 >= scores.size()) return false;
        count = static_cast<std::size_t>(scores[word++]);
        if (count < 4 || count > Capacity ||
            std::isnan(std::bit_cast<Score>(scores[word++]))) return false;
      }
      if (count > size - std::min(size, slot)) return false;
      for (auto i = ((slot + kStride - 1) / kStride) * kStride;
           i < slot + count; i += kStride) {
        if (checkpoints[i / kStride].word != start_word ||
            checkpoints[i / kStride].first != slot) return false;
      }
      slot += count;
    }
    return slot == size && encoded == (scores.size() < size);
  }

 private:
  void checkpoint_run(std::size_t first, std::size_t end,
                      std::size_t word) noexcept {
    for (auto i = ((first + kStride - 1) / kStride) * kStride;
         i < end; i += kStride) {
      checkpoints[i / kStride] = {static_cast<std::uint16_t>(word),
                                  static_cast<std::uint16_t>(first)};
    }
  }
};

}  // namespace goblin::core::detail
