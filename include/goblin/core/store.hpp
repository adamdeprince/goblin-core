#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <memory>
#include <new>
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
#include "goblin/core/keyspace_storage.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/string_value.hpp"
#include "goblin/core/ttl_set.hpp"
#include "goblin/core/swiss_table.hpp"
#include "goblin/core/zset_member_index.hpp"
#include "goblin/core/zset_member_layer.hpp"
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
  // SortedList (score index) sublist target size / load factor, runtime-tunable.
  std::size_t score_index_load{ZSetScoreIndex::kDefaultLoad};
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
  std::size_t member_storage_bytes{0};        // used = live + dead (arena high-water)
  std::size_t member_storage_dead_bytes{0};   // reclaimable-by-compact subset
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
  [[nodiscard]] int add(double score, std::string_view member,
                        const ZSetOptions* options = default_options());
  [[nodiscard]] bool remove(std::string_view member);
  // Remove every member whose score is in the [min, max] range (bounds optionally
  // exclusive; -inf/+inf for an open side). Returns the count removed. Uses the
  // score index's seek, so it is O(log n + removed), not a full scan.
  [[nodiscard]] std::size_t remove_by_score_range(double min, bool min_exclusive,
                                                  double max, bool max_exclusive);
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
    const auto* storage = member_storage();
    auto walk = [storage, &fn](std::uint32_t id) { fn(storage->view(id)); };
    entries().for_member_ids(bounds.first, bounds.count, walk);
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
  [[nodiscard]] ZSetMemoryStats memory_stats(
      const ZSetOptions* options = default_options()) const noexcept;
  void compact(double member_index_density = kDefaultMemberIndexDensity);

  // Serialize this zset as the operands of an OP_ZSET instruction: options,
  // canonical members, and (when with_accelerator) the index accelerator.
  void save(snapshot::Writer& writer, bool with_accelerator,
            const ZSetOptions* options = default_options()) const;
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
    const ZSetOptions* options;  // the store-global config for this full zset
  };
  // Options for standalone / default-constructed zsets; the store passes its own.
  static const ZSetOptions* default_options() noexcept;
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
  void init_empty(const ZSetOptions* options);  // fresh empty listpack-or-full
  void ensure_full(const ZSetOptions* options);
  void maybe_demote_to_small();

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
};

// The type stored at a key. Kept in a dense parallel byte array rather than inside
// the object union, so the union stays 16 B (no discriminant, no padding around
// one) and adding a type never costs a key or value a bit. Mapped to snapshot's
// SectionType at save time.
enum class KeyType : std::uint8_t {
  String = 0,
  Zset = 1,
  Hash = 2,
};

// The swap-remove an erase performs: the key that had id `from` now lives at id
// `to`. External indexes that reference keys by id (the TTL set) fix themselves
// up with this. Returned by Keyspace::erase_at.
struct KeyMove {
  std::uint64_t from;
  std::uint64_t to;
};

// EXPIRE-family condition flags (a bitmask; kNone sets unconditionally). A key
// with no current expiry is treated as +infinity for GT/LT. NX is exclusive with
// XX/GT/LT and GT with LT -- the command layer enforces that.
struct ExpireFlag {
  static constexpr unsigned kNone = 0;
  static constexpr unsigned kNx = 1;  // only if the key has no current expiry
  static constexpr unsigned kXx = 2;  // only if the key has a current expiry
  static constexpr unsigned kGt = 4;  // only if the new expiry is greater
  static constexpr unsigned kLt = 8;  // only if the new expiry is less
};

// A string value as two views: an inline head (the whole value, or its 6-byte
// prefix) and an arena tail (empty when the value fits inline). Two views so GET
// never copies -- the RESP layer writes head then tail.
struct StringValueView {
  std::string_view head;
  std::string_view tail;
  [[nodiscard]] std::size_t size() const noexcept {
    return head.size() + tail.size();
  }
};

// One key's object: a small-string value, a zset (BY VALUE -- the hot path pays
// no extra indirection or per-zset allocation), or a heap-owned hash (112 B, too
// large to inline). A bare union whose active member is named by the parallel
// KeyType array, so it carries no tag of its own; the Keyspace drives every
// construct / destroy / relocate by hand.
union KeyObjectSlot {
  StringValue str;
  ZSet zset;
  Hash* hash;
  KeyObjectSlot() noexcept {}
  ~KeyObjectSlot() {}
};
static_assert(sizeof(KeyObjectSlot) == 16);

