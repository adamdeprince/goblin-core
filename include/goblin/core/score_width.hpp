#pragma once

// Per-zset score-width narrowing. A score is a double at the API boundary, but a
// zset stores its scores at the narrowest SIGNED integer width that holds them
// all exactly -- i16 (chess ±centipawns, ratings), i32, else f64 -- promoting
// one-way when a value doesn't fit. Both the member-storage SoA and the sorted
// score index use this. Width is orthogonal to ordering: an in-range integer
// compares identically to its double, so the SortedList is unchanged.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "goblin/core/score_format.hpp"

namespace goblin::core {

enum class ScoreWidth : std::uint8_t { I16 = 0, I32 = 1, F64 = 2 };

// Does `value` fit exactly in `width`? Fractional / -0.0 / NaN / inf never fit an
// integer width (exact_integer_score rejects them); they fall through to F64.
[[nodiscard]] inline bool score_fits(double value, ScoreWidth width) noexcept {
  if (width == ScoreWidth::F64) {
    return true;
  }
  long long integer = 0;
  if (!score_format::detail::exact_integer_score(value, integer)) {
    return false;
  }
  if (width == ScoreWidth::I16) {
    return integer >= -32768 && integer <= 32767;
  }
  return integer >= -2147483648LL && integer <= 2147483647LL;  // I32
}

// The narrowest width holding `value`, at least `floor` (one-way widen: promotion
// never narrows the running width below what it already reached).
[[nodiscard]] inline ScoreWidth score_width_for(
    double value, ScoreWidth floor = ScoreWidth::I16) noexcept {
  auto width = floor;
  while (width != ScoreWidth::F64 && !score_fits(value, width)) {
    width = static_cast<ScoreWidth>(static_cast<std::uint8_t>(width) + 1);
  }
  return width;
}

[[nodiscard]] constexpr std::string_view score_width_name(
    ScoreWidth width) noexcept {
  switch (width) {
    case ScoreWidth::I16:
      return "i16";
    case ScoreWidth::I32:
      return "i32";
    case ScoreWidth::F64:
      return "f64";
  }
  return "f64";
}

[[nodiscard]] inline std::size_t score_width_bytes(ScoreWidth width) noexcept {
  switch (width) {
    case ScoreWidth::I16:
      return sizeof(std::int16_t);
    case ScoreWidth::I32:
      return sizeof(std::int32_t);
    case ScoreWidth::F64:
      return sizeof(double);
  }
  return sizeof(double);
}

// A growable score array stored at a runtime-selected width, read and written as
// double. Widens automatically (never narrows) when a value doesn't fit the
// current width; rebuild_to() is the explicit path OPTIMIZE uses to demote after
// scanning. Copyable (the member layer's SoA is copied on a CoW split).
class ScoreArray {
 public:
  ScoreArray() : data_(std::vector<std::int16_t>{}) {}

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
    return capacity() * score_width_bytes(width());
  }
  [[nodiscard]] double at(std::size_t index) const noexcept {
    return std::visit(
        [index](const auto& v) { return static_cast<double>(v[index]); }, data_);
  }
  void reserve(std::size_t count) {
    std::visit([count](auto& v) { v.reserve(count); }, data_);
  }
  void pop_back() noexcept {
    std::visit([](auto& v) { v.pop_back(); }, data_);
  }
  void copy_within(std::size_t dst, std::size_t src) noexcept {
    std::visit([dst, src](auto& v) { v[dst] = v[src]; }, data_);
  }
  // Widen to hold `value` if needed, then append it.
  void push_back(double value) {
    widen_for(value);
    std::visit(
        [value](auto& v) {
          using T = typename std::decay_t<decltype(v)>::value_type;
          v.push_back(static_cast<T>(value));
        },
        data_);
  }
  // Widen to hold `value` if needed, then store at `index`.
  void set(std::size_t index, double value) {
    widen_for(value);
    std::visit(
        [index, value](auto& v) {
          using T = typename std::decay_t<decltype(v)>::value_type;
          v[index] = static_cast<T>(value);
        },
        data_);
  }
  // Explicit rebuild to a target width (OPTIMIZE demote; caller guarantees fit).
  void rebuild_to(ScoreWidth target) {
    if (target == width()) {
      return;
    }
    switch (target) {
      case ScoreWidth::I16:
        data_ = to_vector<std::int16_t>();
        break;
      case ScoreWidth::I32:
        data_ = to_vector<std::int32_t>();
        break;
      case ScoreWidth::F64:
        data_ = to_vector<double>();
        break;
    }
  }

 private:
  using Storage = std::variant<std::vector<std::int16_t>,
                               std::vector<std::int32_t>, std::vector<double>>;

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
      out.push_back(static_cast<T>(at(i)));
    }
    return out;
  }

  Storage data_;
};

}  // namespace goblin::core
