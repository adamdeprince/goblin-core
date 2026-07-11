#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "goblin/core/hash_listpack.hpp"
#include "goblin/core/hash_storage.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/zset_member_index.hpp"

namespace goblin::core {

// A hash uses the growth knob for the full form, plus a listpack threshold for
// tiny tables (same knee as zsets: 32 by default; 0 disables listpack).
struct HashOptions {
  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t chunk_bytes{HashStorage::kDefaultChunkBytes};
  // Max fields kept as a compact listpack before promoting to swiss+arena.
  // 0 disables the listpack (always full). 32 matches zset_listpack_max_entries.
  std::size_t listpack_max_entries{32};
};

struct HashMemoryStats {
  std::size_t field_count{0};
  std::size_t field_value_live_bytes{0};
  std::size_t field_value_dead_bytes{0};
  std::size_t field_value_allocated_bytes{0};
  std::size_t field_index_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
};

// A Redis hash: field->value, both arbitrary byte strings (<= 64 KiB each).
// Tiny hashes live as one HashListpack blob (linear scan); past the threshold
// they promote to a swiss field index + packed HashStorage (same dual as zsets).
class Hash {
 public:
  static constexpr double kDefaultFieldIndexDensity = 0.97;

  explicit Hash(HashOptions options = {}) : options_(options) {
    init_empty();
  }

  Hash(const Hash&) = delete;
  Hash& operator=(const Hash&) = delete;

  Hash(Hash&& other) noexcept
      : options_(other.options_), rep_(std::move(other.rep_)) {}
  Hash& operator=(Hash&& other) noexcept {
    if (this != &other) {
      options_ = other.options_;
      rep_ = std::move(other.rep_);
    }
    return *this;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->size();
    }
    return full().storage->size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] const HashOptions& options() const noexcept { return options_; }
  [[nodiscard]] bool is_small() const noexcept {
    return std::holds_alternative<HashListpack>(rep_);
  }

  // Pre-size the full form for an upcoming bulk insert. No-op while listpack
  // (promotion happens on the set that crosses the threshold).
  void reserve_additional(std::size_t additional) {
    if (additional == 0 || is_small()) {
      return;
    }
    const auto n = size() + additional;
    full().storage->reserve(n);
    full().fields.reserve(n);
  }

  // Empty the hash in place (keeps the Hash object for freelist reuse).
  void clear() { init_empty(); }

  // HSET one field. Returns 1 if the field is new, 0 if it updated an existing
  // field's value.
  int set(std::string_view field, std::string_view value) {
    if (auto* lp = small_ptr()) {
      const auto result =
          lp->set(field, value, options_.listpack_max_entries);
      if (!result.needs_full) {
        return result.added ? 1 : 0;
      }
      ensure_full();
    }
    return set_full(field, value);
  }

  // HSETNX. Returns 1 if the field was set, 0 if it already existed.
  int set_nx(std::string_view field, std::string_view value) {
    if (auto* lp = small_ptr()) {
      if (!lp->absent(field)) {
        return 0;
      }
      const auto result =
          lp->set(field, value, options_.listpack_max_entries);
      if (!result.needs_full) {
        return result.added ? 1 : 0;
      }
      ensure_full();
      // After promote the field is still absent; fall through to full set_nx.
    }
    auto& fs = full();
    if (fs.fields.find(field) != nullptr) {
      return 0;
    }
    const auto field_id = fs.storage->push_back(field, value);
    fs.fields.insert_absent(fs.storage->view(field_id),
                            ZSetMemberMeta{.member_id = field_id});
    return 1;
  }

  [[nodiscard]] std::optional<std::string_view> get(std::string_view field) const {
    if (const auto* lp = small_ptr()) {
      return lp->get(field);
    }
    const auto* meta = full().fields.find(field);
    if (meta == nullptr) {
      return std::nullopt;
    }
    return full().storage->value(meta->member_id);
  }

