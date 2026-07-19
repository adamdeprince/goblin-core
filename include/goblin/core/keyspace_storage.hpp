#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "goblin/core/page_arena.hpp"
#include "goblin/core/string_encoding.hpp"

namespace goblin::core {

// The keyspace member index's per-slot payload: a key id packed to 48 bits (6
// bytes, alignment 1, memcpy-accessed) so the slot array holds no padding while
// supporting > 2^32 keys. 48 bits is 281 trillion ids -- far past any keystore
// -- and matches the 48-bit member the TTL set references keys by. get()/set()
// are the interface MemberIndex uses (see ZSetMemberMeta).
struct KeyMeta {
  using id_type = std::uint64_t;
  char packed[6];

  [[nodiscard]] id_type get() const noexcept {
    std::uint32_t low;
    std::uint16_t high;
    std::memcpy(&low, packed, sizeof low);
    std::memcpy(&high, packed + sizeof low, sizeof high);
    return static_cast<id_type>(low) | (static_cast<id_type>(high) << 32);
  }
  void set(id_type value) noexcept {
    const auto low = static_cast<std::uint32_t>(value);
    const auto high = static_cast<std::uint16_t>(value >> 32);
    std::memcpy(packed, &low, sizeof low);
    std::memcpy(packed + sizeof low, &high, sizeof high);
  }
};
static_assert(sizeof(KeyMeta) == 6);
static_assert(alignof(KeyMeta) == 1);

// The largest key id: the packed 48-bit KeyMeta bounds the id space at 2^48 - 1.
inline constexpr std::uint64_t kMaxKeyId = (std::uint64_t{1} << 48) - 1;

// The unified keyspace's byte arena plus its per-key location table. Every key of
// every type is packed here as a blob addressed by a 32/32 {block, offset}; the
// tail of a string value (the bytes past StringValue's inline prefix) shares the
// same arena. Total keys+values is therefore bounded only by the split address
// (chunk_bytes << 32), not the 4 GiB a single u32 offset would impose.
//
// A MemberIndex over this storage resolves a key id to its bytes via view(id).
// Value tails are addressed directly by the {block, offset} carried in their
// StringValue and are not part of the id-indexed key table -- this storage just
// hands out arena space for them and tracks their bytes for compaction.
class KeyspaceStorage {
 public:
  using size_type = std::size_t;

  static constexpr size_type kDefaultChunkBytes = std::size_t{1} << 20;  // 1 MiB
  // A blob (key or value tail) is at most 64 KiB, so any chunk >= 128 KiB holds
  // one without ever straddling a block boundary.
  static constexpr size_type kMinChunkBytes = std::size_t{1} << 17;
  // Keys and value tails are addressed by a 16-bit length (<= 64 KiB - 1).
  static constexpr size_type kMaxBlobBytes =
      std::numeric_limits<std::uint16_t>::max();

  struct TailLocation {
    std::uint32_t block{0};
    std::uint32_t offset{0};
  };

  struct BlobAllocation {
    TailLocation location{};
    char* data{nullptr};
  };

  KeyspaceStorage() { configure_chunk_bytes(kDefaultChunkBytes); }
  explicit KeyspaceStorage(size_type chunk_bytes,
                           double growth = kDefaultArenaGrowth)
      : growth_(growth > 1.0 ? growth : kDefaultArenaGrowth) {
    configure_chunk_bytes(chunk_bytes);
  }

  [[nodiscard]] size_type size() const noexcept { return key_len_.size(); }
  [[nodiscard]] bool empty() const noexcept { return key_len_.empty(); }
  [[nodiscard]] size_type chunk_bytes() const noexcept { return chunk_bytes_; }

  void reserve(size_type key_count) {
    key_block_.reserve(key_count);
    key_offset_.reserve(key_count);
    key_len_.reserve(key_count);
  }

  // --- key table (id-indexed; MemberIndex resolves keys through view) ---

  [[nodiscard]] std::string_view view(std::uint64_t id) const noexcept {
    assert(id < key_len_.size());
    const auto len = key_len_[id];
    if (len == 0) {
      return {};
    }
    return std::string_view(chunk_ptr(key_block_[id], key_offset_[id]), len);
  }

