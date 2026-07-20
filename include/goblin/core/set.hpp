#pragma once

// Redis SET: unique members. Tiny sets live in a fingerprint-indexed listpack
// (hash listpack without values). Larger ones promote to a Swiss MemberIndex
// over encoded member bytes with the zset-aggressive parameters (97% density,
// 2^0.25 growth). Members are always string-encoded (compact ints, UUIDs, raw).

#include <algorithm>
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
#include <utility>
#include <variant>
#include <vector>

#include "goblin/core/set_listpack.hpp"
#include "goblin/core/set_storage.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/string_encoding.hpp"
#include "goblin/core/zset_member_index.hpp"

namespace goblin::core {

struct SetOptions {
  static constexpr std::size_t kDefaultListpackMaxEntries = 32;

  double member_index_growth{ZSetMemberIndex::kDefaultGrowth};
  std::size_t chunk_bytes{SetStorage::kDefaultChunkBytes};
  StringEncodingOptions string_encoding{};
  // 0 disables the compact form (always Swiss). Default matches lists/zsets.
  std::size_t listpack_max_entries{kDefaultListpackMaxEntries};
};

struct SetMemoryStats {
  std::size_t member_count{0};
  std::size_t member_live_bytes{0};
  std::size_t member_dead_bytes{0};
  std::size_t member_allocated_bytes{0};
  std::size_t member_index_capacity{0};
  std::size_t member_index_tombstones{0};
  std::size_t member_index_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
  bool is_listpack{false};
};

// Contiguous encoded probe key for the Swiss table.
class EncodedMemberProbe {
 public:
  EncodedMemberProbe(std::string_view member, StringEncodingOptions options)
      : encoded_(member, options) {
    if (encoded_.size() <= sizeof(stack_)) {
      encoded_.write_to(stack_);
      view_ = std::string_view(stack_, encoded_.size());
      return;
    }
    heap_.resize(encoded_.size());
    encoded_.write_to(heap_.data());
    view_ = std::string_view(heap_.data(), encoded_.size());
  }

  [[nodiscard]] std::string_view view() const noexcept { return view_; }
  [[nodiscard]] const EncodedString& encoded() const noexcept {
    return encoded_;
  }

 private:
  EncodedString encoded_;
  alignas(8) char stack_[32]{};
  std::string heap_;
  std::string_view view_;
};

class Set {
 public:
  static constexpr double kDefaultMemberIndexDensity = 0.97;

  explicit Set(SetOptions options = {}) : options_(std::move(options)) {
    init_empty();
  }

  Set(const Set&) = delete;
  Set& operator=(const Set&) = delete;
  Set(Set&&) noexcept = default;
  Set& operator=(Set&&) noexcept = default;

  [[nodiscard]] const SetOptions& options() const noexcept { return options_; }
  [[nodiscard]] bool is_small() const noexcept {
    return std::holds_alternative<SetListpack>(rep_);
  }
  [[nodiscard]] std::size_t size() const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->size();
    }
    return full().storage->size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  void reserve_additional(std::size_t additional) {
    if (additional == 0 || is_small()) {
      return;
    }
    full().storage->reserve_additional(additional);
    full().members.reserve_additional(additional);
  }

  int add(std::string_view member) {
    validate_member(member);
    if (auto* lp = small_ptr()) {
      const auto result =
          lp->add(member, options_.listpack_max_entries, options_.string_encoding);
      if (!result.needs_full) {
        return result.added ? 1 : 0;
      }
      ensure_full();
    }
    return add_full(member);
  }

  long long add_many(std::span<const std::string_view> members) {
    if (members.empty()) {
      return 0;
    }
    for (const auto member : members) {
      validate_member(member);
    }
    if (auto* lp = small_ptr()) {
      const auto result = lp->add_many(members, options_.listpack_max_entries,
                                       options_.string_encoding);
      if (!result.needs_full) {
        return result.added;
      }
      ensure_full();
    }
    reserve_additional(members.size());
    long long added = 0;
    for (const auto member : members) {
      added += add_full(member);
    }
    return added;
  }

