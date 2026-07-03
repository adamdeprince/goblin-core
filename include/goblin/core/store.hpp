#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/swiss_table.hpp"
#include "goblin/core/zset_member_index.hpp"
#include "goblin/core/zset_member_storage.hpp"
#include "goblin/core/zset_score_index.hpp"

namespace goblin::core {

// Target member-index load factor used by compaction when no density is
// requested. 0.97 is essentially the table's steady-state max load (31/32),
// packing tight while still leaving empty slots so lookups of absent members
// terminate quickly (a fully packed 1.0 index makes misses scan O(n)).
inline constexpr double kDefaultMemberIndexDensity = 0.97;

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
};

struct ZSetMemoryStats {
  std::size_t member_count{0};
  RankCacheMode rank_cache_mode{RankCacheMode::Off};
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
  std::size_t score_entry_count{0};
  std::size_t score_block_count{0};
  std::size_t score_block_capacity_sum{0};
  std::size_t score_index_allocated_bytes{0};
  std::size_t rank_location_cache_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
};

class ZSet {
 public:
  explicit ZSet(ZSetOptions options = {});

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
    // Range iteration walks member ids in score order, which is scattered
    // relative to member storage id order. Software-prefetch the pointer chase
    // id -> location -> bytes two stages deep (location further ahead than the
    // packed bytes, so the byte prefetch reads a resident offset). Neutral for
    // small ranges, ~4% for large ones.
    const auto* storage = member_storage_.get();
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
    entries_.for_member_id_spans(bounds.first, bounds.count, span);
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
    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(member_view(member_id), score);
    };
    entries_.for_range(bounds.first, bounds.count, callback);
    return bounds.count;
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
    auto callback = [this, &fn](double, std::uint32_t member_id) {
      fn(member_view(member_id), score_text_view(member_id));
    };
    entries_.for_range(bounds.first, bounds.count, callback);
    return bounds.count;
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

    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(ZSetEntry{.member = member_view(member_id),
                   .score = score,
                   .score_text = {}});
    };
    entries_.for_range(bounds->first, bounds->count, callback);
    return bounds->count;
  }
  template <class Fn>
  std::size_t for_range_with_score_text(long long start,
                                        long long stop,
                                        Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(ZSetEntry{.member = member_view(member_id),
                   .score = score,
                   .score_text = score_text_view(member_id)});
    };
    entries_.for_range(bounds->first, bounds->count, callback);
    return bounds->count;
  }
  template <class Fn>
  std::size_t for_reverse_range_members(ZSetRangeBounds bounds, Fn&& fn) const {
    auto callback = [this, &fn](std::uint32_t member_id) {
      fn(member_view(member_id));
    };
    entries_.for_reverse_member_ids(bounds.first, bounds.count, callback);
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
    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(member_view(member_id), score);
    };
    entries_.for_reverse_range(bounds.first, bounds.count, callback);
    return bounds.count;
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
    auto callback = [this, &fn](double, std::uint32_t member_id) {
      fn(member_view(member_id), score_text_view(member_id));
    };
    entries_.for_reverse_range(bounds.first, bounds.count, callback);
    return bounds.count;
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

    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(ZSetEntry{.member = member_view(member_id),
                   .score = score,
                   .score_text = {}});
    };
    entries_.for_reverse_range(bounds->first, bounds->count, callback);
    return bounds->count;
  }
  template <class Fn>
  std::size_t for_reverse_range_with_score_text(long long start,
                                                long long stop,
                                                Fn&& fn) const {
    const auto bounds = range_bounds(start, stop);
    if (!bounds) {
      return 0;
    }

    auto callback = [this, &fn](double score, std::uint32_t member_id) {
      fn(ZSetEntry{.member = member_view(member_id),
                   .score = score,
                   .score_text = score_text_view(member_id)});
    };
    entries_.for_reverse_range(bounds->first, bounds->count, callback);
    return bounds->count;
  }
  [[nodiscard]] bool check_invariants() const;
  [[nodiscard]] ZSetMemoryStats memory_stats() const noexcept;
  void compact(double member_index_density = kDefaultMemberIndexDensity);

  [[nodiscard]] std::size_t allocated_member_slots() const noexcept;
  [[nodiscard]] std::size_t free_member_slots() const noexcept;
  [[nodiscard]] std::size_t member_index_capacity() const noexcept;
  [[nodiscard]] std::size_t member_index_tombstones() const noexcept;
  [[nodiscard]] bool should_compact_after_removal(std::size_t removed_count) const noexcept;
  bool cleanup_member_index_after_removal_if_needed(std::size_t removed_count);
  bool rehash_member_index_same_capacity();
  bool compact_after_removal_if_needed(std::size_t removed_count);

 private:
  [[nodiscard]] std::uint32_t allocate_member_id(std::string_view member,
                                                 double score);
  [[nodiscard]] std::string_view member_view(std::uint32_t member_id) const noexcept;
  [[nodiscard]] std::string_view score_text_view(std::uint32_t member_id) const noexcept;
  void rebind_indexes() noexcept;
  void move_last_member_into_slot(std::uint32_t removed_member_id);

  std::unique_ptr<ZSetMemberStorage> member_storage_;
  ZSetMemberIndex members_;
  ZSetScoreIndex entries_;
  ZSetOptions options_;
};

struct StoreOptions {
  RankCacheMode rank_cache_mode{RankCacheMode::Off};
  bool score_string_cache{false};
  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
};

struct StoreMemoryStats {
  std::size_t inline_zset_count{0};
  std::size_t overflow_zset_count{0};
  std::size_t overflow_zset_capacity{0};
  std::size_t overflow_table_allocated_bytes{0};
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
  [[nodiscard]] StoreMemoryStats memory_stats() const noexcept;
  // Compact a zset in place to reclaim insertion slack (block capacity, vector
  // over-allocation) and repack the member index to `member_index_density`.
  // Returns the number of allocated bytes reclaimed, or nullopt if the key does
  // not exist.
  [[nodiscard]] std::optional<std::size_t> optimize(
      std::string_view key,
      double member_index_density = kDefaultMemberIndexDensity);

 private:
  [[nodiscard]] ZSet* find_zset(std::string_view key) noexcept;
  [[nodiscard]] const ZSet* find_zset(std::string_view key) const noexcept;
  [[nodiscard]] ZSet& get_or_create_zset(std::string_view key);
  void erase_if_empty(std::string_view key, const ZSet& zset);

  std::optional<ZSet> inline_zset_;
  std::string inline_key_;
  SwissTable<std::string, ZSet> overflow_zsets_;
  StoreOptions options_;
};

[[nodiscard]] std::string format_score(double score);

}  // namespace goblin::core
