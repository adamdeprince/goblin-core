#include "goblin/core/string_encoding.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#include <lz4.h>
#include <lz4hc.h>

namespace goblin::core {
namespace {

[[nodiscard]] bool parse_canonical_magnitude(std::string_view digits,
                                             std::uint64_t& value) noexcept {
  if (digits.empty() || (digits.size() > 1 && digits.front() == '0')) {
    return false;
  }
  const auto* begin = digits.data();
  const auto* end = begin + digits.size();
  const auto [ptr, error] = std::from_chars(begin, end, value, 10);
  return error == std::errc{} && ptr == end;
}

void write_big_endian(char* destination, std::uint64_t value,
                      std::size_t width) noexcept {
  for (std::size_t index = 0; index < width; ++index) {
    destination[width - index - 1] = static_cast<char>(value & 0xff);
    value >>= 8;
  }
}

[[nodiscard]] std::uint64_t read_big_endian(const EncodedStringView& encoded,
                                            std::size_t offset,
                                            std::size_t width) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < width; ++index) {
    value = (value << 8) | encoded.encoded_byte(offset + index);
  }
  return value;
}

[[nodiscard]] std::uint32_t read_u24_big_endian(
    const EncodedStringView& encoded, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(
      read_big_endian(encoded, offset, 3));
}

[[nodiscard]] std::size_t width_for_offset(std::uint64_t& offset) noexcept {
  for (std::size_t width = 1; width < 8; ++width) {
    const auto capacity = std::uint64_t{1} << (width * 8);
    if (offset < capacity) {
      return width;
    }
    offset -= capacity;
  }
  return 8;
}

[[nodiscard]] std::uint64_t positive_base(std::size_t width) noexcept {
  std::uint64_t base = 128;
  for (std::size_t prior = 1; prior < width; ++prior) {
    base += std::uint64_t{1} << (prior * 8);
  }
  return base;
}

[[nodiscard]] std::uint64_t negative_base(std::size_t width) noexcept {
  std::uint64_t base = 1;
  for (std::size_t prior = 1; prior < width; ++prior) {
    base += std::uint64_t{1} << (prior * 8);
  }
  return base;
}

[[nodiscard]] bool lower_hex_nibble(char ch, std::uint8_t& value) noexcept {
  if (ch >= '0' && ch <= '9') {
    value = static_cast<std::uint8_t>(ch - '0');
    return true;
  }
  if (ch >= 'a' && ch <= 'f') {
    value = static_cast<std::uint8_t>(ch - 'a' + 10);
    return true;
  }
  return false;
}

[[nodiscard]] bool encode_uuid(std::string_view value, bool dashed,
                               std::array<char, 17>& output) noexcept {
  if (value.size() != (dashed ? 36 : 32)) {
    return false;
  }
  constexpr std::array<std::size_t, 4> kDashes{8, 13, 18, 23};
  std::size_t source = 0;
  for (std::size_t byte = 0; byte < 16; ++byte) {
    if (dashed && std::find(kDashes.begin(), kDashes.end(), source) !=
                      kDashes.end()) {
      if (value[source] != '-') {
        return false;
      }
      ++source;
    }
    std::uint8_t high = 0;
    std::uint8_t low = 0;
    if (!lower_hex_nibble(value[source], high) ||
        !lower_hex_nibble(value[source + 1], low)) {
      return false;
    }
    output[byte + 1] = static_cast<char>((high << 4) | low);
    source += 2;
  }
  return source == value.size();
}

[[nodiscard]] std::size_t decimal_digits(std::uint64_t value) noexcept {
  std::size_t digits = 1;
  while (value >= 10) {
    value /= 10;
    ++digits;
  }
  return digits;
}

[[nodiscard]] char lower_hex(std::uint8_t value) noexcept {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('a' + value - 10);
}

}  // namespace