  [[nodiscard]] std::uint16_t key_length(std::uint64_t id) const noexcept {
    assert(id < key_len_.size());
    return key_len_[id];
  }

  [[nodiscard]] std::uint64_t push_back_key(std::string_view key) {
    if (key.size() > kMaxBlobBytes) {
      throw std::length_error("keyspace key too large");
    }
    if (key_len_.size() > kMaxKeyId) {
      throw std::length_error("keyspace id space exhausted");
    }
    const auto id = static_cast<std::uint64_t>(key_len_.size());
    const auto loc = append_blob(key);
    key_block_.push_back(loc.block);
    key_offset_.push_back(loc.offset);
    key_len_.push_back(static_cast<std::uint16_t>(key.size()));
    return id;
  }

  // Replace one live key's bytes without moving its object slot. The caller
  // updates the key index around this operation; keeping the id stable also
  // keeps any TTL attached to the object.
  void replace_key(std::uint64_t id, std::string_view key) {
    assert(id < key_len_.size());
    if (key.size() > kMaxBlobBytes) {
      throw std::length_error("keyspace key too large");
    }
    const auto loc = append_blob(key);
    mark_key_dead(id);
    key_block_[id] = loc.block;
    key_offset_[id] = loc.offset;
    key_len_[id] = static_cast<std::uint16_t>(key.size());
  }

  // Swap-remove support: slide src's key location down into dst (the hole left by
  // an erased id), then pop_back_key() drops the now-duplicated last slot.
  void move_key_slot(std::uint64_t dst, std::uint64_t src) noexcept {
    assert(dst < key_len_.size() && src < key_len_.size());
    key_block_[dst] = key_block_[src];
    key_offset_[dst] = key_offset_[src];
    key_len_[dst] = key_len_[src];
  }

  void pop_back_key() noexcept {
    assert(!key_len_.empty());
    key_block_.pop_back();
    key_offset_.pop_back();
    key_len_.pop_back();
  }

  // Account a key's bytes as dead. Call before the id is removed/overwritten.
  void mark_key_dead(std::uint64_t id) noexcept {
    assert(id < key_len_.size());
    dead_bytes_ += key_len_[id];
  }

  // --- value tails (addressed directly by StringValue, not id-indexed) ---

  [[nodiscard]] TailLocation append_tail(std::string_view tail) {
    if (tail.size() > kMaxBlobBytes) {
      throw std::length_error("keyspace value tail too large");
    }
    return append_blob(tail);
  }

  // Movable object blobs (currently compact hashes) share the same exact-byte
  // arena as keys and spilled string bodies. The owning object keeps only this
  // location and its logical byte count; keyspace compaction rewrites both.
  [[nodiscard]] BlobAllocation reserve_blob(size_type length) {
    if (length > kMaxBlobBytes) {
      throw std::length_error("keyspace object blob too large");
    }
    const auto r = reserve_run_bytes_split(
        blocks_, next_offset_, active_bytes_, committed_bytes_, chunk_bytes_,
        chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, length);
    used_bytes_ += length;
    return {{r.block, r.offset}, r.dst};
  }

  [[nodiscard]] TailLocation append_object_blob(std::string_view bytes) {
    const auto allocation = reserve_blob(bytes.size());
    if (!bytes.empty()) {
      std::memcpy(allocation.data, bytes.data(), bytes.size());
    }
    return allocation.location;
  }

  [[nodiscard]] const char* blob_data(TailLocation loc) const noexcept {
    return chunk_ptr(loc.block, loc.offset);
  }

  [[nodiscard]] char* mutable_blob_data(TailLocation loc) noexcept {
    return chunk_ptr(loc.block, loc.offset);
  }

  // Arena growth replaces the tail block after copying its live prefix. A
  // caller rebuilding an object inside that block can pin the old allocation
  // until it has finished copying from pointers obtained before the growth.
  [[nodiscard]] std::shared_ptr<char[]> pin_blob(TailLocation loc) const {
    assert(loc.block < blocks_.size());
    return blocks_[loc.block];
  }

