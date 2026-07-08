#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "goblin/core/compact_listpack.hpp"
#include "goblin/core/hash.hpp"
#include "goblin/core/key_arena.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/swiss_table.hpp"
#include "goblin/core/zset_member_index.hpp"
#include "goblin/core/zset_member_layer.hpp"
#include "goblin/core/zset_listpack.hpp"
#include "goblin/core/zset_member_storage.hpp"
#include "goblin/core/zset_score_index.hpp"

namespace goblin::core {

// Target member-index load factor used by compaction when no density is
// requested. 0.97 is essentially the table's steady-state max load (31/32),
// packing tight while still leaving empty slots so lookups of absent members
// terminate quickly (a fully packed 1.0 index makes misses scan O(n)).
inline constexpr double kDefaultMemberIndexDensity = 0.97;

// Result of Store::load.
struct SnapshotLoadStats {
  std::size_t keys{0};
  std::size_t members{0};
  bool used_accelerator{false};  // false if the canonical (slow) path was taken
};

struct ZSetEntry {
  std::string_view member;
  double score{0.0};
  std::string_view score_text;
};

struct ZSetRangeBounds {
  std::size_t first{0};
  std::size_t count{0};
};

struct ZSetOptions {
  RankCacheMode rank_cache_mode{RankCacheMode::Off};
  bool score_string_cache{false};
  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t member_chunk_bytes{ZSetMemberStorage::kDefaultChunkBytes};
  // Max entries a zset keeps in the compact listpack before promoting to the full
  // arena-shaped structure. 0 disables the listpack (always full) -- the current
  // default, until the full-structure tests and the shared-member-layer
  // optimization are reconciled with small mode.
  std::size_t listpack_max_entries{0};
};

struct ZSetMemoryStats {
  std::size_t member_count{0};
  RankCacheMode rank_cache_mode{RankCacheMode::Off};
  ScoreWidth score_width{ScoreWidth::I16};
  std::size_t member_storage_bytes{0};
  std::size_t member_storage_allocated_bytes{0};
  std::size_t member_ref_capacity{0};
  std::size_t score_string_cache_bytes{0};
  std::size_t score_string_cache_ref_capacity{0};
  std::size_t score_string_cache_allocated_bytes{0};
  std::size_t member_index_capacity{0};
  std::size_t member_index_member_slot_capacity{0};
  std::size_t member_index_tombstones{0};
  std::size_t member_index_allocated_bytes{0};
  std::size_t member_layer_share_count{0};
  std::size_t score_index_share_count{0};
  std::size_t score_entry_count{0};
  std::size_t score_block_count{0};
  std::size_t score_block_capacity_sum{0};
  std::size_t score_index_allocated_bytes{0};
  std::size_t rank_location_cache_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
};

class Store;

class ZSet {
 public:
  // Default: reference a shared static default-options object.
  ZSet();
  // The options object must outlive the zset. The store owns one and points every
  // zset at it (all zsets in a store share identical options), so a zset carries
  // an 8 B pointer, not a 32 B copy; standalone/test zsets pass a stable object.
  explicit ZSet(const ZSetOptions* options);

  ZSet(const ZSet&) = delete;
  ZSet& operator=(const ZSet&) = delete;

