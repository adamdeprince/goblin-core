#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace goblin::core {

// Stored string-value encoding. Prefixes 0x92..0xfd remain reserved.
inline constexpr std::uint8_t kStringPositiveBase = 0x80;
inline constexpr std::uint8_t kStringNegativeBase = 0x88;
inline constexpr std::uint8_t kStringUuidHex = 0x90;
inline constexpr std::uint8_t kStringUuidDashed = 0x91;
inline constexpr std::uint8_t kStringLz4 = 0xfe;
inline constexpr std::uint8_t kStringRaw = 0xff;

// Every encoded value fits an ordinary u16 length. The prefix consumes one
// byte, leaving at most 65,534 bytes for either the raw string or the compressed
// payload. An LZ4 payload begins with a three-byte logical length, so
// compressible logical strings may be larger than the raw limit without
// widening arena refs.
inline constexpr std::size_t kStringMaxEncodedBytes = 65'535;
inline constexpr std::size_t kStringMaxBytes = 65'534;
inline constexpr std::size_t kStringMaxVerbatimBytes = 65'535;
inline constexpr std::size_t kStringMaxPayloadBytes = 65'534;
inline constexpr std::size_t kStringMaxCompressedLogicalBytes = 0xFF'FFFF;

// The low 25 bits hold threshold+1 (zero means --use-lz4 was absent). The next
// five bits hold the compression selector, and bit 30 disables the shared
// encoding entirely. Keeping every knob in one word avoids growing each owning
// Hash/List options object.
struct StringEncodingOptions {
  static constexpr std::uint32_t kThresholdMask = (std::uint32_t{1} << 25) - 1;
  static constexpr std::uint32_t kLevelShift = 25;
  static constexpr std::uint32_t kLevelMask = std::uint32_t{0x1f} << kLevelShift;
  static constexpr std::uint32_t kDisabledMask = std::uint32_t{1} << 30;

  std::uint32_t packed{0};

  [[nodiscard]] static constexpr StringEncodingOptions with_lz4(
      std::size_t min_bytes) noexcept {
    constexpr auto kLargestThreshold = kStringMaxCompressedLogicalBytes + 1;
    const auto capped =
        min_bytes > kLargestThreshold ? kLargestThreshold : min_bytes;
    return {static_cast<std::uint32_t>(capped + 1)};
  }

  constexpr void set_lz4_min_bytes(std::size_t min_bytes) noexcept {
    const auto flags = packed & (kLevelMask | kDisabledMask);
    *this = with_lz4(min_bytes);
    packed |= flags;
  }

  [[nodiscard]] static constexpr bool valid_lz4_compress_level(
      int level) noexcept {
    return level == 0 || (level >= 3 && level <= 12) ||
           (level <= -1 && level >= -8);
  }

  constexpr void set_lz4_compress_level(int level) noexcept {
    if (!valid_lz4_compress_level(level)) {
      level = 0;
    }
    std::uint32_t code = 0;
    if (level < 0) {
      code = static_cast<std::uint32_t>(-level);
    } else if (level > 0) {
      code = static_cast<std::uint32_t>(level + 6);
    }
    packed = (packed & ~kLevelMask) | (code << kLevelShift);
  }

  constexpr void disable() noexcept { packed |= kDisabledMask; }
  [[nodiscard]] constexpr bool encoding_enabled() const noexcept {
    return (packed & kDisabledMask) == 0;
  }

  [[nodiscard]] constexpr bool lz4_enabled() const noexcept {
    return encoding_enabled() && (packed & kThresholdMask) != 0;
  }
  [[nodiscard]] constexpr std::size_t lz4_min_bytes() const noexcept {
    return lz4_enabled()
               ? static_cast<std::size_t>((packed & kThresholdMask) - 1)
               : 0;
  }
  [[nodiscard]] constexpr int lz4_compress_level() const noexcept {
    const auto code = static_cast<int>((packed & kLevelMask) >> kLevelShift);
    if (code <= 8) {
      return -code;
    }
    return code - 6;
  }
  [[nodiscard]] constexpr std::size_t max_value_bytes() const noexcept {
    return encoding_enabled() ? kStringMaxBytes : kStringMaxVerbatimBytes;
  }
};
static_assert(sizeof(StringEncodingOptions) == 4);

enum class StringCompressionMode : std::uint8_t {
  AllowLz4,
  NeverLz4,
};

