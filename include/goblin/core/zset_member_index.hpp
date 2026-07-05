#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "goblin/core/snapshot.hpp"
#include "goblin/core/zset_member_storage.hpp"

namespace goblin::core {

struct ZSetMemberMeta {
  std::uint32_t member_id{0};
};
static_assert(sizeof(ZSetMemberMeta) == 4);

class ZSetMemberIndex {
 public:
  using size_type = std::size_t;
  using hash_type = std::uint32_t;

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

  ZSetMemberIndex() = default;

  explicit ZSetMemberIndex(const ZSetMemberStorage* members,
                           double growth = kDefaultGrowth)
      : members_(members), growth_(growth > 1.0 ? growth : kDefaultGrowth) {}

  void set_members(const ZSetMemberStorage* members) noexcept {
    members_ = members;
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
  void insert_packed(std::string_view member, ZSetMemberMeta meta) {
    const auto hash = hash_member(member);
    const auto index = find_insert_index(hash);
    slots_[index].meta = meta;
    set_control(index, fingerprint(hash));
    ++size_;
  }

  [[nodiscard]] ZSetMemberMeta* find(std::string_view member) {
    const auto index = find_index(member);
    if (index == npos) {
      return nullptr;
    }
    return &slots_[index].meta;
  }

  [[nodiscard]] const ZSetMemberMeta* find(std::string_view member) const {
    const auto index = find_index(member);
    if (index == npos) {
      return nullptr;
    }
    return &slots_[index].meta;
  }

  [[nodiscard]] bool move_member_id(std::uint32_t old_member_id,
                                    std::uint32_t new_member_id) {
    const auto index = find_index(member_view(old_member_id));
    if (index == npos) {
      return false;
    }
    if (slots_[index].meta.member_id != old_member_id) {
      return false;
    }

    slots_[index].meta.member_id = new_member_id;
    return true;
  }

  std::pair<ZSetMemberMeta*, bool> insert(std::string_view member, ZSetMemberMeta meta) {
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
        fn(member_view(slots_[i].meta.member_id), slots_[i].meta);
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
      writer.u32(slots_[i].meta.member_id);
    }
  }

  // Restore a table dumped by write_accelerator(). growth_ is left as
  // constructed (it comes from options, not the dump). The caller must only
  // invoke this when the accelerator version matches the running binary.
  void read_accelerator(snapshot::Reader& reader, const ZSetMemberStorage* members) {
    members_ = members;
    capacity_ = static_cast<size_type>(reader.u64());
    size_ = static_cast<size_type>(reader.u64());
    tombstones_ = static_cast<size_type>(reader.u64());
    const auto control = reader.bytes(capacity_ + kGroupWidth);
    const auto* control_begin = reinterpret_cast<const std::uint8_t*>(control.data());
    control_.assign(control_begin, control_begin + control.size());
    slots_.resize(capacity_);
    for (size_type i = 0; i < capacity_; ++i) {
      slots_[i].meta.member_id = reader.u32();
    }
  }

 private:
  struct Slot {
    ZSetMemberMeta meta;
  };

  static constexpr std::uint8_t kEmpty = 0x80;
  static constexpr std::uint8_t kDeleted = 0xFE;
  static constexpr size_type kMinTombstonesForSameCapacityCleanup = 4096;
  static constexpr size_type npos = std::numeric_limits<size_type>::max();

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
#if defined(__SSE2__)
    static_assert(kGroupWidth == 16, "SSE2 group probe expects a 16-byte group");
    const __m128i control =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(group));
    const __m128i equal =
        _mm_cmpeq_epi8(control, _mm_set1_epi8(static_cast<char>(needle)));
    return static_cast<std::uint64_t>(
        static_cast<unsigned>(_mm_movemask_epi8(equal)) & 0xFFFFU);
#elif defined(__aarch64__)
    static_assert(kGroupWidth == 16, "NEON group probe expects a 16-byte group");
    // AArch64 NEON has no pmovmskb; AND each 0x00/0xFF compare lane with its
    // slot bit and horizontally add per 8-byte half to build a 16-bit, one-bit-
    // per-slot mask matching the SSE2 result.
    static constexpr std::uint8_t kSlotBits[16] = {
        1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t control = vld1q_u8(group);
    const uint8x16_t equal = vceqq_u8(control, vdupq_n_u8(needle));
    const uint8x16_t masked = vandq_u8(equal, vld1q_u8(kSlotBits));
    const std::uint32_t low = vaddv_u8(vget_low_u8(masked));
    const std::uint32_t high = vaddv_u8(vget_high_u8(masked));
    return static_cast<std::uint64_t>(low | (high << 8));
#else
    std::uint64_t mask = 0;
    for (size_type i = 0; i < kGroupWidth; ++i) {
      mask |= static_cast<std::uint64_t>(group[i] == needle) << i;
    }
    return mask;
#endif
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

  [[nodiscard]] std::string_view member_view(std::uint32_t member_id) const noexcept {
    return members_->view(member_id);
  }

  [[nodiscard]] hash_type hash_member(std::string_view member) const {
    auto hash = static_cast<size_type>(hasher_(member));
    if constexpr (sizeof(size_type) > sizeof(hash_type)) {
      hash ^= hash >> std::numeric_limits<hash_type>::digits;
    }
    return static_cast<hash_type>(hash);
  }

  void clear() {
    control_.clear();
    slots_.clear();
    size_ = 0;
    tombstones_ = 0;
    capacity_ = 0;
  }

  void ensure_capacity_for_insert() {
    if (capacity_ == 0) {
      rehash(kGroupWidth);
      return;
    }

    if (size_ + tombstones_ + 1 > max_usable(capacity_)) {
      if (size_ + 1 <= max_usable(capacity_)) {
        rehash(capacity_);
      } else {
        rehash(grow_capacity(capacity_));
      }
    }
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

        if (member_view(slots_[index].meta.member_id) == member) {
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
    const auto hash = hash_member(member_view(slot.meta.member_id));
    const auto index = find_insert_index(hash);
    slots_[index] = slot;
    set_control(index, fingerprint(hash));
    ++size_;
  }

  void rehash(size_type requested_capacity) {
    ZSetMemberIndex replacement(members_, growth_);
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

  const ZSetMemberStorage* members_{nullptr};
  std::vector<std::uint8_t> control_;
  std::vector<Slot> slots_;
  size_type size_{0};
  size_type tombstones_{0};
  size_type capacity_{0};
  double growth_{kDefaultGrowth};
  [[no_unique_address]] std::hash<std::string_view> hasher_{};
};

}  // namespace goblin::core
