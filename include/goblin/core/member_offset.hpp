#pragma once

// 48-bit virtual byte offsets into a chunked member arena: 32-bit low + 16-bit
// high (256 TiB). Packed member metadata is a fixed 8-byte location
// (offset[6] + length[2]) followed by a score tail sized exactly to the
// current score width (2/4/8 bytes). Stride is 10/12/16 bytes; records are
// usually misaligned for wider loads — access via memcpy (LoongArch-safe).

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <variant>
#include <vector>

#include "goblin/core/score_width.hpp"

namespace goblin::core {

// How member metadata (arena location + score) is stored. Chosen at process
// startup via --member-meta-mode; there is no runtime promotion between modes.
enum class MemberMetaMode : std::uint8_t { Packed, PartialAoS, FullAoS };

[[nodiscard]] constexpr std::string_view member_meta_mode_name(
    MemberMetaMode mode) noexcept {
  switch (mode) {
    case MemberMetaMode::Packed:
      return "packed";
    case MemberMetaMode::PartialAoS:
      return "partial-aos";
    case MemberMetaMode::FullAoS:
      return "full-aos";
  }
  return "packed";
}

inline constexpr std::uint64_t kMemberOffset48Max =
    (std::uint64_t{1} << 48) - 1;

inline constexpr std::size_t kPackedLocationBytes = 8;

[[nodiscard]] inline bool member_offset_fits48(std::uint64_t offset) noexcept {
  return offset <= kMemberOffset48Max;
}

[[nodiscard]] constexpr std::size_t packed_member_stride(ScoreWidth width) noexcept {
  return kPackedLocationBytes + score_width_bytes(width);
}

static_assert(packed_member_stride(ScoreWidth::I16) - score_width_bytes(ScoreWidth::I16) ==
              kPackedLocationBytes);
static_assert(packed_member_stride(ScoreWidth::I32) - score_width_bytes(ScoreWidth::I32) ==
              kPackedLocationBytes);
static_assert(packed_member_stride(ScoreWidth::F64) - score_width_bytes(ScoreWidth::F64) ==
              kPackedLocationBytes);

[[nodiscard]] inline std::uint64_t member_offset48_decode(
    const std::uint8_t bytes[6]) noexcept {
  std::uint64_t offset = 0;
  for (int i = 5; i >= 0; --i) {
    offset = (offset << 8) | static_cast<std::uint64_t>(bytes[i]);
  }
  return offset;
}

inline void member_offset48_encode(std::uint8_t bytes[6],
                                   std::uint64_t offset) noexcept {
  assert(member_offset_fits48(offset));
  for (int i = 0; i < 6; ++i) {
    bytes[i] = static_cast<std::uint8_t>(offset & 0xFFU);
    offset >>= 8;
  }
}

struct PackedMemberLocation {
  std::uint8_t offset[6]{};
  std::uint16_t length{0};
};

static_assert(sizeof(PackedMemberLocation) == kPackedLocationBytes);
static_assert(std::is_standard_layout_v<PackedMemberLocation>);

inline void write_packed_location(std::uint8_t* base, std::size_t stride,
                                  std::size_t member_id, std::uint64_t offset,
                                  std::uint16_t length) noexcept {
  PackedMemberLocation loc{};
  member_offset48_encode(loc.offset, offset);
  loc.length = length;
  std::memcpy(base + member_id * stride, &loc, kPackedLocationBytes);
}

[[nodiscard]] inline PackedMemberLocation read_packed_location(
    const std::uint8_t* base, std::size_t stride, std::size_t member_id) noexcept {
  PackedMemberLocation loc{};
  std::memcpy(&loc, base + member_id * stride, kPackedLocationBytes);
  return loc;
}

[[nodiscard]] inline std::uint64_t read_packed_member_offset(
    const std::uint8_t* base, std::size_t stride, std::size_t member_id) noexcept {
  return member_offset48_decode(
      read_packed_location(base, stride, member_id).offset);
}

[[nodiscard]] inline std::uint16_t read_packed_member_length(
    const std::uint8_t* base, std::size_t stride, std::size_t member_id) noexcept {
  return read_packed_location(base, stride, member_id).length;
}

inline void write_packed_score(std::uint8_t* base, std::size_t stride,
                               ScoreWidth width, std::size_t member_id,
                               double score) noexcept {
  const auto offset = member_id * stride + kPackedLocationBytes;
  switch (width) {
    case ScoreWidth::I16: {
      const auto value = static_cast<std::int16_t>(score);
      std::memcpy(base + offset, &value, sizeof(value));
      break;
    }
    case ScoreWidth::I32: {
      const auto value = static_cast<std::int32_t>(score);
      std::memcpy(base + offset, &value, sizeof(value));
      break;
    }
    case ScoreWidth::F64: {
      std::memcpy(base + offset, &score, sizeof(score));
      break;
    }
  }
}

[[nodiscard]] inline double read_packed_score(const std::uint8_t* base,
                                              std::size_t stride, ScoreWidth width,
                                              std::size_t member_id) noexcept {
  const auto offset = member_id * stride + kPackedLocationBytes;
  switch (width) {
    case ScoreWidth::I16: {
      std::int16_t value{};
      std::memcpy(&value, base + offset, sizeof(value));
      return static_cast<double>(value);
    }
    case ScoreWidth::I32: {
      std::int32_t value{};
      std::memcpy(&value, base + offset, sizeof(value));
      return static_cast<double>(value);
    }
    case ScoreWidth::F64: {
      double value{};
      std::memcpy(&value, base + offset, sizeof(value));
      return value;
    }
  }
  return 0.0;
}

inline void write_packed_member(std::uint8_t* base, std::size_t stride,
                                ScoreWidth width, std::size_t member_id,
                                std::uint64_t offset, std::uint16_t length,
                                double score) noexcept {
  write_packed_location(base, stride, member_id, offset, length);
  write_packed_score(base, stride, width, member_id, score);
}

inline void copy_packed_member(const std::uint8_t* src, std::uint8_t* dst,
                               std::size_t stride, std::size_t member_id) noexcept {
  std::memcpy(dst + member_id * stride, src + member_id * stride, stride);
}

// Full AoS: one {location, score} struct per member at the zset's current
// narrow score width (separate vector types for i16/i32/f64, same byte sizes as
// packed). Chosen at startup via --member-meta-mode full-aos.
struct FullAoSRecordI16 {
  PackedMemberLocation location{};
  std::int16_t score{0};
};
struct FullAoSRecordI32 {
  PackedMemberLocation location{};
  std::int32_t score{0};
};
struct FullAoSRecordF64 {
  PackedMemberLocation location{};
  double score{0.0};
};

static_assert(sizeof(FullAoSRecordI16) == packed_member_stride(ScoreWidth::I16));
static_assert(sizeof(FullAoSRecordI32) == packed_member_stride(ScoreWidth::I32));
static_assert(sizeof(FullAoSRecordF64) == packed_member_stride(ScoreWidth::F64));

[[nodiscard]] inline PackedMemberLocation make_packed_location(
    std::uint64_t offset, std::uint16_t length) noexcept {
  PackedMemberLocation loc{};
  member_offset48_encode(loc.offset, offset);
  loc.length = length;
  return loc;
}

class FullAoSRecords {
 public:
  FullAoSRecords() : data_(std::vector<FullAoSRecordI16>{}) {}

