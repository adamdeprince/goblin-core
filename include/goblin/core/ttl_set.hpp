#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>

#include "goblin/core/chunked_sorted_list.hpp"
#include "goblin/core/swiss_table.hpp"

namespace goblin::core {

// 48-bit little-endian load/store (a 32-bit + a 16-bit access, memcpy so there is
// no alignment requirement). Every TTL value -- key id and ms-since-epoch expiry
// -- is stored this way so the structures below waste no padding.
[[nodiscard]] inline std::uint64_t load_u48(const char* p) noexcept {
  std::uint32_t low;
  std::uint16_t high;
  std::memcpy(&low, p, sizeof low);
  std::memcpy(&high, p + sizeof low, sizeof high);
  return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
}
inline void store_u48(char* p, std::uint64_t value) noexcept {
  const auto low = static_cast<std::uint32_t>(value);
  const auto high = static_cast<std::uint16_t>(value >> 32);
  std::memcpy(p, &low, sizeof low);
  std::memcpy(p + sizeof low, &high, sizeof high);
}

// A 48-bit value packed to 6 bytes (alignment 1). Used as both the key and the
// value of the by-key-id index, so a slot is pair<PackedU48, PackedU48> = 12 B.
struct PackedU48 {
  char bytes[6];
  [[nodiscard]] std::uint64_t get() const noexcept { return load_u48(bytes); }
  [[nodiscard]] static PackedU48 make(std::uint64_t value) noexcept {
    PackedU48 packed;
    store_u48(packed.bytes, value);
    return packed;
  }
  [[nodiscard]] bool operator==(const PackedU48& other) const noexcept {
    return std::memcmp(bytes, other.bytes, sizeof bytes) == 0;
  }
};
static_assert(sizeof(PackedU48) == 6);
static_assert(alignof(PackedU48) == 1);

struct PackedU48Hash {
  [[nodiscard]] std::size_t operator()(const PackedU48& key) const noexcept {
    return std::hash<std::uint64_t>{}(key.get());
  }
};
struct PackedU48Eq {
  [[nodiscard]] bool operator()(const PackedU48& a,
                                const PackedU48& b) const noexcept {
    return a == b;
  }
};

// The by-expiry ordered entry: {expiry, key id} packed to 12 bytes. Ordered by
// (expiry, key id) so the front is the soonest-to-expire and the pair is unique
// (a key id appears once), which lets erase locate an exact entry.
struct TtlEntry {
  char expiry[6];
  char key_id[6];
  [[nodiscard]] std::uint64_t expiry_ms() const noexcept { return load_u48(expiry); }
  [[nodiscard]] std::uint64_t key() const noexcept { return load_u48(key_id); }
  [[nodiscard]] static TtlEntry make(std::uint64_t expiry_ms,
                                     std::uint64_t key) noexcept {
    TtlEntry entry;
    store_u48(entry.expiry, expiry_ms);
    store_u48(entry.key_id, key);
    return entry;
  }
};
static_assert(sizeof(TtlEntry) == 12);
static_assert(alignof(TtlEntry) == 1);

struct TtlEntryLess {
  [[nodiscard]] bool operator()(const TtlEntry& a,
                                const TtlEntry& b) const noexcept {
    const auto ae = a.expiry_ms();
    const auto be = b.expiry_ms();
    if (ae != be) {
      return ae < be;
    }
    return a.key() < b.key();
  }
};

// The keyspace-wide expiry set: sparse (only keys with a TTL), keyed by the
// 48-bit keyspace id, scored by a 48-bit ms-since-epoch expiry. It recycles the
// zset's sorted-list mechanism for the by-expiry order (front = who is next) and
// adds a packed swiss key_id -> expiry for O(1) lookup / removal / rekey. There
// is exactly one of these per store, so it carries no listpack tier.
class TtlSet {
 public:
  [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }

  // Set or replace a key's expiry.
  void set(std::uint64_t key_id, std::uint64_t expiry_ms) {
    if (auto* existing = index_.find(PackedU48::make(key_id))) {
      (void)order_.erase_one(TtlEntry::make(existing->get(), key_id));
      *existing = PackedU48::make(expiry_ms);
    } else {
      index_.insert_or_assign(PackedU48::make(key_id), PackedU48::make(expiry_ms));
    }
    order_.insert(TtlEntry::make(expiry_ms, key_id));
  }

  // Remove a key's TTL; true if it had one.
  bool clear(std::uint64_t key_id) {
    const auto* existing = index_.find(PackedU48::make(key_id));
    if (existing == nullptr) {
      return false;
    }
    (void)order_.erase_one(TtlEntry::make(existing->get(), key_id));
    (void)index_.erase(PackedU48::make(key_id));
    return true;
  }

  [[nodiscard]] std::optional<std::uint64_t> expiry(std::uint64_t key_id) const {
    const auto* existing = index_.find(PackedU48::make(key_id));
    if (existing == nullptr) {
      return std::nullopt;
    }
    return existing->get();
  }
  [[nodiscard]] bool contains(std::uint64_t key_id) const {
    return index_.find(PackedU48::make(key_id)) != nullptr;
  }

  // The keyspace slid the key at id `from` down into `to` (swap-remove on erase);
  // move its TTL entry to match. A no-op if the moved key had no TTL.
  void rekey(std::uint64_t from, std::uint64_t to) {
    const auto* existing = index_.find(PackedU48::make(from));
    if (existing == nullptr) {
      return;
    }
    const auto expiry_ms = existing->get();
    (void)order_.erase_one(TtlEntry::make(expiry_ms, from));
    (void)index_.erase(PackedU48::make(from));
    order_.insert(TtlEntry::make(expiry_ms, to));
    index_.insert_or_assign(PackedU48::make(to), PackedU48::make(expiry_ms));
  }

  // Expire every key due at or before now_ms, calling fn(key_id) for each. fn may
  // in turn rekey OTHER entries (deleting a key swap-removes another into its
  // slot), which is safe: each victim is removed from both structures before fn
  // runs, and the loop re-reads the front each time rather than holding a cursor.
  template <class Fn>
  std::size_t expire_due(std::uint64_t now_ms, std::size_t budget, Fn&& fn) {
    std::size_t expired = 0;
    while (expired < budget && !order_.empty()) {
      const TtlEntry front = order_.at(0);
      if (front.expiry_ms() > now_ms) {
        break;
      }
      const auto key_id = front.key();
      (void)order_.erase_one(front);
      (void)index_.erase(PackedU48::make(key_id));
      fn(key_id);
      ++expired;
    }
    return expired;
  }

  // Visit every (key_id, expiry_ms) pair (unordered). Used to persist TTLs.
  template <class Fn>
  void for_each(Fn&& fn) const {
    index_.for_each([&fn](const auto& entry) {
      fn(entry.first.get(), entry.second.get());
    });
  }

  void clear_all() noexcept { *this = TtlSet{}; }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return index_.allocated_bytes() + order_.size() * sizeof(TtlEntry);
  }

 private:
  ChunkedSortedList<TtlEntry, TtlEntryLess> order_;
  SwissTable<PackedU48, PackedU48, PackedU48Hash, PackedU48Eq> index_;
};

}  // namespace goblin::core