// Classifies one logical string and prepares its encoded bytes. Raw and
// specialized forms allocate nothing; only the selected LZ4 form owns a buffer.
class EncodedString {
 public:
  explicit EncodedString(
      std::string_view value, StringEncodingOptions options = {},
      StringCompressionMode compression = StringCompressionMode::AllowLz4);

  [[nodiscard]] std::size_t size() const noexcept { return encoded_size_; }
  [[nodiscard]] std::size_t logical_size() const noexcept {
    return source_.size();
  }
  [[nodiscard]] std::uint8_t prefix() const noexcept;
  [[nodiscard]] bool is_raw() const noexcept {
    return storage_ == Storage::Raw || storage_ == Storage::Verbatim;
  }
  [[nodiscard]] bool is_lz4() const noexcept {
    return storage_ == Storage::Lz4;
  }
  [[nodiscard]] bool encoding_enabled() const noexcept {
    return storage_ != Storage::Verbatim;
  }
  [[nodiscard]] bool equals_encoded(std::string_view stored) const noexcept;

  void write_to(char* destination) const noexcept;
  void write_range_to(std::size_t offset, std::size_t length,
                      char* destination) const noexcept;

 private:
  enum class Storage : std::uint8_t { Verbatim, Raw, Inline, Lz4 };

  std::string_view source_;
  std::array<char, 17> inline_{};
  std::string lz4_;
  std::uint16_t encoded_size_{0};
  Storage storage_{Storage::Raw};
};

// A non-owning view of stored encoded bytes. The two-piece form is used by the
// top-level StringValue inline-prefix/arena-tail layout; hash and list values
// normally use the one-piece constructor.
class EncodedStringView {
 public:
  EncodedStringView() = default;
  explicit EncodedStringView(std::string_view encoded,
                             bool encoding_enabled = true)
      : head_(encoded), encoding_enabled_(encoding_enabled) {}
  EncodedStringView(std::string_view head, std::string_view tail,
                    bool encoding_enabled = true)
      : head_(head), tail_(tail), encoding_enabled_(encoding_enabled) {}

  [[nodiscard]] std::size_t encoded_size() const noexcept {
    return head_.size() + tail_.size();
  }
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] std::uint8_t prefix() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool is_raw() const noexcept {
    return !encoding_enabled_ || prefix() == kStringRaw;
  }
  [[nodiscard]] bool is_lz4() const noexcept {
    return encoding_enabled_ && prefix() == kStringLz4;
  }
  [[nodiscard]] bool encoding_enabled() const noexcept {
    return encoding_enabled_;
  }
  [[nodiscard]] std::uint8_t encoded_byte(std::size_t index) const noexcept {
    return byte_at(index);
  }

  void append_to(std::string& destination) const;
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] bool equals(std::string_view logical) const;

  void copy_encoded_to(char* destination) const noexcept;
  void copy_encoded_range_to(std::size_t offset, std::size_t length,
                             char* destination) const noexcept {
    copy_range(offset, length, destination);
  }
  void append_encoded_to(std::string& destination) const;
  [[nodiscard]] std::string_view encoded_head() const noexcept { return head_; }
  [[nodiscard]] std::string_view encoded_tail() const noexcept { return tail_; }

  friend bool operator==(const EncodedStringView& encoded,
                         std::string_view logical) {
    return encoded.equals(logical);
  }
  friend bool operator==(std::string_view logical,
                         const EncodedStringView& encoded) {
    return encoded.equals(logical);
  }
  friend bool operator!=(const EncodedStringView& encoded,
                         std::string_view logical) {
    return !encoded.equals(logical);
  }

 private:
  [[nodiscard]] std::uint8_t byte_at(std::size_t index) const noexcept;
  void copy_range(std::size_t offset, std::size_t length,
                  char* destination) const noexcept;
  [[nodiscard]] bool range_equals(std::size_t offset,
                                  std::string_view logical) const noexcept;

  std::string_view head_;
  std::string_view tail_;
  bool encoding_enabled_{true};
};

// Values within the raw limit always fit. Larger values are classified and
// compressed once to determine whether their encoded form fits the u16 arena
// length; allocation failures still propagate.
[[nodiscard]] bool string_value_fits(
    std::string_view value, StringEncodingOptions options = {},
    StringCompressionMode compression = StringCompressionMode::AllowLz4);

}  // namespace goblin::core