EncodedString::EncodedString(std::string_view value,
                             StringEncodingOptions options,
                             StringCompressionMode compression)
    : source_(value) {
  if (!options.encoding_enabled()) {
    if (value.size() > kStringMaxVerbatimBytes) {
      throw std::length_error(
          "verbatim string value too large (max 65,535 bytes)");
    }
    storage_ = Storage::Verbatim;
    encoded_size_ = static_cast<std::uint16_t>(value.size());
    return;
  }

  std::uint64_t magnitude = 0;
  if (parse_canonical_magnitude(value, magnitude)) {
    storage_ = Storage::Inline;
    if (magnitude <= 127) {
      inline_[0] = static_cast<char>(magnitude);
      encoded_size_ = 1;
      return;
    }
    auto offset = magnitude - 128;
    const auto width = width_for_offset(offset);
    inline_[0] = static_cast<char>(kStringPositiveBase + width - 1);
    write_big_endian(inline_.data() + 1, offset, width);
    encoded_size_ = static_cast<std::uint16_t>(width + 1);
    return;
  }

  if (value.size() > 1 && value.front() == '-' &&
      parse_canonical_magnitude(value.substr(1), magnitude) && magnitude != 0) {
    storage_ = Storage::Inline;
    auto offset = magnitude - 1;
    const auto width = width_for_offset(offset);
    inline_[0] = static_cast<char>(kStringNegativeBase + width - 1);
    write_big_endian(inline_.data() + 1, offset, width);
    encoded_size_ = static_cast<std::uint16_t>(width + 1);
    return;
  }

  if (encode_uuid(value, false, inline_)) {
    storage_ = Storage::Inline;
    inline_[0] = static_cast<char>(kStringUuidHex);
    encoded_size_ = 17;
    return;
  }
  if (encode_uuid(value, true, inline_)) {
    storage_ = Storage::Inline;
    inline_[0] = static_cast<char>(kStringUuidDashed);
    encoded_size_ = 17;
    return;
  }

  const bool may_compress =
      compression == StringCompressionMode::AllowLz4 &&
      options.lz4_enabled() &&
      (value.size() >= options.lz4_min_bytes() ||
       value.size() > kStringMaxBytes);
  if (may_compress && value.size() <= kStringMaxCompressedLogicalBytes) {
    constexpr std::size_t kLogicalLengthBytes = 3;
    constexpr std::size_t kHeaderBytes = 1 + kLogicalLengthBytes;
    constexpr std::size_t kMaxCompressedBytes =
        kStringMaxEncodedBytes - kHeaderBytes;
    const auto source_size = static_cast<int>(value.size());
    const auto bound = LZ4_compressBound(source_size);
    const auto destination_capacity =
        std::min<std::size_t>(static_cast<std::size_t>(bound),
                              kMaxCompressedBytes);
    lz4_.resize(destination_capacity + kHeaderBytes);
    lz4_[0] = static_cast<char>(kStringLz4);
    write_big_endian(lz4_.data() + 1, value.size(), kLogicalLengthBytes);
    static constexpr char kEmpty = 0;
    const auto* source = value.empty() ? &kEmpty : value.data();
    const auto level = options.lz4_compress_level();
    int compressed = 0;
    if (level > 0) {
      compressed = LZ4_compress_HC(
          source, lz4_.data() + kHeaderBytes, source_size,
          static_cast<int>(destination_capacity), level);
    } else if (level < 0) {
      compressed = LZ4_compress_fast(
          source, lz4_.data() + kHeaderBytes, source_size,
          static_cast<int>(destination_capacity), -level);
    } else {
      compressed = LZ4_compress_default(
          source, lz4_.data() + kHeaderBytes, source_size,
          static_cast<int>(destination_capacity));
    }
    if (compressed > 0) {
      storage_ = Storage::Lz4;
      lz4_.resize(static_cast<std::size_t>(compressed) + kHeaderBytes);
      encoded_size_ = static_cast<std::uint16_t>(lz4_.size());
      return;
    }
    lz4_.clear();
  }

  if (value.size() > kStringMaxBytes) {
    throw std::length_error(
        value.size() > kStringMaxCompressedLogicalBytes
            ? "string value exceeds the 24-bit compressed logical length"
            : options.lz4_enabled() &&
                compression == StringCompressionMode::AllowLz4
                  ? "string value is neither LZ4-compressible nor raw-storable"
                  : "string value too large (max 65,534 raw bytes)");
  }

  storage_ = Storage::Raw;
  encoded_size_ = static_cast<std::uint16_t>(value.size() + 1);
}

