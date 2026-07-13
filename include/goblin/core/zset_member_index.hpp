#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "goblin/core/simd_ops.hpp"
#include "goblin/core/snapshot.hpp"
#include "goblin/core/zset_member_storage.hpp"

namespace goblin::core {

// The default per-slot payload: a 32-bit member id (zset members and hash fields
// never exceed 2^32). `id_type`/`get()`/`set()` are the uniform interface the
// index uses so a wider payload (the keyspace's packed 48-bit KeyMeta) can be
// dropped in without touching the swiss logic; `.member_id` stays so every
// existing zset/hash call site is unchanged.
struct ZSetMemberMeta {
  using id_type = std::uint32_t;
  std::uint32_t member_id{0};
  [[nodiscard]] id_type get() const noexcept { return member_id; }
  void set(id_type value) noexcept { member_id = value; }
};
static_assert(sizeof(ZSetMemberMeta) == 4);

// The tuned Swiss table (SIMD group probe, non-power-of-two capacity, growth
// factor, density-targeted packing). It is storage-agnostic: it stores only
// member ids in its slots and resolves a slot's key bytes on demand through
// `Storage::view(id_type) -> std::string_view`. `ZSetMemberIndex` (below) is the
// zset instantiation over ZSetMemberStorage; the hash type reuses it over
// HashStorage for its field->id index, and the keyspace instantiates it over
// KeyspaceStorage with a packed 48-bit KeyMeta.
template <class Storage, class Meta = ZSetMemberMeta>
class MemberIndex {
 public:
  using size_type = std::size_t;
  using hash_type = std::uint32_t;
  using id_type = typename Meta::id_type;

  // Control bytes are probed a full 16-byte SIMD group at a time. Wider groups
  // (AVX2/AVX-512) measured slower: at a ~90% load factor a lookup resolves in
  // the first group, so a wider scan only adds false-positive member compares
  // and cache-line-split loads. 16 also matches an SSE2/NEON register.
  static constexpr size_type kGroupWidth = 16;

  // 2^0.25. A tight growth factor keeps the always-on (never-compacted) load
  // factor >= 1/growth ~= 84% at any size -- so the member index never balloons
  // to ~2x just past a power of two -- at the cost of more frequent rehashes
  // during load (memory-first; OPTIMIZE repacks read-mostly sets regardless).
  static constexpr double kDefaultGrowth = 1.1892071150027210;

  // A dumped control/slot table only means anything to code whose hash puts
  // members in the same buckets with the same fingerprints. std::hash differs
  // across standard libraries (libc++ vs libstdc++), so a snapshot carries this
  // probe; a loader whose hash_identity() differs must rebuild from canonical
  // rather than trust the dump.
  [[nodiscard]] static std::uint64_t hash_identity() noexcept {
    return static_cast<std::uint64_t>(
        std::hash<std::string_view>{}("goblin-core member index hash probe"));
  }

  MemberIndex() = default;

  explicit MemberIndex(const Storage* members,
                       double growth = kDefaultGrowth)
      : members_(members), growth_(growth > 1.0 ? growth : kDefaultGrowth) {}

  void set_members(const Storage* members) noexcept {
    members_ = members;
  }

  // Copy the swiss table and rebind to a forked member storage (shallow COW).
  [[nodiscard]] MemberIndex clone_rebound(const Storage* members) const {
    MemberIndex copy(members, growth_);
    copy.control_ = control_;
    copy.slots_ = slots_;
    copy.size_ = size_;
    copy.tombstones_ = tombstones_;
    copy.capacity_ = capacity_;
    return copy;
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

  [[nodiscard]] size_type size() const noexcept {
    return size_;
  }

  [[nodiscard]] size_type capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] size_type tombstone_count() const noexcept {
    return tombstones_;
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return control_.capacity() * sizeof(std::uint8_t) +
           slots_.capacity() * sizeof(Slot);
  }

  [[nodiscard]] size_type member_slot_capacity() const noexcept {
    return 0;
  }

  bool cleanup_after_removal_if_needed(size_type removed_count) {
    if (removed_count == 0 || tombstones_ == 0) {
      return false;
    }

    if (size_ == 0) {
      clear();
      return true;
    }

    const auto shrunk_capacity = capacity_for_size(size_);
    if (capacity_ > kGroupWidth && size_ < capacity_ / 4 &&
        shrunk_capacity < capacity_) {
      rehash(shrunk_capacity);
      return true;
    }

    if (tombstones_ >= kMinTombstonesForSameCapacityCleanup &&
        tombstones_ >= size_) {
      rehash(capacity_);
      return true;
    }

    return false;
  }

