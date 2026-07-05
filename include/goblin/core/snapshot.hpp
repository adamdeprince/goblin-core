#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

// Goblin Core snapshot format.
//
// A snapshot is a two-layer format:
//
//   * A portable CANONICAL layer: for each zset, its members as
//     (score, length, bytes) records in member-id order. This alone fully
//     determines the set and is rebuilt into working indexes on load.
//   * An optional ACCELERATOR layer: a physical dump of the member index
//     (swiss table) plus the member-id order sorted by score. When the accel
//     version matches the running binary it lets load skip re-hashing and
//     re-sorting; otherwise it is ignored and the indexes are rebuilt from the
//     canonical layer.
//
// IMPORTANT (format evolution): the CANONICAL layer is the contract. If a future
// version changes it, bump kFormatVersion and keep a reader for every older
// canonical version -- old files must always remain loadable via the (possibly
// slow) canonical path. The ACCELERATOR layer carries no such promise: bump
// kAcceleratorVersion whenever the member-index dump layout or the swiss-table
// bucketing/fingerprint/layout changes, and old accelerators are simply
// discarded in favor of a canonical rebuild. Never write an old-accelerator
// reader; the canonical layer is the fallback.

namespace goblin::core::snapshot {

class snapshot_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr char kMagic[4] = {'G', 'C', 'S', 'N'};

// Bump when the canonical layer layout changes (and keep an old reader).
inline constexpr std::uint32_t kFormatVersion = 1;

// Bump when the accelerator/member-index dump layout or the swiss-table
// bucketing, fingerprint, or control/slot layout changes. Old accelerators are
// then ignored and indexes are rebuilt from canonical.
inline constexpr std::uint32_t kAcceleratorVersion = 1;

inline constexpr std::uint32_t kFlagAccelerator = 1u << 0;

// FNV-1a 64-bit. Corruption detection only; not cryptographic.
inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

[[nodiscard]] inline std::uint64_t checksum(std::string_view data,
                                            std::uint64_t seed = kFnvOffsetBasis) noexcept {
  std::uint64_t hash = seed;
  for (const char c : data) {
    hash ^= static_cast<unsigned char>(c);
    hash *= kFnvPrime;
  }
  return hash;
}

// Little-endian, length-prefixed writer over a growable byte buffer.
class Writer {
 public:
  explicit Writer(std::string& buffer) : buffer_(buffer) {}

  void u8(std::uint8_t value) { buffer_.push_back(static_cast<char>(value)); }
  void u16(std::uint16_t value) { put(value); }
  void u32(std::uint32_t value) { put(value); }
  void u64(std::uint64_t value) { put(value); }
  void f64(double value) { put(std::bit_cast<std::uint64_t>(value)); }

  void bytes(const char* data, std::size_t size) { buffer_.append(data, size); }
  void bytes(std::string_view data) { buffer_.append(data); }

  // Length-prefixed byte string.
  void str(std::string_view data) {
    u32(static_cast<std::uint32_t>(data.size()));
    buffer_.append(data);
  }

 private:
  template <class T>
  void put(T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      buffer_.push_back(static_cast<char>(value & 0xFF));
      value = static_cast<T>(value >> 8);
    }
  }

  std::string& buffer_;
};

// Little-endian reader over a byte span, bounds-checked.
class Reader {
 public:
  explicit Reader(std::string_view data)
      : cursor_(data.data()), end_(data.data() + data.size()) {}

  std::uint8_t u8() {
    need(1);
    return static_cast<std::uint8_t>(*cursor_++);
  }
  std::uint16_t u16() { return get<std::uint16_t>(); }
  std::uint32_t u32() { return get<std::uint32_t>(); }
  std::uint64_t u64() { return get<std::uint64_t>(); }
  double f64() { return std::bit_cast<double>(get<std::uint64_t>()); }

  std::string_view bytes(std::size_t size) {
    need(size);
    std::string_view result(cursor_, size);
    cursor_ += size;
    return result;
  }

  std::string_view str() { return bytes(u32()); }

  [[nodiscard]] bool at_end() const noexcept { return cursor_ == end_; }

 private:
  void need(std::size_t size) const {
    if (static_cast<std::size_t>(end_ - cursor_) < size) {
      throw snapshot_error("snapshot truncated");
    }
  }

  template <class T>
  T get() {
    need(sizeof(T));
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      value = static_cast<T>(
          value | (static_cast<T>(static_cast<unsigned char>(cursor_[i])) << (8 * i)));
    }
    cursor_ += sizeof(T);
    return value;
  }

  const char* cursor_;
  const char* end_;
};

}  // namespace goblin::core::snapshot