  [[nodiscard]] std::string_view object_blob_view(
      TailLocation loc, std::uint16_t len) const noexcept {
    return tail_view(loc, len);
  }

  void mark_object_blob_dead(std::uint16_t len) noexcept {
    dead_bytes_ += len;
  }

  [[nodiscard]] TailLocation append_encoded_tail(
      const EncodedString& encoded, size_type skip) {
    assert(skip <= encoded.size());
    const auto length = encoded.size() - skip;
    const auto r = reserve_run_bytes_split(
        blocks_, next_offset_, active_bytes_, committed_bytes_, chunk_bytes_,
        chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, length);
    if (r.dst != nullptr) {
      encoded.write_range_to(skip, length, r.dst);
      used_bytes_ += length;
    }
    return {r.block, r.offset};
  }

  [[nodiscard]] std::string_view tail_view(TailLocation loc,
                                           std::uint16_t len) const noexcept {
    if (len == 0) {
      return {};
    }
    return std::string_view(chunk_ptr(loc.block, loc.offset), len);
  }

  void mark_tail_dead(std::uint16_t len) noexcept { dead_bytes_ += len; }

  // --- accounting ---

  [[nodiscard]] size_type used_bytes() const noexcept { return used_bytes_; }
  [[nodiscard]] size_type dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] size_type live_bytes() const noexcept {
    return used_bytes_ - dead_bytes_;
  }
  [[nodiscard]] size_type committed_bytes() const noexcept {
    return committed_bytes_;
  }

  [[nodiscard]] size_type allocated_bytes() const noexcept {
    return committed_bytes_ +
           key_block_.capacity() * sizeof(std::uint32_t) +
           key_offset_.capacity() * sizeof(std::uint32_t) +
           key_len_.capacity() * sizeof(std::uint16_t);
  }

  // Reclamation is a rebuild: the Keyspace replays live keys and tails into a
  // fresh storage, updating each StringValue's tail address, then swaps. This is
  // the trigger it polls after a delete/overwrite orphans bytes.
  [[nodiscard]] bool should_compact() const noexcept {
    return dead_bytes_ >= kCompactFloorBytes && dead_bytes_ * 2 >= used_bytes_;
  }

 private:
  static constexpr size_type kCompactFloorBytes = std::size_t{1} << 16;

  void configure_chunk_bytes(size_type chunk_bytes) {
    if (!std::has_single_bit(chunk_bytes) || chunk_bytes < kMinChunkBytes) {
      chunk_bytes = kDefaultChunkBytes;
    }
    chunk_bytes_ = chunk_bytes;
    chunk_shift_ = static_cast<size_type>(std::countr_zero(chunk_bytes));
    chunk_mask_ = chunk_bytes - 1;
  }

  [[nodiscard]] char* chunk_ptr(std::uint32_t block,
                                std::uint32_t offset) const noexcept {
    return blocks_[block].get() + offset;
  }

  [[nodiscard]] TailLocation append_blob(std::string_view bytes) {
    const auto r = reserve_run_bytes_split(
        blocks_, next_offset_, active_bytes_, committed_bytes_, chunk_bytes_,
        chunk_shift_, chunk_mask_, growth_, kInitialArenaBytes, bytes.size());
    if (r.dst != nullptr) {
      std::memcpy(r.dst, bytes.data(), bytes.size());
      used_bytes_ += bytes.size();
    }
    return {r.block, r.offset};
  }

  std::vector<std::shared_ptr<char[]>> blocks_;
  size_type next_offset_{0};
  size_type active_bytes_{0};
  size_type committed_bytes_{0};
  size_type used_bytes_{0};
  size_type dead_bytes_{0};
  size_type chunk_bytes_{kDefaultChunkBytes};
  size_type chunk_shift_{20};
  size_type chunk_mask_{kDefaultChunkBytes - 1};
  double growth_{kDefaultArenaGrowth};

  std::vector<std::uint32_t> key_block_;
  std::vector<std::uint32_t> key_offset_;
  std::vector<std::uint16_t> key_len_;
};

}  // namespace goblin::core