  bool rehash_same_capacity() {
    if (capacity_ == 0 || tombstones_ == 0) {
      return false;
    }

    rehash(capacity_);
    return true;
  }

  void reserve(size_type expected_size) {
    if (expected_size == 0 || expected_size <= max_usable(capacity_)) {
      return;
    }
    rehash(capacity_for_size(expected_size));
  }

  // Reserve room for a batch while preserving the configured geometric growth
  // ratio. reserve(size() + batch) intentionally packs tightly and therefore
  // rehashes again on the next batch; this path grows only when the batch no
  // longer fits and clears tombstones in the same rebuild.
  void reserve_additional(size_type additional) {
    if (additional == 0) {
      return;
    }
    if (additional > std::numeric_limits<size_type>::max() - size_) {
      throw std::length_error("member index size overflow");
    }

    const auto expected_size = size_ + additional;
    const auto usable = max_usable(capacity_);
    const auto occupied = size_ + tombstones_;
    const bool occupied_batch_fits =
        occupied <= usable && additional <= usable - occupied;
    if (expected_size <= usable && occupied_batch_fits) {
      return;
    }

    auto target_capacity = capacity_ == 0 ? kGroupWidth : capacity_;
    while (expected_size > max_usable(target_capacity)) {
      target_capacity = grow_capacity(target_capacity);
    }
    rehash(target_capacity);
  }

  // Allocate for exactly `expected_size` members at a target load factor
  // (`density` in (0, 1]). density 1.0 packs the table with no spare slots,
  // which minimizes memory for a set that will not receive further inserts;
  // combine with insert_packed(), which does not grow. Larger-than-max_usable
  // densities are honored here (unlike reserve()).
  void reserve_for_density(size_type expected_size, double density) {
    if (expected_size == 0) {
      return;
    }
    if (!(density > 0.0) || density > 1.0) {
      density = static_cast<double>(31) / 32;
    }
    const auto needed =
        static_cast<size_type>(static_cast<double>(expected_size) / density);
    rehash(round_up_to_group(std::max(needed, expected_size)));
  }

  // Insert a known-unique member without a duplicate check or a capacity grow.
  // The caller must have reserved enough capacity (see reserve_for_density).
  void insert_packed(std::string_view member, Meta meta) {
    const auto hash = hash_member(member);
    const auto index = find_insert_index(hash);
    slots_[index].meta = meta;
    set_control(index, fingerprint(hash));
    ++size_;
  }

  [[nodiscard]] Meta* find(std::string_view member) {
    const auto index = find_index(member);
    if (index == npos) {
      return nullptr;
    }
    return &slots_[index].meta;
  }

  [[nodiscard]] const Meta* find(std::string_view member) const {
    const auto index = find_index(member);
    if (index == npos) {
      return nullptr;
    }
    return &slots_[index].meta;
  }

  [[nodiscard]] std::optional<size_type> find_slot(std::string_view member) const {
    const auto index = find_index(member);
    if (index == npos) {
      return std::nullopt;
    }
    return index;
  }

  [[nodiscard]] id_type member_id_at(size_type index) const noexcept {
    return slots_[index].meta.get();
  }

  bool erase_at_index(size_type index) {
    if (index >= capacity_ || !is_full(index)) {
      return false;
    }

    set_control(index, kDeleted);
    --size_;
    ++tombstones_;
    return true;
  }

  [[nodiscard]] bool move_member_id(id_type old_member_id,
                                    id_type new_member_id) {
    const auto index = find_index(member_view(old_member_id));
    if (index == npos) {
      return false;
    }
    return move_member_id_at_slot(index, old_member_id, new_member_id);
  }

  [[nodiscard]] bool move_member_id_at_slot(size_type index,
                                            id_type old_member_id,
                                            id_type new_member_id) {
    if (index >= capacity_ || !is_full(index)) {
      return false;
    }
    if (slots_[index].meta.get() != old_member_id) {
      return false;
    }

    slots_[index].meta.set(new_member_id);
    return true;
  }

  std::pair<Meta*, bool> insert(std::string_view member, Meta meta) {
    ensure_capacity_for_insert();

    const auto hash = hash_member(member);
    const auto existing = find_index_with_hash(member, hash);
    if (existing != npos) {
      return {&slots_[existing].meta, false};
    }

    const auto index = find_insert_index(hash);
    const auto old_control = control_[index];
    slots_[index].meta = meta;
    set_control(index, fingerprint(hash));
    ++size_;
    if (old_control == kDeleted) {
      --tombstones_;
    }
    return {&slots_[index].meta, true};
  }

