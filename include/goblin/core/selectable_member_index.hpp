#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "goblin/core/linear_hash_index.hpp"
#include "goblin/core/zset_member_index.hpp"

namespace goblin::core {

enum class MemberIndexImplementation : std::uint8_t {
  Efficient,
  Realtime,
};

// Runtime selection without imposing a virtual call on every probe. Hashes use
// one selector per promoted object; the keyspace uses one for the whole store.
template <class Storage, class Meta = ZSetMemberMeta>
class SelectableMemberIndex {
 public:
  using EfficientIndex = MemberIndex<Storage, Meta>;
  using RealtimeIndex = LinearHashIndex<Storage, Meta>;
  using RealtimeArena = typename RealtimeIndex::Arena;
  using size_type = std::size_t;
  using id_type = typename Meta::id_type;

  SelectableMemberIndex() : index_(EfficientIndex{}) {}
  explicit SelectableMemberIndex(
      const Storage* members, double growth = EfficientIndex::kDefaultGrowth,
      MemberIndexImplementation implementation =
          MemberIndexImplementation::Efficient,
      std::shared_ptr<RealtimeArena> realtime_arena = {})
      : index_(implementation == MemberIndexImplementation::Realtime
                   ? Variant(std::make_unique<RealtimeIndex>(
                         members, std::move(realtime_arena)))
                   : Variant(EfficientIndex(members, growth))) {}

  [[nodiscard]] MemberIndexImplementation implementation() const noexcept {
    return std::holds_alternative<std::unique_ptr<RealtimeIndex>>(index_)
               ? MemberIndexImplementation::Realtime
               : MemberIndexImplementation::Efficient;
  }
  [[nodiscard]] bool supports_accelerator() const noexcept {
    return implementation() == MemberIndexImplementation::Efficient;
  }

  void set_members(const Storage* members) noexcept {
    visit([&](auto& index) { index.set_members(members); });
  }
  [[nodiscard]] bool empty() const noexcept {
    return visit([](const auto& index) { return index.empty(); });
  }
  [[nodiscard]] size_type size() const noexcept {
    return visit([](const auto& index) { return index.size(); });
  }
  [[nodiscard]] size_type capacity() const noexcept {
    return visit([](const auto& index) { return index.capacity(); });
  }
  [[nodiscard]] size_type tombstone_count() const noexcept {
    return visit([](const auto& index) { return index.tombstone_count(); });
  }
  [[nodiscard]] size_type allocated_bytes() const noexcept {
    const auto bytes =
        visit([](const auto& index) { return index.allocated_bytes(); });
    return bytes + (implementation() == MemberIndexImplementation::Realtime
                        ? sizeof(RealtimeIndex)
                        : 0);
  }
  [[nodiscard]] size_type realtime_arena_allocated_bytes() const noexcept {
    if (const auto* realtime =
            std::get_if<std::unique_ptr<RealtimeIndex>>(&index_)) {
      return (*realtime)->arena()->allocated_bytes();
    }
    return 0;
  }
  [[nodiscard]] size_type realtime_arena_slack_bytes() const noexcept {
    if (const auto* realtime =
            std::get_if<std::unique_ptr<RealtimeIndex>>(&index_)) {
      const auto& arena = *(*realtime)->arena();
      return arena.allocated_bytes() -
             arena.live_cells() * arena.cell_bytes() -
             arena.live_directory_bytes();
    }
    return 0;
  }
  [[nodiscard]] size_type member_slot_capacity() const noexcept {
    return visit([](const auto& index) { return index.member_slot_capacity(); });
  }

  bool cleanup_after_removal_if_needed(size_type removed_count) {
    return visit([&](auto& index) {
      return index.cleanup_after_removal_if_needed(removed_count);
    });
  }
  bool rehash_same_capacity() {
    return visit([](auto& index) { return index.rehash_same_capacity(); });
  }
  void reserve(size_type expected_size) {
    visit([&](auto& index) { index.reserve(expected_size); });
  }
  void reserve_additional(size_type additional) {
    visit([&](auto& index) { index.reserve_additional(additional); });
  }
  void reserve_for_density(size_type expected_size, double density) {
    visit([&](auto& index) {
      index.reserve_for_density(expected_size, density);
    });
  }
  void insert_packed(std::string_view member, Meta meta) {
    visit([&](auto& index) { index.insert_packed(member, meta); });
  }

  // Direct RT index pointer for monomorphic Hash FullState hot paths. Null when
  // this selector holds the efficient Swiss index.
  [[nodiscard]] RealtimeIndex* realtime() noexcept {
    if (auto* realtime =
            std::get_if<std::unique_ptr<RealtimeIndex>>(&index_)) {
      return realtime->get();
    }
    return nullptr;
  }
  [[nodiscard]] const RealtimeIndex* realtime() const noexcept {
    if (const auto* realtime =
            std::get_if<std::unique_ptr<RealtimeIndex>>(&index_)) {
      return realtime->get();
    }
    return nullptr;
  }
  [[nodiscard]] EfficientIndex* efficient() noexcept {
    return std::get_if<EfficientIndex>(&index_);
  }
  [[nodiscard]] const EfficientIndex* efficient() const noexcept {
    return std::get_if<EfficientIndex>(&index_);
  }