  [[nodiscard]] bool contains(std::string_view field) const {
    if (const auto* lp = small_ptr()) {
      return lp->contains(field);
    }
    return full().fields.find(field) != nullptr;
  }

  // HDEL one field. Returns true if it was present and removed.
  bool erase(std::string_view field) {
    if (auto* lp = small_ptr()) {
      return lp->erase(field);
    }
    auto& fs = full();
    auto* meta = fs.fields.find(field);
    if (meta == nullptr) {
      return false;
    }
    const auto field_id = meta->member_id;
    fs.storage->orphan(field_id);
    const bool erased = fs.fields.erase(field);
    assert(erased);
    move_last_field_into_slot(field_id);
    maybe_compact();
    maybe_demote_to_small();
    return erased;
  }

  // Iterate every (field, value). Listpack order is insertion order; full form
  // is dense id order. Callers must not rely on either.
  template <class Fn>
  void for_each(Fn&& fn) const {
    if (const auto* lp = small_ptr()) {
      lp->for_each(std::forward<Fn>(fn));
      return;
    }
    const auto n = static_cast<std::uint32_t>(full().storage->size());
    for (std::uint32_t id = 0; id < n; ++id) {
      fn(full().storage->view(id), full().storage->value(id));
    }
  }

  // Rebuild: listpack re-packs for free (no dead bytes); full form drops
  // orphaned arena bytes and may demote back to listpack when small enough.
  void compact(double field_index_density = kDefaultFieldIndexDensity) {
    if (is_small()) {
      return;  // listpack has no fragmentation
    }
    auto& fs = full();
    const auto n = static_cast<std::uint32_t>(fs.storage->size());
    auto new_storage = std::make_unique<HashStorage>(
        fs.storage->chunk_bytes(), options_.member_index_growth);
    new_storage->reserve(n);
    MemberIndex<HashStorage> new_index(new_storage.get(),
                                       options_.member_index_growth);
    new_index.reserve_for_density(n, field_index_density);
    for (std::uint32_t id = 0; id < n; ++id) {
      const auto new_id =
          new_storage->push_back(fs.storage->view(id), fs.storage->value(id));
      new_index.insert_packed(new_storage->view(new_id),
                              ZSetMemberMeta{.member_id = new_id});
    }
    fs.storage = std::move(new_storage);
    fs.fields = std::move(new_index);
    fs.fields.set_members(fs.storage.get());
    maybe_demote_to_small();
  }

  [[nodiscard]] HashMemoryStats memory_stats() const noexcept {
    HashMemoryStats stats;
    stats.field_count = size();
    if (const auto* lp = small_ptr()) {
      stats.field_value_live_bytes = lp->allocated_bytes();
      stats.field_value_allocated_bytes = lp->allocated_bytes();
      stats.total_allocated_bytes = lp->allocated_bytes();
      return stats;
    }
    const auto& fs = full();
    stats.field_value_live_bytes = fs.storage->live_bytes();
    stats.field_value_dead_bytes = fs.storage->dead_bytes();
    stats.field_value_allocated_bytes = fs.storage->allocated_bytes();
    stats.field_index_allocated_bytes = fs.fields.allocated_bytes();
    stats.total_allocated_bytes =
        stats.field_value_allocated_bytes + stats.field_index_allocated_bytes;
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
    writer.f64(options_.member_index_growth);
    writer.u64(static_cast<std::uint64_t>(size()));
    for_each([&writer](std::string_view field, std::string_view value) {
      writer.str(field);
      writer.str(value);
    });
    const bool write_accel = with_accelerator && !is_small();
    writer.u8(write_accel ? 1 : 0);
    if (write_accel) {
      full().fields.write_accelerator(writer);
    }
  }