// The unified keyspace: one namespace mapping every key of every type to its
// object, so a name resolves to at most one object (SET over a zset clobbers it;
// a wrong-type command is a structural error, not a second object). It is a
// MemberIndex (key -> id) over a KeyspaceStorage (id -> key bytes + the shared
// value arena), a parallel object union per id, and a parallel type byte per id.
// Ids stay dense via swap-remove.
class Keyspace {
 public:
  explicit Keyspace(HashOptions hash_options)
      : index_(&storage_), hash_options_(hash_options) {}
  ~Keyspace() { destroy_all(); }
  Keyspace(const Keyspace&) = delete;
  Keyspace& operator=(const Keyspace&) = delete;
  // Movable (an empty store is move-assigned in a few places). The member index
  // holds a back-pointer into our storage, so every move rebinds it; the deque
  // move is O(1) and never relocates a live object.
  Keyspace(Keyspace&& other) noexcept
      : storage_(std::move(other.storage_)),
        index_(std::move(other.index_)),
        objects_(std::move(other.objects_)),
        types_(std::move(other.types_)),
        hash_options_(other.hash_options_) {
    index_.set_members(&storage_);
  }
  Keyspace& operator=(Keyspace&& other) noexcept {
    if (this != &other) {
      destroy_all();
      storage_ = std::move(other.storage_);
      index_ = std::move(other.index_);
      objects_ = std::move(other.objects_);
      types_ = std::move(other.types_);
      hash_options_ = other.hash_options_;
      index_.set_members(&storage_);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept { return types_.size(); }
  [[nodiscard]] bool empty() const noexcept { return types_.empty(); }

  [[nodiscard]] std::optional<KeyType> type_of(
      std::string_view key) const noexcept {
    const auto id = find_id(key);
    if (!id) {
      return std::nullopt;
    }
    return static_cast<KeyType>(types_[*id]);
  }
  [[nodiscard]] bool contains(std::string_view key) const noexcept {
    return find_id(key).has_value();
  }
  [[nodiscard]] bool is_type(std::string_view key, KeyType type) const noexcept {
    const auto id = find_id(key);
    return id.has_value() && types_[*id] == static_cast<std::uint8_t>(type);
  }

  // ---- strings ----
  [[nodiscard]] std::optional<StringValueView> get_string(
      std::string_view key) const noexcept {
    const auto id = find_id(key);
    if (!id || types_[*id] != static_cast<std::uint8_t>(KeyType::String)) {
      return std::nullopt;
    }
    return value_view(objects_[*id].str);
  }
  [[nodiscard]] std::optional<std::size_t> string_length(
      std::string_view key) const noexcept {
    const auto id = find_id(key);
    if (!id || types_[*id] != static_cast<std::uint8_t>(KeyType::String)) {
      return std::nullopt;
    }
    return objects_[*id].str.length;
  }
  // SET: clobbers any prior object at this name.
  void set_string(std::string_view key, std::string_view value) {
    if (const auto id = find_id(key)) {
      store_string(*id, value);
    } else {
      write_string_value(objects_[create_key(key, KeyType::String)].str, value);
    }
    maybe_compact();
  }
  // SETNX-style: set only if the key is absent (of any type). true = stored.
  [[nodiscard]] bool set_string_if_absent(std::string_view key,
                                          std::string_view value) {
    if (find_id(key)) {
      return false;
    }
    write_string_value(objects_[create_key(key, KeyType::String)].str, value);
    return true;
  }

  // ---- zset ----
  [[nodiscard]] ZSet* find_zset(std::string_view key) noexcept {
    const auto id = find_id(key);
    if (!id || types_[*id] != static_cast<std::uint8_t>(KeyType::Zset)) {
      return nullptr;
    }
    return &objects_[*id].zset;
  }
  [[nodiscard]] const ZSet* find_zset(std::string_view key) const noexcept {
    const auto id = find_id(key);
    if (!id || types_[*id] != static_cast<std::uint8_t>(KeyType::Zset)) {
      return nullptr;
    }
    return &objects_[*id].zset;
  }
  [[nodiscard]] ZSet& get_or_create_zset(std::string_view key,
                                         const ZSetOptions* zset_options) {
    if (const auto id = find_id(key)) {
      assert(types_[*id] == static_cast<std::uint8_t>(KeyType::Zset));
      return objects_[*id].zset;
    }
    const auto id = create_key(key, KeyType::Zset);
    return *::new (static_cast<void*>(&objects_[id].zset)) ZSet(zset_options);
  }
  [[nodiscard]] ZSet& place_loaded_zset(std::string_view key, ZSet&& zset) {
    const auto id = create_key(key, KeyType::Zset);
    return *::new (static_cast<void*>(&objects_[id].zset)) ZSet(std::move(zset));
  }

  // ---- hash ----
  [[nodiscard]] Hash* find_hash(std::string_view key) noexcept {
    const auto id = find_id(key);
    if (!id || types_[*id] != static_cast<std::uint8_t>(KeyType::Hash)) {
      return nullptr;
    }
    return objects_[*id].hash;
  }
  [[nodiscard]] const Hash* find_hash(std::string_view key) const noexcept {
    const auto id = find_id(key);
    if (!id || types_[*id] != static_cast<std::uint8_t>(KeyType::Hash)) {
      return nullptr;
    }
    return objects_[*id].hash;
  }
  [[nodiscard]] Hash& get_or_create_hash(std::string_view key) {
    if (const auto id = find_id(key)) {
      assert(types_[*id] == static_cast<std::uint8_t>(KeyType::Hash));
      return *objects_[*id].hash;
    }
    const auto id = create_key(key, KeyType::Hash);
    objects_[id].hash = new Hash(hash_options_);
    return *objects_[id].hash;
  }
  [[nodiscard]] Hash& place_loaded_hash(std::string_view key, Hash&& hash) {
    const auto id = create_key(key, KeyType::Hash);
    objects_[id].hash = new Hash(std::move(hash));
    return *objects_[id].hash;
  }

  // ---- lifecycle ----
  [[nodiscard]] std::optional<std::uint64_t> id_of(
      std::string_view key) const noexcept {
    return find_id(key);
  }
  // Key bytes for a live id (e.g. one a TTL entry references, for snapshotting).
  [[nodiscard]] std::string_view key_for_id(std::uint64_t id) const noexcept {
    return storage_.view(id);
  }

  // Erase the key at `id`, returning the swap-remove it caused (the last key slid
  // into `id`), or nullopt if `id` was the last key. The caller fixes up any
  // external id-keyed index (the TTL set) using the returned move.
  [[nodiscard]] std::optional<KeyMove> erase_at(std::uint64_t id) {
    const auto last = static_cast<std::uint64_t>(types_.size() - 1);
    destroy_object(id);
    storage_.mark_key_dead(id);
    (void)index_.erase(storage_.view(id));  // key bytes at id still intact
    std::optional<KeyMove> moved;
    if (id != last) {
      (void)index_.move_member_id(last, id);  // last's key now maps to id
      storage_.move_key_slot(id, last);
      types_[id] = types_[last];
      relocate_object(id, last);
      moved = KeyMove{last, id};
    }
    storage_.pop_back_key();
    objects_.pop_back();
    types_.pop_back();
    maybe_compact();
    return moved;
  }

  template <class Fn>
  void for_each_string(Fn&& fn) const {  // fn(std::string_view, StringValueView)
    for (std::uint64_t id = 0; id < types_.size(); ++id) {
      if (types_[id] == static_cast<std::uint8_t>(KeyType::String)) {
        fn(storage_.view(id), value_view(objects_[id].str));
      }
    }
  }
  template <class Fn>
  void for_each_zset(Fn&& fn) const {  // fn(std::string_view, const ZSet&)
    for (std::uint64_t id = 0; id < types_.size(); ++id) {
      if (types_[id] == static_cast<std::uint8_t>(KeyType::Zset)) {
        fn(storage_.view(id), objects_[id].zset);
      }
    }
  }
  template <class Fn>
  void for_each_hash(Fn&& fn) const {  // fn(std::string_view, const Hash&)
    for (std::uint64_t id = 0; id < types_.size(); ++id) {
      if (types_[id] == static_cast<std::uint8_t>(KeyType::Hash)) {
        fn(storage_.view(id), *objects_[id].hash);
      }
    }
  }

  void clear() noexcept {
    destroy_all();
    objects_.clear();
    types_.clear();
    storage_ = KeyspaceStorage(storage_.chunk_bytes());
    index_ = MemberIndex<KeyspaceStorage, KeyMeta>(&storage_);
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return storage_.allocated_bytes() + index_.allocated_bytes() +
           types_.capacity() + types_.size() * sizeof(KeyObjectSlot);
  }
  [[nodiscard]] std::size_t value_arena_used_bytes() const noexcept {
    return storage_.used_bytes();
  }
  // Keyspace contribution to used_memory: the key+value arena's used (live+dead)
  // bytes plus the fixed index / type / object-slot tables (no dead of their own).
  [[nodiscard]] std::size_t footprint_used_bytes() const noexcept {
    return storage_.used_bytes() + index_.allocated_bytes() +
           types_.capacity() + types_.size() * sizeof(KeyObjectSlot);
  }
  [[nodiscard]] std::size_t footprint_dead_bytes() const noexcept {
    return storage_.dead_bytes();
  }

 private:
  [[nodiscard]] std::optional<std::uint64_t> find_id(
      std::string_view key) const noexcept {
    const auto* meta = index_.find(key);
    if (meta == nullptr) {
      return std::nullopt;
    }
    return meta->get();
  }

  // Append the key blob, a raw object slot, and its type; register key -> id.
  [[nodiscard]] std::uint64_t create_key(std::string_view key, KeyType type) {
    const auto id = storage_.push_back_key(key);
    objects_.emplace_back();
    types_.push_back(static_cast<std::uint8_t>(type));
    KeyMeta meta;
    meta.set(id);
    index_.insert(key, meta);
    return id;
  }

  // Overwrite id (a string, or another type being clobbered) with a string value.
  void store_string(std::uint64_t id, std::string_view value) {
    if (types_[id] == static_cast<std::uint8_t>(KeyType::String)) {
      auto& slot = objects_[id].str;
      if (!slot.is_inline()) {
        storage_.mark_tail_dead(slot.tail_length());
      }
      write_string_value(slot, value);
      return;
    }
    destroy_object(id);
    types_[id] = static_cast<std::uint8_t>(KeyType::String);
    write_string_value(objects_[id].str, value);
  }

  void write_string_value(StringValue& slot, std::string_view value) {
    slot.length = static_cast<std::uint16_t>(value.size());
    if (slot.is_inline()) {
      std::memcpy(slot.inline_bytes, value.data(), value.size());
      return;
    }
    std::memcpy(slot.spill.prefix, value.data(), StringValue::kPrefixCap);
    const auto loc = storage_.append_tail(value.substr(StringValue::kPrefixCap));
    slot.set_tail(loc.block, loc.offset);
  }

  [[nodiscard]] StringValueView value_view(
      const StringValue& slot) const noexcept {
    if (slot.is_inline()) {
      return {slot.head(), {}};
    }
    return {slot.head(),
            storage_.tail_view({slot.tail_block(), slot.tail_offset()},
                               slot.tail_length())};
  }

  void destroy_object(std::uint64_t id) noexcept {
    switch (static_cast<KeyType>(types_[id])) {
      case KeyType::String: {
        auto& slot = objects_[id].str;
        if (!slot.is_inline()) {
          storage_.mark_tail_dead(slot.tail_length());
        }
        break;
      }
      case KeyType::Zset:
        objects_[id].zset.~ZSet();
        break;
      case KeyType::Hash:
        delete objects_[id].hash;
        break;
    }
  }

  // Move src's object into dst (raw storage) and leave src destroyed.
  void relocate_object(std::uint64_t dst, std::uint64_t src) noexcept {
    switch (static_cast<KeyType>(types_[src])) {
      case KeyType::String:
        objects_[dst].str = objects_[src].str;
        break;
      case KeyType::Zset:
        ::new (static_cast<void*>(&objects_[dst].zset))
            ZSet(std::move(objects_[src].zset));
        objects_[src].zset.~ZSet();
        break;
      case KeyType::Hash:
        objects_[dst].hash = objects_[src].hash;
        break;
    }
  }

  void destroy_all() noexcept {
    for (std::uint64_t id = 0; id < types_.size(); ++id) {
      destroy_object(id);
    }
  }

  void maybe_compact() {
    if (storage_.should_compact()) {
      compact_arena();
    }
  }
  // Rebuild the arena, dropping dead key/tail bytes. Ids are preserved (walked in
  // order), so the index and object/type arrays stay valid -- only the arena and
  // each spilled value's tail address change.
  void compact_arena() {
    KeyspaceStorage fresh(storage_.chunk_bytes());
    fresh.reserve(types_.size());
    for (std::uint64_t id = 0; id < types_.size(); ++id) {
      (void)fresh.push_back_key(storage_.view(id));
      if (types_[id] == static_cast<std::uint8_t>(KeyType::String)) {
        auto& slot = objects_[id].str;
        if (!slot.is_inline()) {
          const auto tail = storage_.tail_view(
              {slot.tail_block(), slot.tail_offset()}, slot.tail_length());
          const auto loc = fresh.append_tail(tail);
          slot.set_tail(loc.block, loc.offset);
        }
      }
    }
    storage_ = std::move(fresh);
    index_.set_members(&storage_);
  }

  KeyspaceStorage storage_;
  MemberIndex<KeyspaceStorage, KeyMeta> index_;
  std::deque<KeyObjectSlot> objects_;
  std::vector<std::uint8_t> types_;
  HashOptions hash_options_;
};

struct StoreOptions {
  RankCacheMode rank_cache_mode{RankCacheMode::Off};
  bool score_string_cache{false};
  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t zset_chunk_bytes{ZSetMemberStorage::kDefaultChunkBytes};
  std::size_t hash_chunk_bytes{HashStorage::kDefaultChunkBytes};
  // SortedList (score index) load factor -- sublist target size. Bigger = fewer,
  // larger sublists (less block overhead, more memmove per mutation); smaller =
  // the reverse. Runtime-tunable via --load-factor to sweep the large-zset knee.
  std::size_t zset_score_index_load{ZSetScoreIndex::kDefaultLoad};
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

// Process-wide allocation accounting for INFO, summed from every arena's O(1)
// live/dead counters (no scan): `used_memory` is what our allocation layers hold
// -- arena live + dead + the fixed index/table structures -- and
// `reclaimable_bytes` is the dead subset a compaction would free.
struct MemoryReport {
  std::size_t used_memory{0};
  std::size_t reclaimable_bytes{0};
};

class Store {
 public:
  explicit Store(StoreOptions options = {});

  [[nodiscard]] long long zadd(std::string_view key, double score, std::string_view member);
  [[nodiscard]] long long zrem(std::string_view key, std::span<const std::string_view> members);
  // ZREMRANGEBYSCORE: remove members with score in [min, max] (bounds optionally
  // exclusive; -inf/+inf accepted). Returns the count removed; drops an emptied key.
  [[nodiscard]] long long zremrangebyscore(std::string_view key, double min,
                                           bool min_exclusive, double max,
                                           bool max_exclusive);
  // GOBLIN.ZWINDOW sliding-window limiter: evict entries with score <= `cutoff`,
  // then if the count is below `limit` record `member` at `now` and (re)arm the
  // key's TTL to `when_ms`. Returns true if the request was admitted.
  [[nodiscard]] bool zwindow(std::string_view key, double now, double cutoff,
                             long long limit, std::string_view member,
                             std::uint64_t when_ms, std::uint64_t now_ms);
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
  // Iterate every (member, score) of a zset in ZRANGE order (by score, ties by
  // member bytes), for whole-zset reads such as the time-decay rescore.
  // `fn(std::string_view member, double score)`; a missing key is a no-op.
  template <class Fn>
  std::size_t for_each_zset_entry(std::string_view key, Fn&& fn) const {
    const auto* zset = find_zset(key);
    if (zset == nullptr) {
      return 0;
    }
    return zset->for_range(0, -1, [&fn](const ZSetEntry& e) {
      fn(e.member, e.score);
    });
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

  // --- String (value) ---
  // A key holds at most one type (unified keyspace). SET replaces whatever was
  // there, of any type; the other string ops and the key ops below gate on type
  // in the command layer (WRONGTYPE) exactly as the zset/hash ops do.
  [[nodiscard]] static constexpr std::size_t max_value_bytes() noexcept {
    return StringValueMaxBytes;
  }
  void set(std::string_view key, std::string_view value);
  [[nodiscard]] bool set_nx(std::string_view key, std::string_view value);
  [[nodiscard]] std::optional<StringValueView> get(
      std::string_view key) const noexcept;
  [[nodiscard]] std::optional<std::string> get_set(std::string_view key,
                                                   std::string_view value);
  [[nodiscard]] std::optional<std::string> get_del(std::string_view key);
  // GOBLIN.CAD compare-and-delete: if the key holds a string equal to
  // `expected`, delete it and return true; otherwise false. The value is
  // compared in place (no allocation on the common inline path). A non-string
  // key is rejected as WRONGTYPE by the command layer before this is reached.
  [[nodiscard]] bool compare_and_delete(std::string_view key,
                                        std::string_view expected);
  // GOBLIN.CAEXPIRE compare-and-expire (renew): if the key holds a string equal
  // to `expected`, set its absolute expiry to `when_ms` and return true;
  // otherwise false. `when_ms` at/before `now` deletes the key (as PEXPIRE does
  // with a non-positive TTL). A non-string key is rejected as WRONGTYPE by the
  // command layer before this is reached.
  [[nodiscard]] bool compare_and_expire(std::string_view key,
                                        std::string_view expected,
                                        std::uint64_t when_ms, std::uint64_t now);
  // GOBLIN.CAS compare-and-set: if the key holds a string equal to `expected`,
  // overwrite it with `new_value` **preserving any existing TTL** (KEEPTTL, not a
  // bare SET) and return true; otherwise false. A non-string key is rejected as
  // WRONGTYPE by the command layer before this is reached.
  [[nodiscard]] bool compare_and_set(std::string_view key,
                                     std::string_view expected,
                                     std::string_view new_value);
  [[nodiscard]] std::optional<std::size_t> strlen(
      std::string_view key) const noexcept;
  [[nodiscard]] std::size_t append(std::string_view key, std::string_view value);
  // INCR/DECR/INCRBY/DECRBY: nullopt if the value is not an integer or the result
  // would overflow long long.
  [[nodiscard]] std::optional<long long> incr_by(std::string_view key,
                                                 long long delta);
  // GOBLIN.INCRBOUND: bounded increment. If the current integer value plus
  // `delta` is <= `max`, apply the increment (creating the key from 0 if absent,
  // preserving any TTL) and return the new value; otherwise leave the key
  // untouched and return -1. nullopt only when the stored value is not an integer
  // or the admitted result would underflow long long.
  [[nodiscard]] std::optional<long long> incr_bound(std::string_view key,
                                                    long long delta, long long max);
  // GOBLIN.DECRPOS: decrement by one only when the current integer value is > 0,
  // returning the new value; otherwise (including an absent key) leave the key
  // untouched -- never creating it -- and return -1. nullopt only when the stored
  // value is not an integer.
  [[nodiscard]] std::optional<long long> decr_positive(std::string_view key);
  // GOBLIN.INCREX: INCR the key by 1, and if the result is 1 (the key was just
  // created) set its absolute expiry to `when_ms`. Returns the new value, or
  // nullopt if the value is not an integer or the result would overflow.
  [[nodiscard]] std::optional<long long> incr_expire(std::string_view key,
                                                     std::uint64_t when_ms,
                                                     std::uint64_t now);
  // INCRBYFLOAT: nullopt if the value or the result is not a finite number;
  // returns the canonical text form that is also stored.
  [[nodiscard]] std::optional<std::string> incr_by_float(std::string_view key,
                                                         double delta);
  // GETRANGE key start end: the inclusive substring, with Redis index rules
  // (negatives count from the end; out-of-range clamps; empty when start > end
  // or the value is absent/empty).
  [[nodiscard]] std::string getrange(std::string_view key, long long start,
                                     long long end) const;
  // SETRANGE key offset value: overwrite starting at offset, zero-padding any
  // gap, and return the new length -- or nullopt if the result would exceed the
  // 64 KiB ceiling. offset must be >= 0 (the command layer rejects negatives).
  [[nodiscard]] std::optional<std::size_t> setrange(std::string_view key,
                                                    std::size_t offset,
                                                    std::string_view value);

  // --- Keys (any type) ---
  [[nodiscard]] bool del(std::string_view key) { return erase_key(key); }
  [[nodiscard]] bool exists(std::string_view key) const noexcept {
    return keyspace_.contains(key);
  }
  [[nodiscard]] std::optional<KeyType> key_type(
      std::string_view key) const noexcept {
    return keyspace_.type_of(key);
  }
  [[nodiscard]] bool key_is_string(std::string_view key) const noexcept {
    return keyspace_.is_type(key, KeyType::String);
  }

  // --- TTL (absolute times are ms since the Unix epoch) ---
  // Current wall-clock ms. The command layer reads it once and passes it to the
  // methods below, so one command sees a single consistent "now"; tests pass
  // their own to stay deterministic.
  [[nodiscard]] std::uint64_t now_ms() const noexcept;
  [[nodiscard]] bool ttl_empty() const noexcept { return ttl_.empty(); }
  // Set the key's absolute expiry, subject to the ExpireFlag condition. A time
  // already at/past now deletes the key. Returns false when the key does not
  // exist or the condition is not met.
  [[nodiscard]] bool expire_at_ms(std::string_view key, std::uint64_t when_ms,
                                  std::uint64_t now,
                                  unsigned flags = ExpireFlag::kNone);
  // Remaining ms, or -2 (no such key) / -1 (no expiry).
  [[nodiscard]] long long pttl_ms(std::string_view key, std::uint64_t now) const;
  // Absolute expiry ms, or -2 (no such key) / -1 (no expiry).
  [[nodiscard]] long long expiretime_ms(std::string_view key) const;
  // PERSIST: drop the TTL; true if one was removed.
  [[nodiscard]] bool persist(std::string_view key);
  // SET ... KEEPTTL: store a value without touching an existing TTL.
  void set_keep_ttl(std::string_view key, std::string_view value);
  // Lazy expiration: if the key's TTL is at/past now, delete it and report it.
  // Cheap no-op when no TTLs exist.
  bool purge_if_expired(std::string_view key, std::uint64_t now);
  // Active expiration: delete up to `budget` keys due at/before now, soonest
  // first; returns the count expired.
  std::size_t active_expire(std::uint64_t now, std::size_t budget);

  [[nodiscard]] StoreMemoryStats memory_stats() const noexcept;
  // Aggregate used_memory / reclaimable across every zset, hash, and the keyspace.
  [[nodiscard]] MemoryReport memory_report() const noexcept;
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
  [[nodiscard]] ZSet* find_zset(std::string_view key) noexcept {
    return keyspace_.find_zset(key);
  }
  [[nodiscard]] const ZSet* find_zset(std::string_view key) const noexcept {
    return keyspace_.find_zset(key);
  }
  [[nodiscard]] ZSet& get_or_create_zset(std::string_view key) {
    return keyspace_.get_or_create_zset(key, &zset_options_);
  }
  [[nodiscard]] const ZSetOptions* zset_options() const noexcept {
    return &zset_options_;
  }
  [[nodiscard]] const ZSet* find_member_layer_template() const noexcept;
  void erase_if_empty(std::string_view key, const ZSet& zset);

  void place_loaded_hash(std::string key, Hash&& hash);
  [[nodiscard]] Hash* find_hash(std::string_view key) noexcept {
    return keyspace_.find_hash(key);
  }
  [[nodiscard]] const Hash* find_hash(std::string_view key) const noexcept {
    return keyspace_.find_hash(key);
  }
  [[nodiscard]] Hash& get_or_create_hash(std::string_view key) {
    return keyspace_.get_or_create_hash(key);
  }
  void erase_if_empty(std::string_view key, const Hash& hash);

  // Erase a key (any type), clearing its own TTL and rekeying the TTL of the key
  // that the swap-remove slid into its slot.
  bool erase_key(std::string_view key) {
    const auto id = keyspace_.id_of(key);
    if (!id) {
      return false;
    }
    ttl_.clear(*id);
    erase_keyspace_at(*id);
    return true;
  }
  // Erase the keyspace key at `id` and fix up the swapped key's TTL. Does NOT
  // touch id's own TTL entry -- callers reached from the TtlSet (active
  // expiration) already removed it.
  void erase_keyspace_at(std::uint64_t id) {
    if (const auto moved = keyspace_.erase_at(id)) {
      ttl_.rekey(moved->from, moved->to);
    }
  }

  StoreOptions options_;
  // Built once in the constructor from options_; every zset in this store shares
  // it (an 8 B pointer per zset, not a 32 B copy). Declared before keyspace_ so
  // it is alive when the keyspace binds to it.
  ZSetOptions zset_options_;
  // Every key of every type lives here, in one namespace. See Keyspace above.
  Keyspace keyspace_;
  // The one keyspace-wide expiry set (sparse: only keys with a TTL). See TtlSet.
  TtlSet ttl_;
  int background_save_child_ = -1;  // pid of an in-flight fork(), or -1
  std::string background_save_path_;
};

[[nodiscard]] std::string format_score(double score);

}  // namespace goblin::core
