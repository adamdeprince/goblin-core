#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "goblin/core/hash_listpack.hpp"
#include "goblin/core/hash_storage.hpp"
#include "goblin/core/keyspace_storage.hpp"
#include "goblin/core/selectable_member_index.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/zset_member_index.hpp"

namespace goblin::core {

enum class HashImplementation : std::uint8_t {
  Efficient,
  Realtime,
};

[[nodiscard]] constexpr std::string_view hash_implementation_name(
    HashImplementation implementation) noexcept {
  switch (implementation) {
    case HashImplementation::Efficient:
      return "efficient";
    case HashImplementation::Realtime:
      return "rt";
  }
  return "unknown";
}

// A hash uses the growth knob for the full form, plus an optional count ceiling
// for the compact form. By default the 64 KiB blob capacity decides promotion;
// 0 disables the compact form.
struct HashOptions {
  static constexpr std::size_t kDefaultListpackMaxEntries =
      std::numeric_limits<std::size_t>::max();
  static constexpr std::size_t kDefaultCompactionWorkBudget = 32;

  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t chunk_bytes{HashStorage::kDefaultChunkBytes};
  // Exact subset-sum donor selection is useful for measuring maximum packing,
  // but is too expensive for automatic maintenance unless explicitly enabled.
  bool compaction_knapsack{false};
  StringEncodingOptions string_encoding{};
  // Field ids or chunk candidates inspected after each mutating command while
  // automatic arena compaction is active.
  std::size_t compaction_work_budget{kDefaultCompactionWorkBudget};
  // Optional count ceiling for the indexed compact blob. The default has no
  // count ceiling: the 64 KiB blob limit decides promotion. 0 disables the
  // compact form; a finite value lets miss- or rebuild-heavy workloads promote
  // earlier.
  std::size_t listpack_max_entries{kDefaultListpackMaxEntries};
  // Shared fixed-record pool for RT field indexes. The pool is rounded to
  // 2 MiB slabs, locked/prefaulted once, and never grows during serving.
  std::size_t realtime_index_bytes{
      LinearHashArena<ZSetMemberMeta>::kDefaultBytes};
};

struct HashContext {
  using RealtimeIndexArena = LinearHashArena<ZSetMemberMeta>;

  KeyspaceStorage* storage{nullptr};
  HashOptions options{};
  void* compact_owner{nullptr};
  void (*maybe_compact)(void*){nullptr};
  std::shared_ptr<RealtimeIndexArena> realtime_index_arena{};

  [[nodiscard]] std::shared_ptr<RealtimeIndexArena> ensure_realtime_arena() {
    if (!realtime_index_arena) {
      realtime_index_arena =
          std::make_shared<RealtimeIndexArena>(options.realtime_index_bytes);
    }
    return realtime_index_arena;
  }
};
static_assert(alignof(HashContext) >= 8);

struct HashMemoryStats {
  HashImplementation implementation{HashImplementation::Efficient};
  std::size_t field_count{0};
  std::size_t field_value_live_bytes{0};
  std::size_t field_value_dead_bytes{0};
  std::size_t field_value_allocated_bytes{0};
  std::size_t field_index_allocated_bytes{0};
  std::size_t field_compaction_active{0};
  std::size_t field_compaction_victim_chunk{0};
  std::size_t field_compaction_fields_scanned{0};
  std::size_t field_compaction_fields_total{0};
  std::size_t field_compaction_candidates_remaining{0};
  std::size_t field_compaction_relocated_fields{0};
  std::size_t field_compaction_relocated_bytes{0};
  std::uint64_t field_compaction_selection_nanoseconds{0};
  std::uint64_t field_compaction_densify_nanoseconds{0};
  std::uint64_t field_compaction_donor_nanoseconds{0};
  std::uint64_t field_compaction_tail_settle_nanoseconds{0};
  std::size_t hash_heap_allocated_bytes{0};
  std::size_t keyspace_accounted_bytes{0};
  std::size_t total_allocated_bytes{0};
};

// A Redis hash: field->value. The Hash object itself is the keyspace's 16-byte
// inline handle. A compact hash stores an arena {block, offset}; promotion swaps
// that word for a heap FullState pointer without changing the keyspace slot.
// Compact bytes therefore share KeyspaceStorage with keys and spilled strings
// and move when that arena compacts. Full hashes retain the Swiss index + packed
// HashStorage representation.
class Hash {
  struct FullState;

 public:
  static constexpr double kDefaultFieldIndexDensity = 0.97;

  explicit Hash(HashOptions options = {},
                HashImplementation implementation =
                    HashImplementation::Efficient) {
    normalize_options(options);
    auto* context = new HashContext;
    context->storage = new KeyspaceStorage;
    context->options = options;
    context_flags_ = reinterpret_cast<std::uintptr_t>(context) | kOwnContext |
                     implementation_flag(implementation);
    init_empty();
  }

