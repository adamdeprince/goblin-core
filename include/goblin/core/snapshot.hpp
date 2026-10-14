#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#include "goblin/core/simd.hpp"

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

// Bump when the canonical layer layout changes. v2 records each zset's score
// width and writes scores at it (i16/i32/f64) instead of always f64. v3 records
// each hash's selected efficient/RT representation. The loader retains the v2
// hash reader and applies the configured default to those older snapshots. v4
// adds the replication lineage, logical offset, and inclusive Kafka cursor.
inline constexpr std::uint32_t kFormatVersion = 4;
inline constexpr std::uint32_t kOldestReadableFormatVersion = 2;

// The snapshot body is a sequence of typed sections so each Redis value type
// gets its own section, and a reader can skip a section type it does not
// recognize (every section's entries are uniformly length-framed). Zsets,
// zsets, packed zsets, strings, hashes, lists, sets, arrays, TTLs, and replication
// metadata currently have emitted sections.
enum class SectionType : std::uint32_t {
  Zset = 1,
  String = 2,
  Hash = 3,
  List = 4,
  Set = 5,
  Ttl = 6,
  Array = 7,
  Replication = 8,
  PackedZset = 9,
};

// Each section body is a stream of instructions -- a tiny per-family bytecode --
// terminated by the End opcode. A non-End instruction is: opcode (u8), operand
// length (u64), operand checksum (u32, CRC32C), then the operands. A reader
// executes the (section_type, opcode) pairs it knows and skips the rest by
// operand length, so new opcodes and even whole new section types stay loadable
// by older readers.
inline constexpr std::uint8_t kOpEnd = 0x00;

enum class ZsetOpcode : std::uint8_t {
  End = kOpEnd,
  Zset = 0x01,  // operands: key, options, canonical members, optional accelerator
};

enum class PackedZsetOpcode : std::uint8_t {
  End = kOpEnd,
  PackedZset = 0x01,  // operands: key, kind, canonical score/member pairs
};

// Bump when the ZSET accelerator (member-index dump) layout or the swiss-table
// bucketing, fingerprint, or control/slot layout changes. Old accelerators are
// then ignored and indexes are rebuilt from the canonical layer.
inline constexpr std::uint32_t kZsetAcceleratorVersion = 1;

enum class HashOpcode : std::uint8_t {
  End = kOpEnd,
  Hash = 0x01,  // operands: key, options, canonical field/value pairs, optional accelerator
};

// Bump when the HASH accelerator (field-index dump) layout or the swiss-table
// bucketing/fingerprint/control/slot layout changes. Old accelerators are then
// ignored and the field index is rebuilt from the canonical layer.
inline constexpr std::uint32_t kHashAcceleratorVersion = 1;

enum class StringOpcode : std::uint8_t {
  End = kOpEnd,
  String = 0x01,  // operands: key, raw value bytes (no accelerator)
};

enum class ListOpcode : std::uint8_t {
  End = kOpEnd,
  List = 0x01,  // operands: key, canonical values, optional raw-copy marker
};

// Bump when the raw-list accelerator framing changes. Canonical ordered values
// remain the fallback.
inline constexpr std::uint32_t kListAcceleratorVersion = 1;

enum class SetOpcode : std::uint8_t {
  End = kOpEnd,
  Set = 0x01,  // operands: key, options, canonical members, optional accelerator
};

// Bump when the SET accelerator (member-index dump) layout, the swiss-table
// bucketing/fingerprint/control/slot layout, or the encoding-identity word
// that gates the dump changes. Old accelerators are then ignored and the
// member index is rebuilt from the canonical layer.
inline constexpr std::uint32_t kSetAcceleratorVersion = 1;

enum class ArrayOpcode : std::uint8_t {
  End = kOpEnd,
  // operands: key, implementation, geometry, insert cursor, (index, value)*
  Array = 0x01,
};

// No accelerator yet: leaf tables rebuild from canonical (index, value) pairs.
inline constexpr std::uint32_t kArrayAcceleratorVersion = 0;

enum class TtlOpcode : std::uint8_t {
  End = kOpEnd,
  Ttl = 0x01,  // operands: key, absolute expiry ms (u64)
};

enum class ReplicationOpcode : std::uint8_t {
  End = kOpEnd,
  State = 0x01,  // operands: valid, replication id, offset, Kafka cursor
};

// CRC32C (Castagnoli), for corruption detection. The result is standard
// CRC32C, so it is identical across the hardware and software paths and across
// architectures (a snapshot written on one machine verifies on another).
//
// Runtime selection uses hardware CRC32C when supported, or the software table.
[[nodiscard]] inline std::uint32_t checksum(std::string_view data) noexcept {
  return simd::checksum(data);
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