std::uint8_t EncodedString::prefix() const noexcept {
  switch (storage_) {
    case Storage::Verbatim:
      return kStringRaw;
    case Storage::Raw:
      return kStringRaw;
    case Storage::Inline:
      return static_cast<std::uint8_t>(inline_[0]);
    case Storage::Lz4:
      return kStringLz4;
  }
  return kStringRaw;
}

void EncodedString::write_to(char* destination) const noexcept {
  switch (storage_) {
    case Storage::Verbatim:
      if (!source_.empty()) {
        std::memcpy(destination, source_.data(), source_.size());
      }
      return;
    case Storage::Raw:
      destination[0] = static_cast<char>(kStringRaw);
      if (!source_.empty()) {
        std::memcpy(destination + 1, source_.data(), source_.size());
      }
      return;
    case Storage::Inline:
      std::memcpy(destination, inline_.data(), encoded_size_);
      return;
    case Storage::Lz4:
      std::memcpy(destination, lz4_.data(), lz4_.size());
      return;
  }
}

void EncodedString::write_range_to(std::size_t offset, std::size_t length,
                                   char* destination) const noexcept {
  if (length == 0) {
    return;
  }
  switch (storage_) {
    case Storage::Verbatim:
      std::memcpy(destination, source_.data() + offset, length);
      return;
    case Storage::Raw: {
      if (offset == 0) {
        destination[0] = static_cast<char>(kStringRaw);
        ++destination;
        ++offset;
        --length;
      }
      if (length != 0) {
        std::memcpy(destination, source_.data() + offset - 1, length);
      }
      return;
    }
    case Storage::Inline:
      std::memcpy(destination, inline_.data() + offset, length);
      return;
    case Storage::Lz4:
      std::memcpy(destination, lz4_.data() + offset, length);
      return;
  }
}

bool EncodedString::equals_encoded(std::string_view stored) const noexcept {
  if (stored.size() != encoded_size_) {
    return false;
  }
  switch (storage_) {
    case Storage::Verbatim:
      return stored == source_;
    case Storage::Raw:
      return !stored.empty() &&
             static_cast<std::uint8_t>(stored.front()) == kStringRaw &&
             stored.substr(1) == source_;
    case Storage::Inline:
      return std::memcmp(stored.data(), inline_.data(), encoded_size_) == 0;
    case Storage::Lz4:
      return stored == lz4_;
  }
  return false;
}

std::uint8_t EncodedStringView::byte_at(std::size_t index) const noexcept {
  if (index < head_.size()) {
    return static_cast<std::uint8_t>(head_[index]);
  }
  return static_cast<std::uint8_t>(tail_[index - head_.size()]);
}

std::uint8_t EncodedStringView::prefix() const noexcept {
  return !encoding_enabled_ || encoded_size() == 0 ? kStringRaw : byte_at(0);
}