  [[nodiscard]] static Hash load(snapshot::Reader& reader, bool use_accelerator,
                                 HashOptions options = {}) {
    options.member_index_growth = reader.f64();
    Hash hash(options);
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
  struct FullState {
    std::unique_ptr<HashStorage> storage;
    MemberIndex<HashStorage> fields;
  };

  [[nodiscard]] HashListpack* small_ptr() noexcept {
    return std::get_if<HashListpack>(&rep_);
  }
  [[nodiscard]] const HashListpack* small_ptr() const noexcept {
    return std::get_if<HashListpack>(&rep_);
  }
  [[nodiscard]] FullState& full() noexcept {
    return *std::get<std::unique_ptr<FullState>>(rep_);
  }
  [[nodiscard]] const FullState& full() const noexcept {
    return *std::get<std::unique_ptr<FullState>>(rep_);
  }

  void init_empty() {
    if (options_.listpack_max_entries == 0) {
      auto storage = std::make_unique<HashStorage>(options_.chunk_bytes,
                                                   options_.member_index_growth);
      MemberIndex<HashStorage> fields(storage.get(),
                                      options_.member_index_growth);
      rep_ = std::make_unique<FullState>(
          FullState{std::move(storage), std::move(fields)});
    } else {
      rep_ = HashListpack{};
    }
  }

  void ensure_full() {
    auto* lp = small_ptr();
    if (lp == nullptr) {
      return;
    }
    HashListpack blob = std::move(*lp);
    auto storage = std::make_unique<HashStorage>(options_.chunk_bytes,
                                                 options_.member_index_growth);
    MemberIndex<HashStorage> fields(storage.get(), options_.member_index_growth);
    const auto n = blob.size();
    storage->reserve(n);
    fields.reserve_for_density(n, kDefaultFieldIndexDensity);
    blob.for_each([&](std::string_view field, std::string_view value) {
      const auto id = storage->push_back(field, value);
      fields.insert_packed(storage->view(id), ZSetMemberMeta{.member_id = id});
    });
    rep_ = std::make_unique<FullState>(
        FullState{std::move(storage), std::move(fields)});
  }

  void maybe_demote_to_small() {
    if (is_small()) {
      return;
    }
    if (options_.listpack_max_entries == 0) {
      return;
    }
    const auto n = full().storage->size();
    if (n > options_.listpack_max_entries) {
      return;
    }
    HashListpack lp;
    auto& fs = full();
    for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(n); ++id) {
      const auto result = lp.set(fs.storage->view(id), fs.storage->value(id),
                                 options_.listpack_max_entries);
      if (result.needs_full) {
        return;
      }
    }
    rep_ = std::move(lp);
  }

  int set_full(std::string_view field, std::string_view value) {
    auto& fs = full();
    const bool inserted = fs.fields.find_or_emplace(
        field,
        [&](ZSetMemberMeta& meta) {
          fs.storage->set_value(meta.member_id, value);
          maybe_compact();
        },
        [&]() {
          const auto field_id = fs.storage->push_back(field, value);
          return ZSetMemberMeta{.member_id = field_id};
        });
    return inserted ? 1 : 0;
  }

  void move_last_field_into_slot(std::uint32_t removed_field_id) {
    auto& fs = full();
    const auto last_id = static_cast<std::uint32_t>(fs.storage->size() - 1);
    if (removed_field_id == last_id) {
      fs.storage->pop_back();
      return;
    }
    fs.storage->copy_ref(removed_field_id, last_id);
    const bool moved = fs.fields.move_member_id(last_id, removed_field_id);
    assert(moved);
    (void)moved;
    fs.storage->pop_back();
  }

  void maybe_compact() {
    if (is_small()) {
      return;
    }
    auto& fs = full();
    if (fs.storage->dead_bytes() >= kAutoCompactDeadFloor &&
        fs.storage->dead_bytes() >= fs.storage->live_bytes()) {
      compact();
    }
  }

  static constexpr std::size_t kAutoCompactDeadFloor = std::size_t{1} << 20;

  HashOptions options_;
  std::variant<HashListpack, std::unique_ptr<FullState>> rep_;
};

}  // namespace goblin::core