  [[nodiscard]] bool contains(std::string_view member) const {
    if (!string_value_fits(member, options_.string_encoding)) {
      return false;
    }
    if (const auto* lp = small_ptr()) {
      return lp->contains(member, options_.string_encoding);
    }
    EncodedMemberProbe probe(member, options_.string_encoding);
    return full().members.find(probe.view()) != nullptr;
  }

  bool erase(std::string_view member) {
    if (!string_value_fits(member, options_.string_encoding)) {
      return false;
    }
    if (auto* lp = small_ptr()) {
      return lp->erase(member, options_.string_encoding);
    }
    if (!erase_full(member)) {
      return false;
    }
    maybe_demote_to_small();
    return true;
  }

  std::size_t erase_many(std::span<const std::string_view> members) {
    std::size_t removed = 0;
    if (is_small()) {
      auto* lp = small_ptr();
      for (const auto member : members) {
        removed +=
            lp->erase(member, options_.string_encoding) ? 1 : 0;
      }
      return removed;
    }
    for (const auto member : members) {
      removed += erase_full(member) ? 1 : 0;
    }
    if (removed != 0) {
      maybe_demote_to_small();
    }
    return removed;
  }

  bool erase_at_id(std::uint32_t member_id) {
    if (auto* lp = small_ptr()) {
      return lp->erase_at_index(member_id);
    }
    if (member_id >= full().storage->size()) {
      return false;
    }
    const auto slot = full().members.find_slot(full().storage->view(member_id));
    if (!slot) {
      return false;
    }
    const auto last_id =
        static_cast<std::uint32_t>(full().storage->size() - 1);
    full().storage->orphan(member_id);
    const bool erased = full().members.erase_at_index(*slot);
    assert(erased);
    (void)erased;
    if (member_id != last_id) {
      full().storage->copy_ref(member_id, last_id);
      const bool moved = full().members.move_member_id(last_id, member_id);
      assert(moved);
      (void)moved;
    }
    full().storage->pop_back();
    (void)full().members.cleanup_after_removal_if_needed(1);
    maybe_compact_arena();
    maybe_demote_to_small();
    return true;
  }

  void clear() { init_empty(); }

  template <class Fn>
  void for_each(Fn&& fn) const {
    if (const auto* lp = small_ptr()) {
      lp->for_each(std::forward<Fn>(fn), options_.string_encoding);
      return;
    }
    const auto n = static_cast<std::uint32_t>(full().storage->size());
    for (std::uint32_t id = 0; id < n; ++id) {
      fn(full().storage->encoded_view(id));
    }
  }