bool EncodedStringView::valid() const noexcept {
  const auto bytes = encoded_size();
  if (!encoding_enabled_) {
    return bytes <= kStringMaxVerbatimBytes;
  }
  if (bytes == 0 || bytes > kStringMaxEncodedBytes) {
    return false;
  }
  const auto tag = prefix();
  if (tag <= 0x7f) {
    return bytes == 1;
  }
  if (tag >= kStringPositiveBase && tag < kStringUuidHex) {
    const auto width = tag < kStringNegativeBase
                           ? tag - kStringPositiveBase + 1
                           : tag - kStringNegativeBase + 1;
    return bytes == static_cast<std::size_t>(width) + 1;
  }
  if (tag == kStringUuidHex || tag == kStringUuidDashed) {
    return bytes == 17;
  }
  if (tag == kStringLz4) {
    return bytes >= 1 + 3 + 1 &&
           read_u24_big_endian(*this, 1) <=
               kStringMaxCompressedLogicalBytes;
  }
  return tag == kStringRaw;
}

std::size_t EncodedStringView::size() const noexcept {
  if (!encoding_enabled_) {
    return encoded_size();
  }
  if (!valid()) {
    return 0;
  }
  const auto tag = prefix();
  if (tag <= 0x7f) {
    return decimal_digits(tag);
  }
  if (tag >= kStringPositiveBase && tag < kStringNegativeBase) {
    const auto width = static_cast<std::size_t>(tag - kStringPositiveBase + 1);
    const auto value = positive_base(width) + read_big_endian(*this, 1, width);
    return decimal_digits(value);
  }
  if (tag >= kStringNegativeBase && tag < kStringUuidHex) {
    const auto width = static_cast<std::size_t>(tag - kStringNegativeBase + 1);
    const auto magnitude =
        negative_base(width) + read_big_endian(*this, 1, width);
    return 1 + decimal_digits(magnitude);
  }
  if (tag == kStringUuidHex) {
    return 32;
  }
  if (tag == kStringUuidDashed) {
    return 36;
  }
  if (tag == kStringLz4) {
    return read_u24_big_endian(*this, 1);
  }
  return encoded_size() - 1;
}

void EncodedStringView::copy_range(std::size_t offset, std::size_t length,
                                   char* destination) const noexcept {
  const auto from_head =
      offset < head_.size() ? std::min(length, head_.size() - offset) : 0;
  if (from_head != 0) {
    std::memcpy(destination, head_.data() + offset, from_head);
  }
  const auto tail_offset = offset + from_head > head_.size()
                               ? offset + from_head - head_.size()
                               : 0;
  if (length > from_head) {
    std::memcpy(destination + from_head, tail_.data() + tail_offset,
                length - from_head);
  }
}

void EncodedStringView::copy_encoded_to(char* destination) const noexcept {
  if (!head_.empty()) {
    std::memcpy(destination, head_.data(), head_.size());
  }
  if (!tail_.empty()) {
    std::memcpy(destination + head_.size(), tail_.data(), tail_.size());
  }
}

void EncodedStringView::append_encoded_to(std::string& destination) const {
  destination.append(head_);
  destination.append(tail_);
}

bool EncodedStringView::range_equals(std::size_t offset,
                                     std::string_view logical) const noexcept {
  if (offset + logical.size() > encoded_size()) {
    return false;
  }
  const auto from_head =
      offset < head_.size()
          ? std::min(logical.size(), head_.size() - offset)
          : 0;
  if (from_head != 0 &&
      head_.substr(offset, from_head) != logical.substr(0, from_head)) {
    return false;
  }
  const auto tail_offset = offset + from_head > head_.size()
                               ? offset + from_head - head_.size()
                               : 0;
  return tail_.substr(tail_offset, logical.size() - from_head) ==
         logical.substr(from_head);
}