  explicit Hash(HashContext& context,
                HashImplementation implementation =
                    HashImplementation::Efficient)
      : context_flags_(reinterpret_cast<std::uintptr_t>(&context) |
                       implementation_flag(implementation)) {
    assert((reinterpret_cast<std::uintptr_t>(&context) & kFlagMask) == 0);
    normalize_options(context.options);
    init_empty();
  }

  ~Hash() {
    destroy_representation();
    destroy_owned_context();
  }

  Hash(const Hash&) = delete;
  Hash& operator=(const Hash&) = delete;

  Hash(Hash&& other) noexcept
      : context_flags_(std::exchange(other.context_flags_, 0)),
        rep_(std::exchange(other.rep_, kEmptyCompact)) {}
  Hash& operator=(Hash&& other) noexcept {
    if (this != &other) {
      destroy_representation();
      destroy_owned_context();
      context_flags_ = std::exchange(other.context_flags_, 0);
      rep_ = std::exchange(other.rep_, kEmptyCompact);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    if (is_small()) {
      return read_small([](const HashListpack& lp) { return lp.size(); });
    }
    return full().storage->size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] const HashOptions& options() const noexcept {
    return context().options;
  }
  [[nodiscard]] bool is_small() const noexcept {
    return (context_flags_ & kFullRepresentation) == 0;
  }
  [[nodiscard]] HashImplementation implementation() const noexcept {
    return (context_flags_ & kRealtimeImplementation) != 0
               ? HashImplementation::Realtime
               : HashImplementation::Efficient;
  }

  void rebind(HashContext& new_context) {
    assert((reinterpret_cast<std::uintptr_t>(&new_context) & kFlagMask) == 0);
    if (&context() == &new_context && !owns_context()) {
      return;
    }
    auto* old_context = &context();
    const bool old_owned = owns_context();
    if (!is_small() && implementation() == HashImplementation::Realtime &&
        old_context->realtime_index_arena !=
            new_context.realtime_index_arena) {
      auto& fs = full();
      SelectableMemberIndex<HashStorage> fields(
          fs.storage.get(), new_context.options.member_index_growth,
          MemberIndexImplementation::Realtime,
          new_context.ensure_realtime_arena());
      fields.reserve_for_density(fs.storage->size(),
                                 kDefaultFieldIndexDensity);
      for (std::uint32_t id = 0;
           id < static_cast<std::uint32_t>(fs.storage->size()); ++id) {
        fields.insert_packed(fs.storage->view(id),
                             ZSetMemberMeta{.member_id = id});
      }
      fs.fields = std::move(fields);
      fs.bind_rt();
    }
    if (is_small() && rep_ != kEmptyCompact) {
      const auto bytes = compact_blob_view();
      const auto location = new_context.storage->append_object_blob(bytes);
      old_context->storage->mark_object_blob_dead(
          static_cast<std::uint16_t>(bytes.size()));
      rep_ = pack_location(location);
    }
    const auto representation =
        context_flags_ & (kFullRepresentation | kRealtimeImplementation);
    context_flags_ = reinterpret_cast<std::uintptr_t>(&new_context) |
                     representation;
    if (old_owned) {
      delete old_context->storage;
      delete old_context;
    }
  }

  void relocate_compact_blob(KeyspaceStorage& destination) {
    if (!is_small() || rep_ == kEmptyCompact) {
      return;
    }
    rep_ = pack_location(destination.append_object_blob(compact_blob_view()));
  }

  // Pre-size the full form for an upcoming bulk insert. No-op while listpack
  // (promotion happens on the set that crosses the threshold). RT indexes only
  // bulk-reserve when empty (linear-hash addressing); non-empty RT growth stays
  // incremental. Efficient Swiss indexes grow geometrically for any size.
  void reserve_additional(std::size_t additional) {
    if (additional == 0 || is_small()) {
      return;
    }
    full().storage->reserve_additional(additional);
    full().fields.reserve_additional(additional);
  }

  // Empty the hash in place. Compact bytes become reclaimable in the shared
  // keyspace arena; a promoted hash releases its full state.
  void clear() {
    destroy_representation();
    init_empty();
  }

  // HSET one field. Returns 1 if the field is new, 0 if it updated an existing
  // field's value.
  int set(std::string_view field, std::string_view value) {
    validate_field(field);
    if (is_small()) {
      const auto result = mutate_small([&](HashListpack& lp) {
        return lp.set(field, value, options().listpack_max_entries,
                      options().string_encoding);
      });
      if (!result.needs_full) {
        return result.added ? 1 : 0;
      }
      ensure_full();
    }
    const auto added = set_full(field, value);
    // Fresh inserts create no dead bytes; only value updates can.
    if (added == 0) {
      maybe_compact();
    }
    return added;
  }

  long long set_many(
      std::span<const std::pair<std::string_view, std::string_view>> fields) {
    if (fields.empty()) {
      return 0;
    }
    for (const auto& [field, value] : fields) {
      validate_field(field);
      if (!string_value_fits(value, options().string_encoding)) {
        throw std::length_error("hash value cannot fit its encoded form");
      }
    }
    if (is_small()) {
      const auto result = mutate_small([&](HashListpack& lp) {
        return lp.set_many(fields, options().listpack_max_entries,
                           options().string_encoding);
      });
      if (!result.needs_full) {
        return result.added;
      }
      ensure_full();
    }
    // Bulk-aware reserve: empty RT indexes pre-grow primaries; efficient Swiss
    // indexes grow geometrically.
    reserve_additional(fields.size());
    long long added = 0;
    long long updates = 0;
    for (const auto& [field, value] : fields) {
      // Each structural RT insert advances one bounded physical maintenance
      // step. Existing-field updates do not advance maintenance.
      const auto inserted = set_full(field, value);
      added += inserted;
      updates += 1 - inserted;
    }
    // Only value updates that orphaned arena bytes need a compact step.
    if (updates != 0) {
      maybe_compact();
    }
    return added;
  }

  // HSETNX. Returns 1 if the field was set, 0 if it already existed.
  int set_nx(std::string_view field, std::string_view value) {
    validate_field(field);
    if (is_small()) {
      if (!read_small([&](const HashListpack& lp) {
            return lp.absent(field, options().string_encoding);
          })) {
        return 0;
      }
      const auto result = mutate_small([&](HashListpack& lp) {
        return lp.set(field, value, options().listpack_max_entries,
                      options().string_encoding);
      });
      if (!result.needs_full) {
        return result.added ? 1 : 0;
      }
      ensure_full();
      // After promote the field is still absent; fall through to full set_nx.
    }
    auto& fs = full();
    // Encode only after absence is known (D): make_meta runs solely on insert.
    const bool inserted =
        fs.rt != nullptr
            ? fs.rt->find_or_emplace(
                  field, [](ZSetMemberMeta&) {},
                  [&]() {
                    const auto field_id = fs.storage->push_back(field, value);
                    return ZSetMemberMeta{.member_id = field_id};
                  })
            : fs.fields.find_or_emplace(
                  field, [](ZSetMemberMeta&) {},
                  [&]() {
                    const auto field_id = fs.storage->push_back(field, value);
                    return ZSetMemberMeta{.member_id = field_id};
                  });
    // Insert-only: no dead bytes, so skip auto-compaction.
    return inserted ? 1 : 0;
  }

  [[nodiscard]] std::optional<EncodedStringView> get(
      std::string_view field) const {
    if (is_small()) {
      return read_small([&](const HashListpack& lp) {
        return lp.get(field, options().string_encoding);
      });
    }
    const auto& fs = full();
    // RT-direct: monomorphic LinearHashIndex probe (no Selectable branch).
    const ZSetMemberMeta* meta = nullptr;
    if (fs.rt != nullptr) {
      meta = fs.rt->find(field);
    } else {
      meta = fs.fields.find(field);
    }
    if (meta == nullptr) {
      return std::nullopt;
    }
    // Field bytes were prefetched during the index compare; pull value bytes
    // into L1 before decoding (contiguous after insert; relocated after grow).
    fs.storage->prefetch_value(meta->member_id);
    return fs.storage->value(meta->member_id);
  }

  [[nodiscard]] bool contains(std::string_view field) const {
    if (is_small()) {
      return read_small([&](const HashListpack& lp) {
        return lp.contains(field, options().string_encoding);
      });
    }
    const auto& fs = full();
    if (fs.rt != nullptr) {
      return fs.rt->find(field) != nullptr;
    }
    return fs.fields.find(field) != nullptr;
  }

  // HDEL one field. Returns true if it was present and removed.
  bool erase(std::string_view field) {
    if (is_small()) {
      return mutate_small([&](HashListpack& lp) {
        return lp.erase(field, options().string_encoding);
      });
    }
    if (!erase_full(field)) {
      return false;
    }
    maybe_compact();
    maybe_demote_to_small();
    return true;
  }

  // Multi-field HDEL performs at most one bounded maintenance step for the
  // whole atomic command, regardless of its argument count.
  std::size_t erase_many(std::span<const std::string_view> fields) {
    std::size_t removed = 0;
    if (is_small()) {
      return mutate_small([&](HashListpack& lp) {
        std::size_t compact_removed = 0;
        for (const auto field : fields) {
          compact_removed +=
              lp.erase(field, options().string_encoding) ? 1 : 0;
        }
        return compact_removed;
      });
    }
    for (const auto field : fields) {
      removed += erase_full(field) ? 1 : 0;
    }
    maybe_compact();
    maybe_demote_to_small();
    return removed;
  }

  [[nodiscard]] HashStorage::CompactionStepResult compact_step(
      std::size_t work_budget = 256,
      std::size_t byte_budget = std::size_t{64} << 10) {
    if (is_small()) {
      return {};
    }
    return full().storage->compact_step(work_budget, byte_budget);
  }

  // Iterate every (field, value). Listpack order is insertion order; full form
  // is dense id order. Callers must not rely on either.
  template <class Fn>
  void for_each(Fn&& fn) const {
    if (is_small()) {
      read_small([&](const HashListpack& lp) {
        lp.for_each(std::forward<Fn>(fn), options().string_encoding);
      });
      return;
    }
    const auto n = static_cast<std::uint32_t>(full().storage->size());
    for (std::uint32_t id = 0; id < n; ++id) {
      fn(full().storage->view(id), full().storage->value(id));
    }
  }

  // Dense field-id access for HRANDFIELD. The callback is invoked while the
  // listpack/full-storage views are valid and receives (field, value).
  template <class Fn>
  bool at(std::size_t index, Fn&& fn) const {
    if (index >= size()) {
      return false;
    }
    if (is_small()) {
      read_small([&](const HashListpack& lp) {
        lp.for_range(index, 1, std::forward<Fn>(fn),
                     options().string_encoding);
      });
      return true;
    }
    const auto id = static_cast<std::uint32_t>(index);
    fn(full().storage->view(id), full().storage->value(id));
    return true;
  }

  // HSCAN cursor is a dense field id in either representation. MATCH filters
  // after consuming work, so COUNT remains a bounded work hint even when a page
  // returns no fields.
  template <class MatchFn, class EmitFn>
  std::uint64_t scan(std::uint64_t cursor, std::size_t count, MatchFn&& match,
                     EmitFn&& emit) const {
    const auto n = size();
    if (n == 0 || cursor >= n) {
      return 0;
    }
    const auto budget = count == 0 ? std::size_t{10} : count;
    const auto first = static_cast<std::size_t>(cursor);
    const auto take = std::min(budget, n - first);
    const auto visit = [&](std::string_view field, EncodedStringView value) {
      if (match(field)) {
        emit(field, value);
      }
    };
    if (is_small()) {
      read_small([&](const HashListpack& lp) {
        lp.for_range(first, take, visit, options().string_encoding);
      });
    } else {
      const auto& storage = *full().storage;
      for (std::size_t id = first; id < first + take; ++id) {
        visit(storage.view(static_cast<std::uint32_t>(id)),
              storage.value(static_cast<std::uint32_t>(id)));
      }
    }
    const auto next = first + take;
    return next == n ? 0 : static_cast<std::uint64_t>(next);
  }

  // Compact the arena in place so frozen HugeTLB-backed blocks survive, then
  // rebuild only the field index at the requested density. The full form may
  // demote back to listpack when small enough.
  void compact(double field_index_density = kDefaultFieldIndexDensity) {
    if (is_small()) {
      return;  // listpack has no fragmentation
    }
    auto& fs = full();
    fs.storage->compact();
    if (implementation() == HashImplementation::Realtime) {
      // The packed storage keeps field ids stable, so the linear index remains
      // valid. Its density follows the split cursor and has no table-wide
      // packing operation to perform.
      return;
    }
    const auto n = static_cast<std::uint32_t>(fs.storage->size());
    SelectableMemberIndex<HashStorage> new_index(
        fs.storage.get(), options().member_index_growth,
        member_index_implementation(), realtime_arena());
    new_index.reserve_for_density(n, field_index_density);
    for (std::uint32_t id = 0; id < n; ++id) {
      new_index.insert_packed(fs.storage->view(id),
                              ZSetMemberMeta{.member_id = id});
    }
    fs.fields = std::move(new_index);
    fs.fields.set_members(fs.storage.get());
    fs.bind_rt();
    maybe_demote_to_small();
  }

  [[nodiscard]] HashMemoryStats memory_stats() const noexcept {
    HashMemoryStats stats;
    stats.implementation = implementation();
    stats.field_count = size();
    if (is_small()) {
      const auto bytes = compact_blob_bytes();
      stats.field_value_live_bytes = bytes;
      stats.field_value_allocated_bytes = bytes;
      stats.keyspace_accounted_bytes = bytes;
      stats.total_allocated_bytes = bytes;
      return stats;
    }
    const auto& fs = full();
    stats.field_value_live_bytes = fs.storage->live_bytes();
    stats.field_value_dead_bytes = fs.storage->dead_bytes();
    stats.field_value_allocated_bytes = fs.storage->allocated_bytes();
    stats.field_index_allocated_bytes = fs.fields.allocated_bytes();
    const auto progress = fs.storage->compaction_progress();
    stats.field_compaction_active = progress.active() ? 1 : 0;
    stats.field_compaction_victim_chunk =
        progress.active() ? progress.victim_chunk : 0;
    stats.field_compaction_fields_scanned = progress.fields_scanned;
    stats.field_compaction_fields_total = progress.fields_total;
    stats.field_compaction_candidates_remaining =
        progress.candidates_remaining;
    stats.field_compaction_relocated_fields = progress.relocated_fields;
    stats.field_compaction_relocated_bytes = progress.relocated_bytes;
    stats.field_compaction_selection_nanoseconds =
        progress.selection_nanoseconds;
    stats.field_compaction_densify_nanoseconds = progress.densify_nanoseconds;
    stats.field_compaction_donor_nanoseconds = progress.donor_nanoseconds;
    stats.field_compaction_tail_settle_nanoseconds =
        progress.tail_settle_nanoseconds;
    stats.hash_heap_allocated_bytes = full_heap_allocated_bytes();
    stats.total_allocated_bytes =
        stats.field_value_allocated_bytes + stats.field_index_allocated_bytes +
        stats.hash_heap_allocated_bytes;
    return stats;
  }

  // 0 while listpack (no swiss index); full form reports capacity.
  [[nodiscard]] std::size_t field_index_capacity() const noexcept {
    if (is_small()) {
      return 0;
    }
    return full().fields.capacity();
  }

  // Snapshot. Canonical layer is always (field, value) pairs -- the portable
  // "unpacked table". Accelerator (swiss dump) only for the full form.
  void save(snapshot::Writer& writer, bool with_accelerator) const {
    writer.u8(implementation() == HashImplementation::Realtime ? 1 : 0);
    writer.f64(options().member_index_growth);
    writer.u64(static_cast<std::uint64_t>(size()));
    for_each([&writer](std::string_view field, EncodedStringView value) {
      writer.str(field);
      writer.str(value.to_string());
    });
    const bool write_accel = with_accelerator && !is_small() &&
                             full().fields.supports_accelerator();
    writer.u8(write_accel ? 1 : 0);
    if (write_accel) {
      full().fields.write_accelerator(writer);
    }
  }

  [[nodiscard]] static Hash load(snapshot::Reader& reader, bool use_accelerator,
                                 HashOptions options = {},
                                 HashImplementation implementation =
                                     HashImplementation::Efficient,
                                 bool read_saved_implementation = true) {
    if (read_saved_implementation) {
      const auto saved = reader.u8();
      if (saved > 1) {
        throw snapshot::snapshot_error("invalid hash implementation");
      }
      implementation = saved == 1 ? HashImplementation::Realtime
                                  : HashImplementation::Efficient;
    }
    options.member_index_growth = reader.f64();
    Hash hash(options, implementation);
    const auto field_count = static_cast<std::uint32_t>(reader.u64());

    // Stream pairs: prefer listpack when small; set() promotes if a blob limit
    // trips. Accelerator flag follows the pairs.
    if (options.listpack_max_entries > 0 &&
        field_count <= options.listpack_max_entries) {
      for (std::uint32_t id = 0; id < field_count; ++id) {
        const auto field = reader.str();
        const auto value = reader.str();
        (void)hash.set(field, value);
      }
    } else {
      hash.ensure_full();
      hash.full().storage->reserve(field_count);
      hash.full().fields.reserve_for_density(field_count,
                                             kDefaultFieldIndexDensity);
      for (std::uint32_t id = 0; id < field_count; ++id) {
        const auto field = reader.str();
        const auto value = reader.str();
        const auto field_id = hash.full().storage->push_back(field, value);
        hash.full().fields.insert_packed(
            hash.full().storage->view(field_id),
            ZSetMemberMeta{.member_id = field_id});
      }
    }

    const bool accel = reader.u8() != 0;
    if (accel && use_accelerator) {
      hash.ensure_full();
      hash.full().fields.read_accelerator(reader, hash.full().storage.get());
    } else if (accel) {
      // Consume accelerator bytes without trusting them; keep the index we built.
      hash.ensure_full();
      MemberIndex<HashStorage> discard(hash.full().storage.get(),
                                       options.member_index_growth);
      discard.read_accelerator(reader, hash.full().storage.get());
    }
    return hash;
  }

 private:
  using RealtimeFieldIndex = LinearHashIndex<HashStorage, ZSetMemberMeta>;

  struct FullState {
    std::unique_ptr<HashStorage> storage;
    SelectableMemberIndex<HashStorage> fields;
    // Non-owning cache of fields.realtime() for monomorphic RT hot paths (C).
    // Null when the hash uses the efficient Swiss index.
    RealtimeFieldIndex* rt{nullptr};

    void bind_rt() noexcept { rt = fields.realtime(); }
  };

  static constexpr std::uintptr_t kFullRepresentation = 1;
  static constexpr std::uintptr_t kOwnContext = 2;
  static constexpr std::uintptr_t kRealtimeImplementation = 4;
  static constexpr std::uintptr_t kFlagMask =
      kFullRepresentation | kOwnContext | kRealtimeImplementation;
  static constexpr std::uintptr_t kEmptyCompact =
      std::numeric_limits<std::uintptr_t>::max();

  static void normalize_options(HashOptions& options) noexcept {
    if (options.compaction_work_budget == 0) {
      options.compaction_work_budget = 1;
    }
  }

  [[nodiscard]] static constexpr std::uintptr_t implementation_flag(
      HashImplementation implementation) noexcept {
    return implementation == HashImplementation::Realtime
               ? kRealtimeImplementation
               : 0;
  }

  [[nodiscard]] MemberIndexImplementation member_index_implementation()
      const noexcept {
    return implementation() == HashImplementation::Realtime
               ? MemberIndexImplementation::Realtime
               : MemberIndexImplementation::Efficient;
  }

  [[nodiscard]] std::shared_ptr<LinearHashArena<ZSetMemberMeta>>
  realtime_arena() {
    return implementation() == HashImplementation::Realtime
               ? context().ensure_realtime_arena()
               : nullptr;
  }

  [[nodiscard]] HashContext& context() noexcept {
    assert((context_flags_ & ~kFlagMask) != 0);
    return *reinterpret_cast<HashContext*>(context_flags_ & ~kFlagMask);
  }
  [[nodiscard]] const HashContext& context() const noexcept {
    assert((context_flags_ & ~kFlagMask) != 0);
    return *reinterpret_cast<const HashContext*>(context_flags_ & ~kFlagMask);
  }
  [[nodiscard]] bool owns_context() const noexcept {
    return (context_flags_ & kOwnContext) != 0;
  }

  [[nodiscard]] FullState& full() noexcept {
    assert(!is_small());
    return *reinterpret_cast<FullState*>(rep_);
  }
  [[nodiscard]] const FullState& full() const noexcept {
    assert(!is_small());
    return *reinterpret_cast<const FullState*>(rep_);
  }

  [[nodiscard]] static std::uintptr_t pack_location(
      KeyspaceStorage::TailLocation location) noexcept {
    return (static_cast<std::uintptr_t>(location.block) << 32) |
           location.offset;
  }

  [[nodiscard]] KeyspaceStorage::TailLocation compact_location() const noexcept {
    assert(is_small() && rep_ != kEmptyCompact);
    return {.block = static_cast<std::uint32_t>(rep_ >> 32),
            .offset = static_cast<std::uint32_t>(rep_)};
  }

  [[nodiscard]] std::size_t compact_blob_bytes() const noexcept {
    if (!is_small() || rep_ == kEmptyCompact) {
      return 0;
    }
    return HashListpack::blob_bytes(
        context().storage->blob_data(compact_location()));
  }

  [[nodiscard]] std::string_view compact_blob_view() const noexcept {
    const auto bytes = compact_blob_bytes();
    return bytes == 0
               ? std::string_view{}
               : std::string_view(
                     context().storage->blob_data(compact_location()), bytes);
  }

  class ArenaBlobStorage final : public HashListpackBlobStorage {
   public:
    ArenaBlobStorage(KeyspaceStorage& arena,
                     KeyspaceStorage::TailLocation old_location,
                     char* old_data) noexcept
        : arena_(arena), current_location_(old_location), current_data_(old_data) {}

    explicit ArenaBlobStorage(KeyspaceStorage& arena) noexcept
        : arena_(arena) {}

    [[nodiscard]] char* allocate(std::size_t bytes) override {
      assert(pending_data_ == nullptr);
      if (current_data_ != nullptr) {
        pinned_block_ = arena_.pin_blob(current_location_);
      }
      const auto allocation = arena_.reserve_blob(bytes);
      if (current_data_ == nullptr) {
        current_location_ = allocation.location;
        current_data_ = allocation.data;
        return current_data_;
      }
      pending_location_ = allocation.location;
      pending_data_ = allocation.data;
      return pending_data_;
    }

    void deallocate(char* p, std::size_t bytes) noexcept override {
      assert(bytes <= std::numeric_limits<std::uint16_t>::max());
      assert(p == current_data_);
      (void)p;
      arena_.mark_object_blob_dead(static_cast<std::uint16_t>(bytes));
      current_data_ = pending_data_;
      current_location_ = pending_location_;
      pending_data_ = nullptr;
      pinned_block_.reset();
    }

    [[nodiscard]] std::uintptr_t packed_location_for(
        const char* p) const noexcept {
      if (p == nullptr) {
        return kEmptyCompact;
      }
      assert(p == current_data_);
      return pack_location(current_location_);
    }

   private:
    KeyspaceStorage& arena_;
    KeyspaceStorage::TailLocation current_location_{};
    KeyspaceStorage::TailLocation pending_location_{};
    char* current_data_{nullptr};
    char* pending_data_{nullptr};
    std::shared_ptr<char[]> pinned_block_;
  };

  class SmallSession {
   public:
    explicit SmallSession(Hash& hash)
        : hash_(hash),
          storage_(*hash.context().storage,
                   hash.rep_ == kEmptyCompact
                       ? KeyspaceStorage::TailLocation{}
                       : hash.compact_location(),
                   hash.rep_ == kEmptyCompact
                       ? nullptr
                       : hash.context().storage->mutable_blob_data(
                             hash.compact_location())),
          listpack_(hash.rep_ == kEmptyCompact
                        ? nullptr
                        : hash.context().storage->mutable_blob_data(
                              hash.compact_location()),
                    storage_) {}

    ~SmallSession() {
      hash_.rep_ = storage_.packed_location_for(listpack_.data());
    }

    [[nodiscard]] HashListpack& listpack() noexcept { return listpack_; }

   private:
    Hash& hash_;
    ArenaBlobStorage storage_;
    HashListpack listpack_;
  };

  template <class Fn>
  std::invoke_result_t<Fn, const HashListpack&> read_small(Fn&& fn) const {
    assert(is_small());
    auto& arena = *context().storage;
    char* data = rep_ == kEmptyCompact
                     ? nullptr
                     : const_cast<char*>(arena.blob_data(compact_location()));
    ArenaBlobStorage storage(
        arena,
        rep_ == kEmptyCompact ? KeyspaceStorage::TailLocation{}
                              : compact_location(),
        data);
    HashListpack listpack(data, storage);
    return std::forward<Fn>(fn)(static_cast<const HashListpack&>(listpack));
  }

  template <class Fn>
  std::invoke_result_t<Fn, HashListpack&> mutate_small(Fn&& fn) {
    assert(is_small());
    using Result = std::invoke_result_t<Fn, HashListpack&>;
    if constexpr (std::is_void_v<Result>) {
      {
        SmallSession session(*this);
        std::forward<Fn>(fn)(session.listpack());
      }
      maybe_compact_keyspace();
    } else {
      auto result = [&]() -> Result {
        SmallSession session(*this);
        return std::forward<Fn>(fn)(session.listpack());
      }();
      maybe_compact_keyspace();
      return result;
    }
  }

  void maybe_compact_keyspace() {
    auto& ctx = context();
    if (ctx.maybe_compact != nullptr && ctx.storage->should_compact()) {
      ctx.maybe_compact(ctx.compact_owner);
    }
  }

  void init_empty() {
    assert(context_flags_ != 0);
    rep_ = kEmptyCompact;
    context_flags_ &= ~kFullRepresentation;
    if (options().listpack_max_entries == 0 ||
        implementation() == HashImplementation::Realtime) {
      auto storage = std::make_unique<HashStorage>(options().chunk_bytes,
                                                   options().member_index_growth,
                                                   options().compaction_knapsack,
                                                   options().string_encoding);
      SelectableMemberIndex<HashStorage> fields(
          storage.get(), options().member_index_growth,
          member_index_implementation(), realtime_arena());
      auto* state =
          new FullState{std::move(storage), std::move(fields), nullptr};
      state->bind_rt();
      rep_ = reinterpret_cast<std::uintptr_t>(state);
      context_flags_ |= kFullRepresentation;
    }
  }

  void ensure_full() {
    if (!is_small()) {
      return;
    }
    auto storage = std::make_unique<HashStorage>(options().chunk_bytes,
                                                 options().member_index_growth,
                                                 options().compaction_knapsack,
                                                 options().string_encoding);
    SelectableMemberIndex<HashStorage> fields(
        storage.get(), options().member_index_growth,
        member_index_implementation(), realtime_arena());
    const auto n = size();
    storage->reserve(n);
    fields.reserve_for_density(n, kDefaultFieldIndexDensity);
    read_small([&](const HashListpack& blob) {
      blob.for_each(
        [&](std::string_view field, EncodedStringView value) {
          const auto id = storage->push_back(field, value.to_string());
          fields.insert_packed(storage->view(id),
                               ZSetMemberMeta{.member_id = id});
        },
        options().string_encoding);
    });
    auto* state =
        new FullState{std::move(storage), std::move(fields), nullptr};
    state->bind_rt();
    if (rep_ != kEmptyCompact) {
      context().storage->mark_object_blob_dead(
          static_cast<std::uint16_t>(compact_blob_bytes()));
    }
    rep_ = reinterpret_cast<std::uintptr_t>(state);
    context_flags_ |= kFullRepresentation;
    maybe_compact_keyspace();
  }

  void maybe_demote_to_small() {
    if (is_small()) {
      return;
    }
    if (options().listpack_max_entries == 0 ||
        implementation() == HashImplementation::Realtime) {
      return;
    }
    const auto n = full().storage->size();
    if (n > options().listpack_max_entries) {
      return;
    }
    // Avoid attempting an O(N) rebuild after every HDEL while a large hash is
    // still far above the 64 KiB listpack ceiling.
    if (!HashListpack::can_encode(n, full().storage->live_bytes())) {
      return;
    }
    auto& fs = full();
    std::vector<std::string> values;
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
    values.reserve(n);
    pairs.reserve(n);
    for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(n); ++id) {
      values.push_back(fs.storage->value(id).to_string());
      pairs.emplace_back(fs.storage->view(id), values.back());
    }
    ArenaBlobStorage arena_storage(*context().storage);
    HashListpack lp(nullptr, arena_storage);
    const auto result = lp.set_many(pairs, options().listpack_max_entries,
                                    options().string_encoding);
    if (result.needs_full) {
      return;
    }
    const auto compact_rep = arena_storage.packed_location_for(lp.data());
    auto* old_state = reinterpret_cast<FullState*>(rep_);
    rep_ = compact_rep;
    context_flags_ &= ~kFullRepresentation;
    delete old_state;
  }