  // Single-hash upsert. A probe returns either the matching slot or the first
  // reusable tombstone/empty slot in the same table walk. `on_existing(meta)`
  // runs for a hit; `make_meta()` runs only on insert (so the caller can push
  // storage only when needed). A miss is probed again only when making room
  // rebuilds the table. Returns true if a new slot was inserted.
  template <class OnExisting, class MakeMeta>
  bool find_or_emplace(std::string_view member, OnExisting&& on_existing,
                       MakeMeta&& make_meta) {
    const auto hash = hash_member(member);
    ProbeResult probe{npos, false};
    if (capacity_ != 0) {
      probe = find_or_insert_index_with_hash(member, hash);
      if (probe.found) {
        on_existing(slots_[probe.index].meta);
        return false;
      }
    }

    if (ensure_capacity_for_insert()) {
      // Rehash preserves absence, but the saved insertion slot is stale.
      probe = find_or_insert_index_with_hash(member, hash);
    }
    if (probe.index == npos) {
      throw std::logic_error("member index has no insertion slot");
    }

    const auto index = probe.index;
    const auto old_control = control_[index];
    slots_[index].meta = make_meta();
    set_control(index, fingerprint(hash));
    ++size_;
    if (old_control == kDeleted) {
      --tombstones_;
    }
    return true;
  }

  // Insert a member the caller has already proven absent (e.g. HSETNX after a
  // negative find). One hash + one insert-slot probe; no existence walk.
  void insert_absent(std::string_view member, Meta meta) {
    ensure_capacity_for_insert();
    const auto hash = hash_member(member);
    const auto index = find_insert_index(hash);
    const auto old_control = control_[index];
    slots_[index].meta = meta;
    set_control(index, fingerprint(hash));
    ++size_;
    if (old_control == kDeleted) {
      --tombstones_;
    }
  }

