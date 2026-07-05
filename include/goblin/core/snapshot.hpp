#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

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

// The snapshot body is a sequence of typed sections so each Redis value type
// gets its own section, and a reader can skip a section type it does not
// recognize (every section's entries are uniformly length-framed). Only ZSET is
// emitted today; the rest are reserved for when those types are implemented.
enum class SectionType : std::uint32_t {
  Zset = 1,
  String = 2,
  Hash = 3,
  List = 4,
  Set = 5,
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

// Bump when the ZSET accelerator (member-index dump) layout or the swiss-table
// bucketing, fingerprint, or control/slot layout changes. Old accelerators are
// then ignored and indexes are rebuilt from the canonical layer.
inline constexpr std::uint32_t kZsetAcceleratorVersion = 1;

// CRC32C (Castagnoli), for corruption detection. The result is standard
// CRC32C, so it is identical across the hardware and software paths and across
// architectures (a snapshot written on one machine verifies on another).
//
// No runtime CPU dispatch: the hardware instruction is used when the target ISA
// provides it -- SSE4.2 on x86-64, the CRC extension on AArch64, and LoongArch
// -- otherwise a table. On x86 that means building with `-msse4.2` (or
// `-march=native`); an x86 old enough to lack SSE4.2 falls to the table, but
// that is hardware Redis was built for, not this.
[[nodiscard]] inline std::uint32_t checksum(std::string_view data) noexcept {
  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t n = data.size();
  std::uint32_t crc = 0xFFFFFFFFu;

#if defined(__SSE4_2__)
  std::uint64_t acc = crc;
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t chunk;
    std::memcpy(&chunk, p, 8);
    acc = _mm_crc32_u64(acc, chunk);
  }
  crc = static_cast<std::uint32_t>(acc);
  for (; n > 0; --n) crc = _mm_crc32_u8(crc, *p++);
#elif defined(__ARM_FEATURE_CRC32)
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t chunk;
    std::memcpy(&chunk, p, 8);
    crc = __crc32cd(crc, chunk);
  }
  for (; n > 0; --n) crc = __crc32cb(crc, *p++);
#elif defined(__loongarch__)
  // crcc.w.* computes CRC32C directly. Untested here (no LoongArch hardware).
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t chunk;
    std::memcpy(&chunk, p, 8);
    crc = static_cast<std::uint32_t>(__builtin_loongarch_crcc_w_d_w(
        static_cast<long long>(chunk), static_cast<int>(crc)));
  }
  for (; n > 0; --n)
    crc = static_cast<std::uint32_t>(
        __builtin_loongarch_crcc_w_b_w(static_cast<char>(*p++), static_cast<int>(crc)));
#else
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    constexpr std::uint32_t poly = 0x82F63B78u;  // reflected Castagnoli
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ poly : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  for (; n > 0; --n) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
#endif

  return crc ^ 0xFFFFFFFFu;
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