  [[nodiscard]] ScoreWidth width() const noexcept {
    return static_cast<ScoreWidth>(data_.index());
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return std::visit([](const auto& v) { return v.size(); }, data_);
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return std::visit([](const auto& v) { return v.capacity(); }, data_);
  }

  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return capacity() * packed_member_stride(width());
  }

  [[nodiscard]] double score(std::size_t index) const noexcept {
    return std::visit(
        [index](const auto& v) { return static_cast<double>(v[index].score); },
        data_);
  }

  [[nodiscard]] PackedMemberLocation location(std::size_t index) const noexcept {
    return std::visit([index](const auto& v) { return v[index].location; },
                      data_);
  }

  [[nodiscard]] std::uint64_t member_offset(std::size_t index) const noexcept {
    return member_offset48_decode(location(index).offset);
  }

  [[nodiscard]] std::uint16_t member_length(std::size_t index) const noexcept {
    return location(index).length;
  }

  void reserve(std::size_t count) {
    std::visit([count](auto& v) { v.reserve(count); }, data_);
  }

  void shrink_to_fit() {
    std::visit([](auto& v) { v.shrink_to_fit(); }, data_);
  }

  void pop_back() noexcept {
    std::visit([](auto& v) { v.pop_back(); }, data_);
  }

  void copy_within(std::size_t dst, std::size_t src) noexcept {
    std::visit([dst, src](auto& v) { v[dst] = v[src]; }, data_);
  }

  void append(std::uint64_t offset, std::uint16_t length, double value) {
    widen_for(value);
    const auto loc = make_packed_location(offset, length);
    std::visit(
        [&loc, value](auto& v) {
          using T = typename std::decay_t<decltype(v)>::value_type;
          v.push_back(T{.location = loc,
                        .score = static_cast<decltype(T::score)>(value)});
        },
        data_);
  }

  void write(std::size_t index, std::uint64_t offset, std::uint16_t length,
             double value) {
    widen_for(value);
    const auto loc = make_packed_location(offset, length);
    std::visit(
        [index, &loc, value](auto& v) {
          using T = typename std::decay_t<decltype(v)>::value_type;
          v[index] = T{.location = loc,
                       .score = static_cast<decltype(T::score)>(value)};
        },
        data_);
  }

  void write_location(std::size_t index, std::uint64_t offset,
                      std::uint16_t length) noexcept {
    const auto loc = make_packed_location(offset, length);
    std::visit([index, &loc](auto& v) { v[index].location = loc; }, data_);
  }

  void set_score(std::size_t index, double value) {
    widen_for(value);
    std::visit(
        [index, value](auto& v) {
          using ScoreT = decltype(v[index].score);
          v[index].score = static_cast<ScoreT>(value);
        },
        data_);
  }

  void rebuild_to(ScoreWidth target) {
    if (target == width()) {
      return;
    }
    switch (target) {
      case ScoreWidth::I16:
        data_ = to_vector<FullAoSRecordI16>();
        break;
      case ScoreWidth::I32:
        data_ = to_vector<FullAoSRecordI32>();
        break;
      case ScoreWidth::F64:
        data_ = to_vector<FullAoSRecordF64>();
        break;
    }
  }

  void prefetch(std::size_t index) const noexcept {
#if defined(__GNUC__) || defined(__clang__)
    std::visit([index](const auto& v) { __builtin_prefetch(&v[index]); }, data_);
#else
    (void)index;
#endif
  }

 private:
  using Storage = std::variant<std::vector<FullAoSRecordI16>,
                               std::vector<FullAoSRecordI32>,
                               std::vector<FullAoSRecordF64>>;

  void widen_for(double value) {
    if (!score_fits(value, width())) {
      rebuild_to(score_width_for(value, width()));
    }
  }

  template <class T>
  [[nodiscard]] std::vector<T> to_vector() const {
    const std::size_t count = size();
    std::vector<T> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      out.push_back(T{.location = location(i),
                      .score = static_cast<decltype(T::score)>(score(i))});
    }
    return out;
  }

  Storage data_;
};

}  // namespace goblin::core