  [[nodiscard]] EncodedStringView at(std::uint32_t member_id) const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->at(member_id, options_.string_encoding);
    }
    return full().storage->encoded_view(member_id);
  }

  [[nodiscard]] std::optional<std::string> random_member() const {
    if (empty()) {
      return std::nullopt;
    }
    return at(random_id()).to_string();
  }

  [[nodiscard]] std::vector<std::string> random_members(std::size_t count,
                                                        bool unique) const {
    std::vector<std::string> out;
    if (count == 0 || empty()) {
      return out;
    }
    if (!unique) {
      out.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        out.push_back(at(random_id()).to_string());
      }
      return out;
    }
    const auto n = size();
    if (count >= n) {
      out.reserve(n);
      for_each([&](EncodedStringView member) {
        out.push_back(member.to_string());
      });
      for (std::size_t i = out.size(); i > 1; --i) {
        const auto j = static_cast<std::size_t>(random_bounded(i));
        std::swap(out[i - 1], out[j]);
      }
      return out;
    }
    out.reserve(count);
    std::vector<std::uint32_t> ids(n);
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
      ids[i] = i;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto j = i + static_cast<std::size_t>(random_bounded(n - i));
      std::swap(ids[i], ids[j]);
      out.push_back(at(ids[i]).to_string());
    }
    return out;
  }

  [[nodiscard]] std::optional<std::string> pop() {
    if (empty()) {
      return std::nullopt;
    }
    const auto id = random_id();
    auto member = at(id).to_string();
    (void)erase_at_id(id);
    return member;
  }

  [[nodiscard]] std::vector<std::string> pop_many(std::size_t count) {
    std::vector<std::string> out;
    if (count == 0 || empty()) {
      return out;
    }
    const auto take = std::min(count, size());
    out.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
      auto member = pop();
      assert(member.has_value());
      out.push_back(std::move(*member));
    }
    return out;
  }

  template <class MatchFn, class EmitFn>
  std::uint64_t scan(std::uint64_t cursor, std::size_t count, MatchFn&& match,
                     EmitFn&& emit) const {
    const auto n = size();
    if (n == 0 || cursor >= n) {
      return 0;
    }
    const auto budget = count == 0 ? std::size_t{10} : count;
    std::size_t emitted = 0;
    std::uint64_t id = cursor;
    const auto walk_limit = std::max(budget * 10, budget);
    std::size_t walked = 0;
    while (id < n && walked < walk_limit && emitted < budget) {
      const auto logical = at(static_cast<std::uint32_t>(id)).to_string();
      if (match(std::string_view(logical))) {
        emit(std::move(logical));
        ++emitted;
      }
      ++id;
      ++walked;
    }
    return id >= n ? 0 : id;
  }

  void compact(double member_index_density = kDefaultMemberIndexDensity) {
    if (is_small()) {
      return;
    }
    full().storage->compact();
    full().storage->shrink_to_fit();
    rebuild_member_index(member_index_density);
    maybe_demote_to_small();
  }

  [[nodiscard]] SetMemoryStats memory_stats() const noexcept {
    SetMemoryStats stats;
    stats.member_count = size();
    stats.is_listpack = is_small();
    if (const auto* lp = small_ptr()) {
      stats.member_live_bytes = lp->allocated_bytes();
      stats.member_allocated_bytes = lp->allocated_bytes();
      stats.total_allocated_bytes = sizeof(Set) + lp->allocated_bytes();
      return stats;
    }
    stats.member_live_bytes = full().storage->live_bytes();
    stats.member_dead_bytes = full().storage->dead_bytes();
    stats.member_allocated_bytes = full().storage->allocated_bytes();
    stats.member_index_capacity = full().members.capacity();
    stats.member_index_tombstones = full().members.tombstone_count();
    stats.member_index_allocated_bytes = full().members.allocated_bytes();
    stats.total_allocated_bytes =
        stats.member_allocated_bytes + stats.member_index_allocated_bytes +
        sizeof(Set) + sizeof(FullState) + sizeof(SetStorage);
    return stats;
  }

  [[nodiscard]] std::size_t member_index_capacity() const noexcept {
    return is_small() ? 0 : full().members.capacity();
  }

  void save(snapshot::Writer& writer, bool with_accelerator) const {
    writer.f64(options_.member_index_growth);
    const auto member_count = static_cast<std::uint32_t>(size());
    writer.u64(member_count);
    for_each([&writer](EncodedStringView member) {
      writer.str(member.to_string());
    });
    // Accelerator only for the full Swiss form (listpack has no table dump).
    const bool write_accel = with_accelerator && !is_small();
    writer.u8(write_accel ? 1 : 0);
    if (write_accel) {
      writer.u32(options_.string_encoding.packed);
      full().members.write_accelerator(writer);
    }
  }

  [[nodiscard]] static Set load(snapshot::Reader& reader, bool use_accelerator,
                                SetOptions options = {}) {
    (void)reader.f64();
    const auto member_count = static_cast<std::uint32_t>(reader.u64());
    std::vector<std::string> loaded;
    loaded.reserve(member_count);
    for (std::uint32_t id = 0; id < member_count; ++id) {
      loaded.push_back(std::string(reader.str()));
    }
    const bool accelerator_present = reader.u8() != 0;

    Set set(std::move(options));
    const bool prefer_listpack =
        !accelerator_present && set.options_.listpack_max_entries > 0 &&
        member_count <= set.options_.listpack_max_entries;

    if (prefer_listpack) {
      std::vector<std::string_view> views;
      views.reserve(loaded.size());
      for (const auto& m : loaded) {
        views.push_back(m);
      }
      auto* lp = set.small_ptr();
      assert(lp != nullptr);
      const auto result = lp->add_many(views, set.options_.listpack_max_entries,
                                       set.options_.string_encoding);
      if (!result.needs_full) {
        // No accelerator to consume.
        return set;
      }
      // Fall through to full form.
      set.ensure_full();
    } else if (set.is_small()) {
      set.ensure_full();
    }

    set.full().storage->reserve(member_count);
    for (std::uint32_t id = 0; id < member_count; ++id) {
      const auto assigned = set.full().storage->push_back(loaded[id]);
      if (assigned != id) {
        throw snapshot::snapshot_error("set member id assignment mismatch");
      }
    }

    if (accelerator_present) {
      const auto saved_encoding = reader.u32();
      const bool encoding_matches =
          saved_encoding == set.options_.string_encoding.packed;
      if (use_accelerator && encoding_matches) {
        set.full().members.read_accelerator(reader, set.full().storage.get());
        if (set.full().members.size() != member_count) {
          throw snapshot::snapshot_error("snapshot set index size mismatch");
        }
      } else {
        MemberIndex<SetStorage> discard(set.full().storage.get(),
                                        set.options_.member_index_growth);
        discard.read_accelerator(reader, set.full().storage.get());
        set.rebuild_member_index();
      }
    } else {
      set.rebuild_member_index();
    }
    return set;
  }

 private:
  struct FullState {
    std::unique_ptr<SetStorage> storage;
    MemberIndex<SetStorage> members;
  };

  static constexpr std::size_t kAutoCompactDeadFloor = std::size_t{1} << 20;

  [[nodiscard]] SetListpack* small_ptr() noexcept {
    return std::get_if<SetListpack>(&rep_);
  }
  [[nodiscard]] const SetListpack* small_ptr() const noexcept {
    return std::get_if<SetListpack>(&rep_);
  }
  [[nodiscard]] FullState& full() noexcept {
    return *std::get<std::unique_ptr<FullState>>(rep_);
  }
  [[nodiscard]] const FullState& full() const noexcept {
    return *std::get<std::unique_ptr<FullState>>(rep_);
  }

  void init_empty() {
    if (options_.listpack_max_entries == 0) {
      ensure_memory_growth(sizeof(FullState) + sizeof(SetStorage));
      auto storage = std::make_unique<SetStorage>(
          options_.chunk_bytes, kDefaultArenaGrowth, options_.string_encoding);
      MemberIndex<SetStorage> members(storage.get(),
                                      options_.member_index_growth);
      rep_ = std::make_unique<FullState>(
          FullState{std::move(storage), std::move(members)});
    } else {
      rep_ = SetListpack{};
    }
  }

  void validate_member(std::string_view member) const {
    if (!string_value_fits(member, options_.string_encoding)) {
      throw std::length_error("set member cannot fit its encoded form");
    }
  }

  int add_full(std::string_view member) {
    EncodedMemberProbe probe(member, options_.string_encoding);
    const bool inserted = full().members.find_or_emplace(
        probe.view(), [](ZSetMemberMeta&) {},
        [&]() {
          const auto id = full().storage->push_back_encoded(probe.encoded());
          return ZSetMemberMeta{.member_id = id};
        });
    return inserted ? 1 : 0;
  }

  bool erase_full(std::string_view member) {
    EncodedMemberProbe probe(member, options_.string_encoding);
    const auto slot = full().members.find_slot(probe.view());
    if (!slot) {
      return false;
    }
    const auto member_id = full().members.member_id_at(*slot);
    const auto last_id =
        static_cast<std::uint32_t>(full().storage->size() - 1);
    full().storage->orphan(member_id);
    const bool erased = full().members.erase_at_index(*slot);
    assert(erased);
    (void)erased;
    if (member_id != last_id) {
      full().storage->copy_ref(member_id, last_id);
      const bool moved = full().members.move_member_id(last_id, member_id);
      assert(moved);
      (void)moved;
    }
    full().storage->pop_back();
    (void)full().members.cleanup_after_removal_if_needed(1);
    maybe_compact_arena();
    return true;
  }

  void ensure_full() {
    if (!is_small()) {
      return;
    }
    ensure_memory_growth(sizeof(FullState) + sizeof(SetStorage));
    auto storage = std::make_unique<SetStorage>(
        options_.chunk_bytes, kDefaultArenaGrowth, options_.string_encoding);
    MemberIndex<SetStorage> members(storage.get(), options_.member_index_growth);
    const auto n = small_ptr()->size();
    storage->reserve(n);
    members.reserve_for_density(n, kDefaultMemberIndexDensity);
    small_ptr()->for_each(
        [&](EncodedStringView member) {
          const auto logical = member.to_string();
          const auto id = storage->push_back(logical);
          members.insert_packed(storage->view(id),
                                ZSetMemberMeta{.member_id = id});
        },
        options_.string_encoding);
    rep_ = std::make_unique<FullState>(
        FullState{std::move(storage), std::move(members)});
  }

  void maybe_demote_to_small() {
    if (is_small() || options_.listpack_max_entries == 0) {
      return;
    }
    const auto n = full().storage->size();
    if (n > options_.listpack_max_entries) {
      return;
    }
    if (!SetListpack::can_encode(n, full().storage->live_bytes())) {
      return;
    }
    try {
      std::vector<std::string> owned;
      std::vector<std::string_view> views;
      owned.reserve(n);
      views.reserve(n);
      for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(n); ++id) {
        owned.push_back(full().storage->encoded_view(id).to_string());
        views.push_back(owned.back());
      }
      SetListpack lp;
      const auto result = lp.add_many(views, options_.listpack_max_entries,
                                      options_.string_encoding);
      if (result.needs_full) {
        return;
      }
      rep_ = std::move(lp);
    } catch (const MaxMemoryExceeded&) {
      // Demotion is an optional representation change after a successful erase.
    }
  }

  void maybe_compact_arena() {
    if (is_small()) {
      return;
    }
    if (full().storage->dead_bytes() >= kAutoCompactDeadFloor &&
        full().storage->dead_bytes() >= full().storage->live_bytes()) {
      try {
        full().storage->compact();
        full().storage->shrink_to_fit();
      } catch (const MaxMemoryExceeded&) {
        // Keep the valid un-compacted representation until memory is freed.
      }
    }
  }

  void rebuild_member_index(
      double density = kDefaultMemberIndexDensity) {
    assert(!is_small());
    const auto n = static_cast<std::uint32_t>(full().storage->size());
    MemberIndex<SetStorage> rebuilt(full().storage.get(),
                                    options_.member_index_growth);
    rebuilt.reserve_for_density(n, density);
    for (std::uint32_t id = 0; id < n; ++id) {
      rebuilt.insert_packed(full().storage->view(id),
                            ZSetMemberMeta{.member_id = id});
    }
    full().members = std::move(rebuilt);
    full().members.set_members(full().storage.get());
  }

  [[nodiscard]] static std::uint64_t& rng_state() noexcept {
    thread_local std::uint64_t state = [] {
      const auto addr = reinterpret_cast<std::uintptr_t>(&rng_state);
      return std::uint64_t{0x9E3779B97F4A7C15ULL} ^
             (static_cast<std::uint64_t>(addr) << 1) ^
             (static_cast<std::uint64_t>(addr) >> 3);
    }();
    return state;
  }

  [[nodiscard]] static std::uint64_t random_u64() noexcept {
    auto& s = rng_state();
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
  }

  [[nodiscard]] static std::uint64_t random_bounded(
      std::uint64_t bound) noexcept {
    assert(bound > 0);
    if (bound == 1) {
      return 0;
    }
    const auto threshold =
        (std::numeric_limits<std::uint64_t>::max() / bound) * bound;
    for (;;) {
      const auto x = random_u64();
      if (x < threshold) {
        return x % bound;
      }
    }
  }

  [[nodiscard]] std::uint32_t random_id() const noexcept {
    assert(!empty());
    return static_cast<std::uint32_t>(random_bounded(size()));
  }

  SetOptions options_;
  std::variant<SetListpack, std::unique_ptr<FullState>> rep_;
};

}  // namespace goblin::core