  int set_full(std::string_view field, std::string_view value,
               bool maintain = true) {
    auto& fs = full();
    // Encode runs only inside the insert/update callbacks (D): updates encode
    // in set_value; pure inserts encode in push_back after the slot is reserved.
    const auto on_existing = [&](ZSetMemberMeta& meta) {
      fs.storage->set_value(meta.member_id, value);
    };
    const auto make_meta = [&]() {
      const auto field_id = fs.storage->push_back(field, value);
      return ZSetMemberMeta{.member_id = field_id};
    };
    const bool inserted =
        fs.rt != nullptr
            ? fs.rt->find_or_emplace(field, on_existing, make_meta, maintain)
            : fs.fields.find_or_emplace(field, on_existing, make_meta, maintain);
    return inserted ? 1 : 0;
  }

  bool erase_full(std::string_view field) {
    auto& fs = full();
    std::optional<std::size_t> slot;
    if (fs.rt != nullptr) {
      slot = fs.rt->find_slot(field);
    } else {
      slot = fs.fields.find_slot(field);
    }
    if (!slot) {
      return false;
    }
    const auto field_id =
        fs.rt != nullptr ? fs.rt->member_id_at(*slot)
                         : fs.fields.member_id_at(*slot);
    const auto last_id = static_cast<std::uint32_t>(fs.storage->size() - 1);
    fs.storage->prepare_copy_ref(field_id, last_id);
    fs.storage->orphan(field_id);
    const bool erased =
        fs.rt != nullptr ? fs.rt->erase_at_index(*slot)
                         : fs.fields.erase_at_index(*slot);
    assert(erased);
    (void)erased;
    move_last_field_into_slot(field_id);
    return true;
  }