  // Hot path: branch on the active alternative without a full std::visit
  // instantiation. RT is unique_ptr so the branch predicts well once a hash is
  // promoted; efficient is the inline MemberIndex.
  [[nodiscard]] Meta* find(std::string_view member) {
    if (auto* rt = realtime()) {
      return rt->find(member);
    }
    return std::get<EfficientIndex>(index_).find(member);
  }
  [[nodiscard]] const Meta* find(std::string_view member) const {
    if (const auto* rt = realtime()) {
      return rt->find(member);
    }
    return std::get<EfficientIndex>(index_).find(member);
  }
  [[nodiscard]] std::optional<size_type> find_slot(
      std::string_view member) const {
    if (const auto* rt = realtime()) {
      return rt->find_slot(member);
    }
    return std::get<EfficientIndex>(index_).find_slot(member);
  }
  [[nodiscard]] id_type member_id_at(size_type slot) const noexcept {
    return visit([&](const auto& index) { return index.member_id_at(slot); });
  }
  bool erase_at_index(size_type slot) {
    return visit([&](auto& index) { return index.erase_at_index(slot); });
  }
  [[nodiscard]] bool move_member_id(id_type old_member_id,
                                    id_type new_member_id) {
    return visit([&](auto& index) {
      return index.move_member_id(old_member_id, new_member_id);
    });
  }
  [[nodiscard]] bool move_member_id_at_slot(size_type slot,
                                            id_type old_member_id,
                                            id_type new_member_id) {
    return visit([&](auto& index) {
      return index.move_member_id_at_slot(slot, old_member_id, new_member_id);
    });
  }
  std::pair<Meta*, bool> insert(std::string_view member, Meta meta) {
    return visit([&](auto& index) { return index.insert(member, meta); });
  }
  template <class OnExisting, class MakeMeta>
  bool find_or_emplace(std::string_view member, OnExisting&& on_existing,
                       MakeMeta&& make_meta, bool maintain = true) {
    if (auto* rt = realtime()) {
      return rt->find_or_emplace(member, std::forward<OnExisting>(on_existing),
                                 std::forward<MakeMeta>(make_meta), maintain);
    }
    // Efficient Swiss index has no incremental maintenance.
    (void)maintain;
    return std::get<EfficientIndex>(index_).find_or_emplace(
        member, std::forward<OnExisting>(on_existing),
        std::forward<MakeMeta>(make_meta));
  }
  void insert_absent(std::string_view member, Meta meta,
                     bool maintain = true) {
    if (auto* rt = realtime()) {
      rt->insert_absent(member, meta, maintain);
      return;
    }
    (void)maintain;
    std::get<EfficientIndex>(index_).insert_absent(member, meta);
  }
  void run_maintenance_steps(size_type steps) noexcept {
    if (auto* rt = realtime()) {
      rt->run_maintenance_steps(steps);
    }
  }
  bool erase(std::string_view member) {
    if (auto* rt = realtime()) {
      return rt->erase(member);
    }
    return std::get<EfficientIndex>(index_).erase(member);
  }
  template <class Fn>
  void for_each(Fn&& fn) const {
    visit([&](const auto& index) { index.for_each(fn); });
  }

  void write_accelerator(snapshot::Writer& writer) const {
    std::get<EfficientIndex>(index_).write_accelerator(writer);
  }
  void read_accelerator(snapshot::Reader& reader, const Storage* members) {
    if (auto* efficient = std::get_if<EfficientIndex>(&index_)) {
      efficient->read_accelerator(reader, members);
      return;
    }
    EfficientIndex discard(members);
    discard.read_accelerator(reader, members);
  }
  void clear() { visit([](auto& index) { index.clear(); }); }

 private:
  using Variant =
      std::variant<EfficientIndex, std::unique_ptr<RealtimeIndex>>;

  template <class Fn>
  decltype(auto) visit(Fn&& fn) {
    return std::visit(
        [&](auto& index) -> decltype(auto) {
          if constexpr (std::is_same_v<std::remove_cvref_t<decltype(index)>,
                                       std::unique_ptr<RealtimeIndex>>) {
            return std::forward<Fn>(fn)(*index);
          } else {
            return std::forward<Fn>(fn)(index);
          }
        },
        index_);
  }
  template <class Fn>
  decltype(auto) visit(Fn&& fn) const {
    return std::visit(
        [&](const auto& index) -> decltype(auto) {
          if constexpr (std::is_same_v<std::remove_cvref_t<decltype(index)>,
                                       std::unique_ptr<RealtimeIndex>>) {
            return std::forward<Fn>(fn)(*index);
          } else {
            return std::forward<Fn>(fn)(index);
          }
        },
        index_);
  }

  Variant index_;
};

}  // namespace goblin::core