  ZSet(ZSet&& other) noexcept;
  ZSet& operator=(ZSet&& other) noexcept;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t block_count() const noexcept;
  [[nodiscard]] int add(double score, std::string_view member);
  [[nodiscard]] bool remove(std::string_view member);
  [[nodiscard]] std::optional<double> score(std::string_view member) const;
  [[nodiscard]] std::optional<std::size_t> rank(std::string_view member) const;
  [[nodiscard]] std::optional<std::size_t> reverse_rank(
      std::string_view member) const;
  [[nodiscard]] std::vector<ZSetEntry> range(long long start, long long stop) const;
  [[nodiscard]] std::vector<ZSetEntry> reverse_range(long long start,
                                                     long long stop) const;
  [[nodiscard]] std::optional<ZSetRangeBounds> range_bounds(
      long long start,
      long long stop) const noexcept;
  template <class Fn>
  std::size_t for_range_members(ZSetRangeBounds bounds, Fn&& fn) const {
    if (const auto* lp = small_ptr()) {
      lp->for_range(bounds.first, bounds.count, false,
                    [&fn](double, std::string_view member) { fn(member); });
      return bounds.count;
    }
    // Range iteration walks member ids in score order, which is scattered
    // relative to member storage id order. Software-prefetch the pointer chase
    // id -> location -> bytes two stages deep (location further ahead than the
    // packed bytes, so the byte prefetch reads a resident offset). Neutral for
    // small ranges, ~4% for large ones.
    const auto* storage = member_storage();
    auto span = [storage, &fn](const std::uint32_t* ids, std::size_t count) {
      constexpr std::size_t kLocationAhead = 10;
      constexpr std::size_t kBytesAhead = 5;
      for (std::size_t i = 0; i < count; ++i) {
        if (i + kLocationAhead < count) {
          storage->prefetch_location(ids[i + kLocationAhead]);
        }
        if (i + kBytesAhead < count) {
          storage->prefetch_bytes(ids[i + kBytesAhead]);
        }
        fn(storage->view(ids[i]));
      }
    };
    entries().for_member_id_spans(bounds.first, bounds.count, span);
    return bounds.count;
  }
  template <class Fn>
  std::size_t for_range_members(long long start, long long stop, Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_range_members(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t for_range_values(ZSetRangeBounds bounds, Fn&& fn) const {
    return for_position_range(
        bounds, false,
        [&fn](std::string_view member, double score, std::string_view) {
          fn(member, score);
        });
  }
  template <class Fn>
  std::size_t for_range_values(long long start, long long stop, Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_range_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t for_range_score_text_values(ZSetRangeBounds bounds,
                                          Fn&& fn) const {
    return for_position_range(
        bounds, false,
        [&fn](std::string_view member, double, std::string_view score_text) {
          fn(member, score_text);
        });
  }
  template <class Fn>
  std::size_t for_range_score_text_values(long long start,
                                          long long stop,
                                          Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_range_score_text_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t for_range(long long start, long long stop, Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_position_range(
        *bounds, false,
        [&fn](std::string_view member, double score, std::string_view) {
          fn(ZSetEntry{.member = member, .score = score, .score_text = {}});
        });
  }
  template <class Fn>
  std::size_t for_range_with_score_text(long long start,
                                        long long stop,
                                        Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }
    return for_position_range(
        *bounds, false,
        [&fn](std::string_view member, double score,
              std::string_view score_text) {
          fn(ZSetEntry{
              .member = member, .score = score, .score_text = score_text});
        });
  }
  template <class Fn>
  std::size_t for_reverse_range_members(ZSetRangeBounds bounds, Fn&& fn) const {
    if (const auto* lp = small_ptr()) {
      lp->for_range(bounds.first, bounds.count, true,
                    [&fn](double, std::string_view member) { fn(member); });
      return bounds.count;
    }
    auto callback = [this, &fn](std::uint32_t member_id) {
      fn(member_view(member_id));
    };
    entries().for_reverse_member_ids(bounds.first, bounds.count, callback);
    return bounds.count;
  }
  template <class Fn>
  std::size_t for_reverse_range_members(long long start,
                                        long long stop,
                                        Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_reverse_range_members(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t for_reverse_range_values(ZSetRangeBounds bounds, Fn&& fn) const {
    return for_position_range(
        bounds, true,
        [&fn](std::string_view member, double score, std::string_view) {
          fn(member, score);
        });
  }
  template <class Fn>
  std::size_t for_reverse_range_values(long long start,
                                       long long stop,
                                       Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_reverse_range_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t for_reverse_range_score_text_values(ZSetRangeBounds bounds,
                                                  Fn&& fn) const {
    return for_position_range(
        bounds, true,
        [&fn](std::string_view member, double, std::string_view score_text) {
          fn(member, score_text);
        });
  }
  template <class Fn>
  std::size_t for_reverse_range_score_text_values(long long start,
                                                  long long stop,
                                                  Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_reverse_range_score_text_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t for_reverse_range(long long start, long long stop, Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    return for_position_range(
        *bounds, true,
        [&fn](std::string_view member, double score, std::string_view) {
          fn(ZSetEntry{.member = member, .score = score, .score_text = {}});
        });
  }
  template <class Fn>
  std::size_t for_reverse_range_with_score_text(long long start,
                                                long long stop,
                                                Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }
    return for_position_range(
        *bounds, true,
        [&fn](std::string_view member, double score,
              std::string_view score_text) {
          fn(ZSetEntry{
              .member = member, .score = score, .score_text = score_text});
        });
  }
  [[nodiscard]] bool check_invariants() const;
  [[nodiscard]] ZSetMemoryStats memory_stats() const noexcept;
  void compact(double member_index_density = kDefaultMemberIndexDensity);

  // Serialize this zset as the operands of an OP_ZSET instruction: options,
  // canonical members, and (when with_accelerator) the index accelerator.
  void save(snapshot::Writer& writer, bool with_accelerator) const;
  // Reconstruct from OP_ZSET operands. use_accelerator says to trust a present
  // accelerator (version matched) rather than rebuild from the canonical layer.
  [[nodiscard]] static ZSet load(snapshot::Reader& reader, bool use_accelerator,
                                 const ZSetOptions* options);

  [[nodiscard]] std::size_t allocated_member_slots() const noexcept;
  [[nodiscard]] std::size_t free_member_slots() const noexcept;
  [[nodiscard]] std::size_t member_index_capacity() const noexcept;
  [[nodiscard]] std::size_t member_index_tombstones() const noexcept;
  [[nodiscard]] bool should_compact_after_removal(std::size_t removed_count) const noexcept;
  bool cleanup_member_index_after_removal_if_needed(std::size_t removed_count);
  bool rehash_member_index_same_capacity();
  bool compact_after_removal_if_needed(std::size_t removed_count);

  friend class Store;

 private:
  [[nodiscard]] std::uint32_t allocate_member_id(std::string_view member,
                                                 double score);
  [[nodiscard]] std::string_view member_view(std::uint32_t member_id) const noexcept;
  [[nodiscard]] std::string_view score_text_view(std::uint32_t member_id) const noexcept;
  void rebind_indexes() noexcept;

  enum class WriteKind {
    ScoreUpdate,
    Structural,
  };

  void ensure_unique_mutable_state(WriteKind kind);
  void adopt_shared_member_layer_from(const ZSet& source);

  // A zset is EITHER a compact listpack (tiny) OR the full arena-shaped structure
  // (heap-allocated). Both alternatives are a single pointer, so sizeof(ZSet) is
  // tiny -- the store keeps it by value in the swiss slot as (key + pointer),
  // not a 48 B inline handle. The full state lives behind a unique_ptr so a tiny
  // zset costs nothing for it.
  struct FullState {
    std::shared_ptr<ZSetMemberLayer> member_layer;
    std::shared_ptr<ZSetScoreIndex> score_index;
  };
  [[nodiscard]] bool is_small() const noexcept {
    return std::holds_alternative<CompactListpack>(rep_);
  }
  [[nodiscard]] CompactListpack* small_ptr() noexcept {
    return std::get_if<CompactListpack>(&rep_);
  }
  [[nodiscard]] const CompactListpack* small_ptr() const noexcept {
    return std::get_if<CompactListpack>(&rep_);
  }
  [[nodiscard]] FullState& full() noexcept {
    return *std::get<std::unique_ptr<FullState>>(rep_);
  }
  [[nodiscard]] const FullState& full() const noexcept {
    return *std::get<std::unique_ptr<FullState>>(rep_);
  }
  void init_empty();  // set rep_ to a fresh empty listpack-or-full per options_
  void ensure_full();

  // The one iteration primitive the range templates funnel through: emit positions
  // [bounds.first, +count) (forward or reverse) as (member, score, score_text).
  // Dispatches on small vs full; in small mode score_text is empty (formatted on
  // demand by the RESP layer, as when the score-text cache is off).
  template <class Fn>
  std::size_t for_position_range(ZSetRangeBounds bounds, bool reverse,
                                 Fn&& fn) const {
    if (const auto* lp = small_ptr()) {
      lp->for_range(bounds.first, bounds.count, reverse,
                    [&fn](double score, std::string_view member) {
                      fn(member, score, std::string_view{});
                    });
      return bounds.count;
    }
    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(member_view(member_id), score, score_text_view(member_id));
    };
    if (reverse) {
      entries().for_reverse_range(bounds.first, bounds.count, callback);
    } else {
      entries().for_range(bounds.first, bounds.count, callback);
    }
    return bounds.count;
  }

  [[nodiscard]] ZSetMemberStorage* member_storage() noexcept;
  [[nodiscard]] const ZSetMemberStorage* member_storage() const noexcept;
  [[nodiscard]] ZSetMemberIndex& members() noexcept;
  [[nodiscard]] const ZSetMemberIndex& members() const noexcept;
  [[nodiscard]] ZSetScoreIndex& entries() noexcept;
  [[nodiscard]] const ZSetScoreIndex& entries() const noexcept;

  std::variant<CompactListpack, std::unique_ptr<FullState>> rep_;
  const ZSetOptions* options_;
};

struct StoreOptions {
  RankCacheMode rank_cache_mode{RankCacheMode::Off};
  bool score_string_cache{false};
  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t zset_chunk_bytes{ZSetMemberStorage::kDefaultChunkBytes};
  std::size_t hash_chunk_bytes{HashStorage::kDefaultChunkBytes};
  // Max entries a zset keeps as a compact listpack before promoting to the full
  // arena-shaped structure (0 disables the listpack). Tiny zsets live as one blob
  // (~1.5x leaner per zset with distinct members). 32 is the CPU knee: memory
  // saving is ~flat with size, but the O(n) blob scan makes ZSCORE ~2.5x at 32
  // and perverse (>6x) by 128, so we promote to the O(log n) structure there.
  std::size_t zset_listpack_max_entries{32};
  // Zsets created before the overflow table are kept in a small inline table
  // for fast key resolution on multi-key workloads.
  std::size_t inline_zset_limit{32};
};

struct StoreMemoryStats {
  std::size_t inline_zset_count{0};
  std::size_t overflow_zset_count{0};
  std::size_t overflow_zset_capacity{0};
  std::size_t overflow_table_allocated_bytes{0};
  std::size_t inline_zset_index_allocated_bytes{0};
  std::size_t inline_zset_allocated_bytes{0};
  std::size_t overflow_zset_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
};

class Store {
 public:
  explicit Store(StoreOptions options = {});

  [[nodiscard]] long long zadd(std::string_view key, double score, std::string_view member);
  [[nodiscard]] long long zrem(std::string_view key, std::span<const std::string_view> members);
  [[nodiscard]] long long zcard(std::string_view key) const;
  [[nodiscard]] std::optional<double> zscore(std::string_view key,
                                             std::string_view member) const;
  [[nodiscard]] std::optional<std::size_t> zrank(std::string_view key,
                                                 std::string_view member) const;
  [[nodiscard]] std::optional<std::size_t> zrevrank(
      std::string_view key,
      std::string_view member) const;
  [[nodiscard]] std::vector<ZSetEntry> zrange(std::string_view key,
                                              long long start,
                                              long long stop) const;
  [[nodiscard]] std::vector<ZSetEntry> zrevrange(std::string_view key,
                                                long long start,
                                                long long stop) const;
  [[nodiscard]] bool score_string_cache_enabled() const noexcept {
    return options_.score_string_cache;
  }
  [[nodiscard]] std::size_t zrange_size(std::string_view key,
                                        long long start,
                                        long long stop) const;
  template <class CountFn, class Fn>
  std::size_t zrange_members_for_each_counted(std::string_view key,
                                              long long start,
                                              long long stop,
                                              CountFn&& count_fn,
                                              Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    const auto bounds = zset->range_bounds(start, stop);
    if (!bounds) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    std::forward<CountFn>(count_fn)(bounds->count);
    return zset->for_range_members(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrange_values_for_each(std::string_view key,
                                     long long start,
                                     long long stop,
                                     Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_range_values(start, stop, std::forward<Fn>(fn));
  }
  template <class CountFn, class Fn>
  std::size_t zrange_values_for_each_counted(std::string_view key,
                                             long long start,
                                             long long stop,
                                             CountFn&& count_fn,
                                             Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    const auto bounds = zset->range_bounds(start, stop);
    if (!bounds) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    std::forward<CountFn>(count_fn)(bounds->count);
    return zset->for_range_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrange_score_text_values_for_each(std::string_view key,
                                                long long start,
                                                long long stop,
                                                Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_range_score_text_values(
        start, stop, std::forward<Fn>(fn));
  }
  template <class CountFn, class Fn>
  std::size_t zrange_score_text_values_for_each_counted(std::string_view key,
                                                        long long start,
                                                        long long stop,
                                                        CountFn&& count_fn,
                                                        Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    const auto bounds = zset->range_bounds(start, stop);
    if (!bounds) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    std::forward<CountFn>(count_fn)(bounds->count);
    return zset->for_range_score_text_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrange_for_each(std::string_view key,
                              long long start,
                              long long stop,
                              Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_range(start, stop, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrange_for_each_with_score_text(std::string_view key,
                                              long long start,
                                              long long stop,
                                              Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_range_with_score_text(start, stop, std::forward<Fn>(fn));
  }
  [[nodiscard]] std::size_t zrevrange_size(std::string_view key,
                                           long long start,
                                           long long stop) const;
  template <class CountFn, class Fn>
  std::size_t zrevrange_members_for_each_counted(std::string_view key,
                                                 long long start,
                                                 long long stop,
                                                 CountFn&& count_fn,
                                                 Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    const auto bounds = zset->range_bounds(start, stop);
    if (!bounds) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    std::forward<CountFn>(count_fn)(bounds->count);
    return zset->for_reverse_range_members(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrevrange_values_for_each(std::string_view key,
                                        long long start,
                                        long long stop,
                                        Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_reverse_range_values(start, stop, std::forward<Fn>(fn));
  }
  template <class CountFn, class Fn>
  std::size_t zrevrange_values_for_each_counted(std::string_view key,
                                                long long start,
                                                long long stop,
                                                CountFn&& count_fn,
                                                Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    const auto bounds = zset->range_bounds(start, stop);
    if (!bounds) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    std::forward<CountFn>(count_fn)(bounds->count);
    return zset->for_reverse_range_values(*bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrevrange_score_text_values_for_each(std::string_view key,
                                                   long long start,
                                                   long long stop,
                                                   Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_reverse_range_score_text_values(
        start, stop, std::forward<Fn>(fn));
  }
  template <class CountFn, class Fn>
  std::size_t zrevrange_score_text_values_for_each_counted(
      std::string_view key,
      long long start,
      long long stop,
      CountFn&& count_fn,
      Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    const auto bounds = zset->range_bounds(start, stop);
    if (!bounds) {
      std::forward<CountFn>(count_fn)(0);
      return 0;
    }

    std::forward<CountFn>(count_fn)(bounds->count);
    return zset->for_reverse_range_score_text_values(
        *bounds, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrevrange_for_each(std::string_view key,
                                 long long start,
                                 long long stop,
                                 Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_reverse_range(start, stop, std::forward<Fn>(fn));
  }
  template <class Fn>
  std::size_t zrevrange_for_each_with_score_text(std::string_view key,
                                                 long long start,
                                                 long long stop,
                                                 Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }

    return zset->for_reverse_range_with_score_text(
        start, stop, std::forward<Fn>(fn));
  }
  [[nodiscard]] std::optional<ZSetMemoryStats> zset_memory_stats(
      std::string_view key) const;

  // --- Hash (field -> value) ---
  // A key holds at most one type. A hash command on a zset key (or vice versa)
  // is a WRONGTYPE error; the command layer gates on these before operating.
  // Both short-circuit when the other type's keyspace is empty.
  [[nodiscard]] bool key_is_zset(std::string_view key) const noexcept {
    return find_zset(key) != nullptr;
  }
  [[nodiscard]] bool key_is_hash(std::string_view key) const noexcept {
    return find_hash(key) != nullptr;
  }

  [[nodiscard]] int hset(std::string_view key, std::string_view field,
                         std::string_view value);
  [[nodiscard]] int hsetnx(std::string_view key, std::string_view field,
                           std::string_view value);
  [[nodiscard]] std::optional<std::string_view> hget(
      std::string_view key, std::string_view field) const;
  [[nodiscard]] bool hexists(std::string_view key, std::string_view field) const;
  [[nodiscard]] bool hdel(std::string_view key, std::string_view field);
  [[nodiscard]] std::size_t hlen(std::string_view key) const;
  [[nodiscard]] std::optional<std::size_t> hstrlen(std::string_view key,
                                                   std::string_view field) const;
  [[nodiscard]] std::optional<long long> hincrby(std::string_view key,
                                                 std::string_view field,
                                                 long long delta);
  template <class Fn>
  void hash_for_each(std::string_view key, Fn&& fn) const {
    const auto* hash = find_hash(key);
    if (hash != nullptr) {
      hash->for_each(std::forward<Fn>(fn));
    }
  }
  [[nodiscard]] std::optional<HashMemoryStats> hash_memory_stats(
      std::string_view key) const;

  [[nodiscard]] StoreMemoryStats memory_stats() const noexcept;
  // Compact a zset in place to reclaim insertion slack (block capacity, vector
  // over-allocation) and repack the member index to `member_index_density`.
  // Returns the number of allocated bytes reclaimed, or nullopt if the key does
  // not exist.
  [[nodiscard]] std::optional<std::size_t> optimize(
      std::string_view key,
      double member_index_density = kDefaultMemberIndexDensity);

  // Write every zset to `out` as a snapshot (see snapshot.hpp). With
  // with_accelerator=false the packed indexes are omitted -- a smaller,
  // portable, canonical-only file that rebuilds its indexes on load. Throws
  // snapshot_error on I/O trouble.
  void save(std::ostream& out, bool with_accelerator = true) const;

  // Fork a copy-on-write child that writes the snapshot to `path` (to a temp
  // file renamed into place on success), so the server keeps serving while it
  // saves. Returns immediately; call reap_background_save() from the event loop
  // to collect the result. Only one background save runs at a time.
  enum class SaveStart { Started, AlreadyRunning, ForkFailed };
  [[nodiscard]] SaveStart start_background_save(std::string path,
                                                bool with_accelerator = true);

  // Non-blocking: if the background save has finished, reap it and return its
  // outcome (clearing the in-progress state); otherwise return nullopt.
  struct SaveOutcome {
    std::string path;
    bool ok;
  };
  [[nodiscard]] std::optional<SaveOutcome> reap_background_save() noexcept;
  [[nodiscard]] bool background_save_in_progress() const noexcept {
    return background_save_child_ > 0;
  }

  // Replace all current data with the snapshot read from `in`. Auto-detects a
  // native Goblin snapshot ("GCSN") or a Redis RDB file ("REDIS") by magic. On
  // any error (bad magic, checksum mismatch, truncation, unsupported encoding)
  // throws and leaves the store empty. Requires a seekable stream.
  SnapshotLoadStats load(std::istream& in);

  // Remove every key.
  void clear() noexcept;

 private:
  SnapshotLoadStats load_native(std::istream& in);
  void place_loaded_zset(std::string key, ZSet&& zset);
  [[nodiscard]] ZSet* find_zset(std::string_view key) noexcept;
  [[nodiscard]] const ZSet* find_zset(std::string_view key) const noexcept;
  [[nodiscard]] ZSet& get_or_create_zset(std::string_view key);
  // The one options object every zset in this store shares (built lazily from
  // options_). All store zsets have identical options, so they hold a shared_ptr
  // to this rather than each copying a ZSetOptions.
  [[nodiscard]] const ZSetOptions* zset_options();
  [[nodiscard]] const ZSet* find_member_layer_template() const noexcept;
  void erase_if_empty(std::string_view key, const ZSet& zset);
  // Rebuild the zset key arena + overflow table once deleted keys dominate it.
  void compact_zset_keys_if_needed();

  void place_loaded_hash(std::string key, Hash&& hash);
  [[nodiscard]] Hash* find_hash(std::string_view key) noexcept;
  [[nodiscard]] const Hash* find_hash(std::string_view key) const noexcept;
  [[nodiscard]] Hash& get_or_create_hash(std::string_view key);
  void erase_if_empty(std::string_view key, const Hash& hash);

  struct InlineZsetSlot {
    std::string key;
    ZSet zset;
  };

  [[nodiscard]] ZSet* find_inline_zset(std::string_view key) noexcept;
  [[nodiscard]] const ZSet* find_inline_zset(std::string_view key) const noexcept;
  [[nodiscard]] bool inline_zset_slots_full() const noexcept;
  [[nodiscard]] ZSet& emplace_inline_zset(std::string_view key);
  void erase_inline_zset_if(std::string_view key, const ZSet& zset) noexcept;

  std::vector<InlineZsetSlot> inline_zsets_;
  SwissTable<std::string, std::size_t, StringTableHash, StringTableEqual>
      inline_zset_index_;
  // Overflow zset keys live packed in this arena; the swiss keys are bare uint64
  // offsets into it (fixed-size slots, no per-key std::string / malloc churn).
  KeyArena zset_key_arena_;
  SwissTable<std::uint64_t, ZSet, KeyArenaHash, KeyArenaEqual> overflow_zsets_;
  std::optional<Hash> inline_hash_;
  std::string inline_hash_key_;
  SwissTable<std::string, Hash, StringTableHash, StringTableEqual> overflow_hashes_;
  StoreOptions options_;
  ZSetOptions zset_options_;  // the one options object every store zset points at
  bool zset_options_ready_ = false;
  int background_save_child_ = -1;  // pid of an in-flight fork(), or -1
  std::string background_save_path_;
};

[[nodiscard]] std::string format_score(double score);

}  // namespace goblin::core