  void move_last_field_into_slot(std::uint32_t removed_field_id) {
    auto& fs = full();
    const auto last_id = static_cast<std::uint32_t>(fs.storage->size() - 1);
    if (removed_field_id == last_id) {
      fs.storage->pop_back();
      return;
    }
    fs.storage->copy_ref(removed_field_id, last_id);
    const bool moved =
        fs.rt != nullptr
            ? fs.rt->move_member_id(last_id, removed_field_id)
            : fs.fields.move_member_id(last_id, removed_field_id);
    assert(moved);
    (void)moved;
    fs.storage->pop_back();
  }

  // Bounded arena compaction only — never a full compact on the hot HSET path
  // (N). Same-content / same-width updates that leave dead_bytes_ unchanged skip
  // the thresholds below.
  void maybe_compact() {
    if (is_small()) {
      return;
    }
    auto& fs = full();
    if (fs.storage->compaction_active() ||
        (fs.storage->dead_bytes() >= kAutoCompactDeadFloor &&
         fs.storage->dead_bytes() >= fs.storage->live_bytes())) {
      (void)fs.storage->compact_step(options().compaction_work_budget,
                                     kAutoCompactByteBudget);
    }
  }

  void destroy_representation() noexcept {
    if (context_flags_ == 0) {
      return;
    }
    if (is_small()) {
      if (rep_ != kEmptyCompact) {
        context().storage->mark_object_blob_dead(
            static_cast<std::uint16_t>(compact_blob_bytes()));
      }
    } else {
      delete reinterpret_cast<FullState*>(rep_);
    }
    rep_ = kEmptyCompact;
    context_flags_ &= ~kFullRepresentation;
  }

  void destroy_owned_context() noexcept {
    if (context_flags_ == 0 || !owns_context()) {
      context_flags_ = 0;
      return;
    }
    auto* owned = &context();
    delete owned->storage;
    delete owned;
    context_flags_ = 0;
  }

  static void validate_field(std::string_view field) {
    if (field.size() > HashStorage::kMaxFieldBytes) {
      throw std::length_error("hash field too large (max 65,535 bytes)");
    }
  }

  [[nodiscard]] static constexpr std::size_t
  full_heap_allocated_bytes() noexcept {
    return sizeof(FullState) + sizeof(HashStorage);
  }

  static constexpr std::size_t kAutoCompactDeadFloor = std::size_t{1} << 20;
  static constexpr std::size_t kAutoCompactByteBudget = std::size_t{16} << 10;

  std::uintptr_t context_flags_{0};
  std::uintptr_t rep_{kEmptyCompact};
};
static_assert(sizeof(Hash) == 16);

}  // namespace goblin::core
