#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "goblin/core/swiss_table.hpp"

namespace goblin::core {

// A packed byte store for the store's keyspace keys. Keys are write-once, so
// std::string's capacity + grow/shrink + per-key malloc (and the fragmentation
// of repeated key insert/delete) is pure waste. Instead every key's bytes live
// here, appended back-to-back and length-prefixed (LEB128, 1 byte for keys
// <= 127), and the keyspace swiss table stores a bare uint64 offset into this
// arena -- so its slots are fixed-size {offset, value} records with no embedded
// string. A deleted key's bytes are marked dead; the store bulk-compacts (rebuild
// arena + swiss) once dead bytes dominate. Offsets are uint64 because a large
// busy server can accumulate more than 4 GiB of keys.
class KeyArena {
 public:
  static constexpr std::uint64_t kInvalid = ~std::uint64_t{0};

  // Append a key; returns its stable offset (until the next compaction).
  [[nodiscard]] std::uint64_t append(std::string_view key) {
    const std::uint64_t offset = data_.size();
    std::uint64_t len = key.size();
    while (len >= 0x80) {
      data_.push_back(static_cast<char>((len & 0x7F) | 0x80));
      len >>= 7;
    }
    data_.push_back(static_cast<char>(len));
    data_.append(key.data(), key.size());
    ++live_count_;
    return offset;
  }

  // Resolve an offset back to the key bytes.
  [[nodiscard]] std::string_view bytes(std::uint64_t offset) const noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(data_.data()) + offset;
    std::uint64_t len = 0;
    unsigned shift = 0;
    while (*p & 0x80) {
      len |= static_cast<std::uint64_t>(*p++ & 0x7F) << shift;
      shift += 7;
    }
    len |= static_cast<std::uint64_t>(*p++) << shift;
    return std::string_view(reinterpret_cast<const char*>(p), len);
  }

  // Mark a removed key's bytes dead (its slot in the arena is now garbage).
  void mark_dead(std::uint64_t offset) noexcept {
    const auto key = bytes(offset);
    dead_bytes_ += (key.data() - (data_.data() + offset)) + key.size();
    --live_count_;
  }

  [[nodiscard]] std::size_t live_count() const noexcept { return live_count_; }
  [[nodiscard]] std::uint64_t dead_bytes() const noexcept { return dead_bytes_; }
  [[nodiscard]] std::uint64_t live_bytes() const noexcept {
    return data_.size() - dead_bytes_;
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return data_.capacity();
  }

  // Rebuild once the dead bytes have caught up with the live bytes past a floor,
  // bounding the arena at ~2x live -- same shape as the member-arena policy.
  [[nodiscard]] bool should_compact() const noexcept {
    return dead_bytes_ >= kCompactFloor && dead_bytes_ >= live_bytes();
  }

  void clear() noexcept {
    data_.clear();
    data_.shrink_to_fit();
    dead_bytes_ = 0;
    live_count_ = 0;
  }

 private:
  static constexpr std::uint64_t kCompactFloor = std::uint64_t{1} << 16;  // 64 KiB
  std::string data_;
  std::uint64_t dead_bytes_ = 0;
  std::size_t live_count_ = 0;
};

// Hash + equality for a keyspace swiss table keyed by KeyArena offsets. Both a
// stored uint64 offset and a lookup string_view resolve to the same key bytes,
// so hashing/comparison agree across insert (offset), rehash (offset), and
// find/erase (string_view heterogeneous lookup). The arena must outlive the table.
struct KeyArenaHash {
  const KeyArena* arena = nullptr;
  [[nodiscard]] std::size_t operator()(std::uint64_t offset) const noexcept {
    return StringTableHash{}(arena->bytes(offset));
  }
  [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
    return StringTableHash{}(key);
  }
};

struct KeyArenaEqual {
  const KeyArena* arena = nullptr;
  [[nodiscard]] bool operator()(std::uint64_t a, std::uint64_t b) const noexcept {
    return a == b || arena->bytes(a) == arena->bytes(b);
  }
  [[nodiscard]] bool operator()(std::uint64_t a, std::string_view b) const noexcept {
    return arena->bytes(a) == b;
  }
  [[nodiscard]] bool operator()(std::string_view a, std::uint64_t b) const noexcept {
    return a == arena->bytes(b);
  }
  [[nodiscard]] bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

}  // namespace goblin::core