void EncodedStringView::append_to(std::string& destination) const {
  if (!valid()) {
    throw std::runtime_error("invalid stored string encoding");
  }
  if (!encoding_enabled_) {
    destination.append(head_);
    destination.append(tail_);
    return;
  }
  const auto tag = prefix();
  if (tag == kStringRaw) {
    const auto prior = destination.size();
    destination.resize(prior + encoded_size() - 1);
    copy_range(1, encoded_size() - 1, destination.data() + prior);
    return;
  }

  std::array<char, 36> decoded{};
  if (tag <= 0x7f) {
    const auto [end, error] =
        std::to_chars(decoded.data(), decoded.data() + decoded.size(), tag);
    if (error != std::errc{}) {
      throw std::runtime_error("failed to decode stored integer string");
    }
    destination.append(decoded.data(), end);
    return;
  }
  if (tag >= kStringPositiveBase && tag < kStringNegativeBase) {
    const auto width = static_cast<std::size_t>(tag - kStringPositiveBase + 1);
    const auto value = positive_base(width) + read_big_endian(*this, 1, width);
    const auto [end, error] =
        std::to_chars(decoded.data(), decoded.data() + decoded.size(), value);
    if (error != std::errc{}) {
      throw std::runtime_error("failed to decode stored integer string");
    }
    destination.append(decoded.data(), end);
    return;
  }
  if (tag >= kStringNegativeBase && tag < kStringUuidHex) {
    const auto width = static_cast<std::size_t>(tag - kStringNegativeBase + 1);
    const auto magnitude =
        negative_base(width) + read_big_endian(*this, 1, width);
    decoded[0] = '-';
    const auto [end, error] = std::to_chars(
        decoded.data() + 1, decoded.data() + decoded.size(), magnitude);
    if (error != std::errc{}) {
      throw std::runtime_error("failed to decode stored integer string");
    }
    destination.append(decoded.data(), end);
    return;
  }
  if (tag == kStringUuidHex || tag == kStringUuidDashed) {
    constexpr std::array<std::size_t, 4> kDashes{8, 13, 18, 23};
    std::size_t output = 0;
    for (std::size_t byte = 0; byte < 16; ++byte) {
      if (tag == kStringUuidDashed &&
          std::find(kDashes.begin(), kDashes.end(), output) != kDashes.end()) {
        decoded[output++] = '-';
      }
      const auto value = byte_at(byte + 1);
      decoded[output++] = lower_hex(value >> 4);
      decoded[output++] = lower_hex(value & 0x0f);
    }
    destination.append(decoded.data(), output);
    return;
  }

  const auto decoded_length = size();
  constexpr std::size_t kHeaderBytes = 1 + 3;
  const auto compressed_length = encoded_size() - kHeaderBytes;
  const char* compressed = nullptr;
  static thread_local std::string compressed_scratch;
  if (head_.size() >= kHeaderBytes + compressed_length) {
    compressed = head_.data() + kHeaderBytes;
  } else if (head_.size() <= kHeaderBytes) {
    compressed = tail_.data() + (kHeaderBytes - head_.size());
  } else {
    compressed_scratch.resize(compressed_length);
    copy_range(kHeaderBytes, compressed_length, compressed_scratch.data());
    compressed = compressed_scratch.data();
  }
  const auto prior = destination.size();
  destination.resize(prior + decoded_length);
  if (decoded_length == 0) {
    return;
  }
  const int result = LZ4_decompress_safe(
      compressed, destination.data() + prior,
      static_cast<int>(compressed_length), static_cast<int>(decoded_length));
  if (result != static_cast<int>(decoded_length)) {
    destination.resize(prior);
    throw std::runtime_error("invalid stored LZ4 string encoding");
  }
}

std::string EncodedStringView::to_string() const {
  std::string result;
  result.reserve(size());
  append_to(result);
  return result;
}

bool EncodedStringView::equals(std::string_view logical) const {
  if (!valid() || size() != logical.size()) {
    return false;
  }
  if (!encoding_enabled_) {
    return range_equals(0, logical);
  }
  if (prefix() == kStringRaw) {
    return range_equals(1, logical);
  }
  return to_string() == logical;
}

bool string_value_fits(std::string_view value, StringEncodingOptions options,
                       StringCompressionMode compression) {
  if (value.size() <= options.max_value_bytes()) {
    return true;
  }
  try {
    (void)EncodedString(value, options, compression);
    return true;
  } catch (const std::length_error&) {
    return false;
  }
}

}  // namespace goblin::core