  bool erase(std::string_view member) {
    const auto index = find_index(member);
    if (index == npos) {
      return false;
    }

    set_control(index, kDeleted);
    --size_;
    ++tombstones_;
    return true;
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        fn(member_view(slots_[i].meta.get()), slots_[i].meta);
      }
    }
  }

  // Dump the raw table state (control bytes + per-slot member ids) so a restore
  // can skip re-hashing. Tied to the current bucketing/fingerprint/layout; see
  // snapshot::kAcceleratorVersion. The member bytes themselves live in the
  // canonical layer, not here.
  void write_accelerator(snapshot::Writer& writer) const {
    writer.u64(capacity_);
    writer.u64(size_);
    writer.u64(tombstones_);
    writer.bytes(reinterpret_cast<const char*>(control_.data()), control_.size());
    for (size_type i = 0; i < capacity_; ++i) {
      writer.u32(static_cast<std::uint32_t>(slots_[i].meta.get()));
    }
  }

  // Empty the table in place (Hash freelist recycle, cleanup_after_removal).
  void clear() {
    control_.clear();
    slots_.clear();
    size_ = 0;
    tombstones_ = 0;
    capacity_ = 0;
  }

  // Restore a table dumped by write_accelerator(). growth_ is left as
  // constructed (it comes from options, not the dump). The caller must only
  // invoke this when the accelerator version matches the running binary.
  void read_accelerator(snapshot::Reader& reader, const Storage* members) {
    members_ = members;
    capacity_ = static_cast<size_type>(reader.u64());
    size_ = static_cast<size_type>(reader.u64());
    tombstones_ = static_cast<size_type>(reader.u64());
    const auto control = reader.bytes(capacity_ + kGroupWidth);
    const auto* control_begin = reinterpret_cast<const std::uint8_t*>(control.data());
    control_.assign(control_begin, control_begin + control.size());
    slots_.resize(capacity_);
    for (size_type i = 0; i < capacity_; ++i) {
      slots_[i].meta.set(reader.u32());
    }
  }

 private:
  struct Slot {
    Meta meta;
  };

  static constexpr std::uint8_t kEmpty = 0x80;
  static constexpr std::uint8_t kDeleted = 0xFE;
  static constexpr size_type kMinTombstonesForSameCapacityCleanup = 4096;
  static constexpr size_type npos = std::numeric_limits<size_type>::max();

  struct ProbeResult {
    size_type index;
    bool found;
  };

  [[nodiscard]] static bool is_full_control(std::uint8_t control) noexcept {
    return control < kEmpty;
  }

  [[nodiscard]] bool is_full(size_type index) const noexcept {
    return is_full_control(control_[index]);
  }

  // Low 7 bits. The bucket() reduction below consumes the high bits, so keeping
  // the fingerprint in the low bits keeps the two independent.
  [[nodiscard]] static std::uint8_t fingerprint(hash_type hash) noexcept {
    return static_cast<std::uint8_t>(hash & 0x7FU);
  }

  // Map a hash to a slot in [0, capacity_) without requiring a power-of-two
  // capacity: Lemire's multiply-shift reduction (one multiply) in place of a
  // bit mask. This lets the table size to ~31/32 load at any member count
  // instead of rounding capacity up to the next power of two.
  [[nodiscard]] size_type bucket(hash_type hash) const noexcept {
    return static_cast<size_type>(
        (static_cast<std::uint64_t>(hash) * capacity_) >> 32);
  }

  [[nodiscard]] static size_type first_set_bit(std::uint64_t mask) noexcept {
    return static_cast<size_type>(std::countr_zero(mask));
  }

  // Return a bitmask with bit i set for each of the kGroupWidth control bytes
  // equal to `needle`. This is the Swiss-table group probe; `pmovmskb` is the
  // canonical implementation and is not auto-generated from portable C++, so a
  // byte-compare intrinsic is used with a scalar fallback. Each variant is
  // baseline for its ISA (no runtime CPU dispatch): SSE2 on x86-64, NEON on
  // AArch64, and a scalar loop everywhere else.
  [[nodiscard]] static std::uint64_t match_byte(const std::uint8_t* group,
                                                std::uint8_t needle) noexcept {
    static_assert(kGroupWidth == 16, "group probe expects a 16-byte group");
    return simd::match_control_group_16(group, needle);
  }

  [[nodiscard]] static size_type max_usable(size_type capacity) noexcept {
    return capacity - (capacity / 32);
  }

  [[nodiscard]] static size_type round_up_to_group(size_type value) noexcept {
    if (value < kGroupWidth) {
      return kGroupWidth;
    }
    return (value + kGroupWidth - 1) / kGroupWidth * kGroupWidth;
  }

  // Grow capacity by the configured factor (rounded to a whole group), always
  // by at least one group. A smaller factor keeps the load factor higher (less
  // memory) between rehashes at the cost of more frequent rehashes.
  [[nodiscard]] size_type grow_capacity(size_type capacity) const noexcept {
    const auto scaled =
        static_cast<size_type>(static_cast<double>(capacity) * growth_);
    const auto next = round_up_to_group(scaled);
    return next > capacity ? next : capacity + kGroupWidth;
  }

  [[nodiscard]] static size_type capacity_for_size(size_type expected_size) {
    if (expected_size == 0) {
      return 0;
    }

    // ~31/32 target load. Round to a whole number of groups rather than the
    // next power of two, so a reserve()/compact() holds the load high at any
    // size instead of dropping to ~50% just past a power of two.
    return round_up_to_group((expected_size * 32 + 30) / 31);
  }

  [[nodiscard]] std::string_view member_view(id_type member_id) const noexcept {
    return members_->view(member_id);
  }

  [[nodiscard]] hash_type hash_member(std::string_view member) const {
    auto hash = static_cast<size_type>(hasher_(member));
    if constexpr (sizeof(size_type) > sizeof(hash_type)) {
      hash ^= hash >> std::numeric_limits<hash_type>::digits;
    }
    return static_cast<hash_type>(hash);
  }

  bool ensure_capacity_for_insert() {
    if (capacity_ == 0) {
      rehash(kGroupWidth);
      return true;
    }

    if (size_ + tombstones_ + 1 > max_usable(capacity_)) {
      if (size_ + 1 <= max_usable(capacity_)) {
        rehash(capacity_);
      } else {
        rehash(grow_capacity(capacity_));
      }
      return true;
    }
    return false;
  }

  void allocate_capacity(size_type requested_capacity) {
    capacity_ = round_up_to_group(requested_capacity);
    if (capacity_ > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("zset member index capacity exhausted");
    }
    slots_.clear();
    slots_.resize(capacity_);
    control_.assign(capacity_ + kGroupWidth, kEmpty);
    size_ = 0;
    tombstones_ = 0;
  }

  void set_control(size_type index, std::uint8_t value) noexcept {
    control_[index] = value;
    if (index < kGroupWidth) {
      control_[capacity_ + index] = value;
    }
  }

  [[nodiscard]] size_type find_index(std::string_view member) const {
    if (capacity_ == 0) {
      return npos;
    }
    return find_index_with_hash(member, hash_member(member));
  }

  [[nodiscard]] size_type find_index_with_hash(std::string_view member,
                                               hash_type hash) const {
    const auto needle = fingerprint(hash);
    auto group_start = bucket(hash);

    for (size_type probed = 0; probed < capacity_; probed += kGroupWidth) {
      const auto* group = control_.data() + group_start;
      auto matches = match_byte(group, needle);
      while (matches != 0) {
        const auto offset = first_set_bit(matches);
        auto index = group_start + offset;
        if (index >= capacity_) {
          index -= capacity_;
        }

        if (simd::bytes_equal(member_view(slots_[index].meta.get()), member)) {
          return index;
        }
        matches &= matches - 1;
      }

      if (match_byte(group, kEmpty) != 0) {
        return npos;
      }

      group_start += kGroupWidth;
      if (group_start >= capacity_) {
        group_start -= capacity_;
      }
    }

    return npos;
  }

  [[nodiscard]] ProbeResult find_or_insert_index_with_hash(
      std::string_view member, hash_type hash) const {
    const auto needle = fingerprint(hash);
    auto group_start = bucket(hash);
    auto first_deleted = npos;

    for (size_type probed = 0; probed < capacity_; probed += kGroupWidth) {
      const auto* group = control_.data() + group_start;
      auto matches = match_byte(group, needle);
      while (matches != 0) {
        const auto offset = first_set_bit(matches);
        auto index = group_start + offset;
        if (index >= capacity_) {
          index -= capacity_;
        }

        if (simd::bytes_equal(member_view(slots_[index].meta.get()), member)) {
          return {index, true};
        }
        matches &= matches - 1;
      }

      const auto deleted = match_byte(group, kDeleted);
      if (first_deleted == npos && deleted != 0) {
        first_deleted = group_start + first_set_bit(deleted);
        if (first_deleted >= capacity_) {
          first_deleted -= capacity_;
        }
      }

      const auto empty = match_byte(group, kEmpty);
      if (empty != 0) {
        auto index = group_start + first_set_bit(empty);
        if (index >= capacity_) {
          index -= capacity_;
        }
        return {first_deleted == npos ? index : first_deleted, false};
      }

      group_start += kGroupWidth;
      if (group_start >= capacity_) {
        group_start -= capacity_;
      }
    }

    return {first_deleted, false};
  }

  [[nodiscard]] size_type find_insert_index(hash_type hash) const {
    auto group_start = bucket(hash);
    auto first_deleted = npos;

    for (size_type probed = 0; probed < capacity_; probed += kGroupWidth) {
      const auto* group = control_.data() + group_start;

      auto deleted = match_byte(group, kDeleted);
      if (first_deleted == npos && deleted != 0) {
        first_deleted = group_start + first_set_bit(deleted);
        if (first_deleted >= capacity_) {
          first_deleted -= capacity_;
        }
      }

      auto empty = match_byte(group, kEmpty);
      if (empty != 0) {
        auto index = group_start + first_set_bit(empty);
        if (index >= capacity_) {
          index -= capacity_;
        }
        return first_deleted == npos ? index : first_deleted;
      }

      group_start += kGroupWidth;
      if (group_start >= capacity_) {
        group_start -= capacity_;
      }
    }

    return first_deleted;
  }

  void insert_existing(Slot slot) {
    const auto hash = hash_member(member_view(slot.meta.get()));
    const auto index = find_insert_index(hash);
    slots_[index] = slot;
    set_control(index, fingerprint(hash));
    ++size_;
  }

  void rehash(size_type requested_capacity) {
    MemberIndex replacement(members_, growth_);
    replacement.allocate_capacity(requested_capacity);

    for (size_type i = 0; i < capacity_; ++i) {
      if (is_full(i)) {
        replacement.insert_existing(slots_[i]);
      }
    }

    control_ = std::move(replacement.control_);
    slots_ = std::move(replacement.slots_);
    size_ = replacement.size_;
    tombstones_ = 0;
    capacity_ = replacement.capacity_;
  }

  const Storage* members_{nullptr};
  std::vector<std::uint8_t> control_;
  std::vector<Slot> slots_;
  size_type size_{0};
  size_type tombstones_{0};
  size_type capacity_{0};
  double growth_{kDefaultGrowth};
  [[no_unique_address]] std::hash<std::string_view> hasher_{};
};

// The zset instantiation: field->member_id over the member-bytes storage. Every
// existing use of ZSetMemberIndex is unchanged.
using ZSetMemberIndex = MemberIndex<ZSetMemberStorage>;

}  // namespace goblin::core
