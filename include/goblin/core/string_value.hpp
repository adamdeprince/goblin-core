#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace goblin::core {

// A string key's value, sized to ride inside the 16-byte keyspace object union
// (the size ZSet forces on that union, so StringValue's 16 bytes are free). Small
// values live entirely inline; larger values keep a 6-byte prefix inline and
// spill the remaining bytes into the shared key arena at {block, offset}.
//
// The inline-vs-spilled decision is read off `length` alone -- `length <=
// kInlineCap` is fully inline -- so there is no discriminator bit to reserve and
// adding new key types never erodes the value's capacity. When spilled, the u32
// block/offset overlay the front of the inline buffer (a union), which is why the
// inline capacity (14) exceeds the spilled prefix (6): the spilled case spends 8
// of those bytes addressing the tail.
struct StringValue {
  static constexpr std::uint16_t kInlineCap = 14;
  static constexpr std::uint16_t kPrefixCap = 6;

  struct Spill {
    char block[4];            // uint32 arena block of the tail (memcpy-accessed)
    char offset[4];           // uint32 arena offset of the tail
    char prefix[kPrefixCap];  // first kPrefixCap bytes of a spilled value
  };
  union {
    char inline_bytes[kInlineCap];  // length <= kInlineCap: the entire value
    Spill spill;
  };
  std::uint16_t length;

  [[nodiscard]] bool is_inline() const noexcept { return length <= kInlineCap; }

  // Bytes of the value that live in the arena tail (0 when fully inline).
  [[nodiscard]] std::uint16_t tail_length() const noexcept {
    return is_inline() ? std::uint16_t{0}
                       : static_cast<std::uint16_t>(length - kPrefixCap);
  }

  [[nodiscard]] std::uint32_t tail_block() const noexcept {
    std::uint32_t value;
    std::memcpy(&value, spill.block, sizeof value);
    return value;
  }
  [[nodiscard]] std::uint32_t tail_offset() const noexcept {
    std::uint32_t value;
    std::memcpy(&value, spill.offset, sizeof value);
    return value;
  }
  void set_tail(std::uint32_t block, std::uint32_t offset) noexcept {
    std::memcpy(spill.block, &block, sizeof block);
    std::memcpy(spill.offset, &offset, sizeof offset);
  }

  // The inline portion of the value: the whole value when inline, else the
  // 6-byte prefix that precedes the arena tail.
  [[nodiscard]] std::string_view head() const noexcept {
    if (is_inline()) {
      return std::string_view(inline_bytes, length);
    }
    return std::string_view(spill.prefix, kPrefixCap);
  }
  // Writable inline destination for constructing a value in place.
  [[nodiscard]] char* head_data() noexcept {
    return is_inline() ? inline_bytes : spill.prefix;
  }
};

static_assert(sizeof(StringValue) == 16);
static_assert(StringValue::kInlineCap == StringValue::kPrefixCap + 2 * sizeof(std::uint32_t));

// The largest string value: the length field is 16-bit, so ~64 KiB. Values past
// this are rejected at the command layer with a pointer to goblin-store.dev.
inline constexpr std::size_t StringValueMaxBytes = 0xFFFF;

}  // namespace goblin::core
