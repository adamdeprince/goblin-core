#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "goblin/core/memory_limit.hpp"
#include "goblin/core/packed_bplus_tree.hpp"
#include "goblin/core/swiss_table.hpp"

namespace goblin::core {

// The six wire-visible packed sorted-set representations. Zero is deliberately
// not a valid kind so Command can use it for an ordinary Redis ZSET command.
enum class PackedZSetKind : std::uint8_t {
  Int32Float32 = 1,
  Int32Float64 = 2,
  Int64Float32 = 3,
  Int64Float64 = 4,
  UuidFloat32 = 5,
  UuidFloat64 = 6,
};

// Representation selected for zsets created by ordinary, unqualified Z*
// commands. Existing keys retain their representation; the six nonzero values
// deliberately line up with PackedZSetKind so the selector is cheap to resolve
// on the command path.
enum class ZSetImplementation : std::uint8_t {
  Standard = 0,
  PackedInt32Float32 = 1,
  PackedInt32Float64 = 2,
  PackedInt64Float32 = 3,
  PackedInt64Float64 = 4,
  PackedUuidFloat32 = 5,
  PackedUuidFloat64 = 6,
};

[[nodiscard]] constexpr std::optional<PackedZSetKind>
packed_zset_kind_for_implementation(ZSetImplementation implementation) noexcept {
  if (implementation == ZSetImplementation::Standard) {
    return std::nullopt;
  }
  return static_cast<PackedZSetKind>(implementation);
}

[[nodiscard]] constexpr std::string_view zset_implementation_name(
    ZSetImplementation implementation) noexcept {
  switch (implementation) {
    case ZSetImplementation::Standard:
      return "standard";
    case ZSetImplementation::PackedInt32Float32:
      return "packed-int32-float32";
    case ZSetImplementation::PackedInt32Float64:
      return "packed-int32-float64";
    case ZSetImplementation::PackedInt64Float32:
      return "packed-int64-float32";
    case ZSetImplementation::PackedInt64Float64:
      return "packed-int64-float64";
    case ZSetImplementation::PackedUuidFloat32:
      return "packed-uuid-float32";
    case ZSetImplementation::PackedUuidFloat64:
      return "packed-uuid-float64";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::optional<ZSetImplementation>
parse_zset_implementation(std::string_view text) noexcept {
  if (text == "standard" || text == "efficient" ||
      text == "string-float64") {
    return ZSetImplementation::Standard;
  }
  if (text == "packed-int32-float32" || text == "int32-float32" ||
      text == "packed_int32_float32" || text == "INT32_FLOAT32") {
    return ZSetImplementation::PackedInt32Float32;
  }
  if (text == "packed-int32-float64" || text == "int32-float64" ||
      text == "packed_int32_float64" || text == "INT32_FLOAT64") {
    return ZSetImplementation::PackedInt32Float64;
  }
  if (text == "packed-int64-float32" || text == "int64-float32" ||
      text == "packed_int64_float32" || text == "INT64_FLOAT32") {
    return ZSetImplementation::PackedInt64Float32;
  }
  if (text == "packed-int64-float64" || text == "int64-float64" ||
      text == "packed_int64_float64" || text == "INT64_FLOAT64") {
    return ZSetImplementation::PackedInt64Float64;
  }
  if (text == "packed-uuid-float32" || text == "uuid-float32" ||
      text == "packed_uuid_float32" || text == "UUID_FLOAT32") {
    return ZSetImplementation::PackedUuidFloat32;
  }
  if (text == "packed-uuid-float64" || text == "uuid-float64" ||
      text == "packed_uuid_float64" || text == "UUID_FLOAT64") {
    return ZSetImplementation::PackedUuidFloat64;
  }
  return std::nullopt;
}

inline constexpr double kDefaultPackedZSetMergeExponent = 0.5;

[[nodiscard]] constexpr bool valid_packed_zset_merge_exponent(
    double exponent) noexcept {
  return exponent >= 0.0 && exponent <= 1.0;
}

[[nodiscard]] constexpr std::string_view packed_zset_kind_name(
    PackedZSetKind kind) noexcept {
  switch (kind) {
    case PackedZSetKind::Int32Float32:
      return "INT32_FLOAT32";
    case PackedZSetKind::Int32Float64:
      return "INT32_FLOAT64";
    case PackedZSetKind::Int64Float32:
      return "INT64_FLOAT32";
    case PackedZSetKind::Int64Float64:
      return "INT64_FLOAT64";
    case PackedZSetKind::UuidFloat32:
      return "UUID_FLOAT32";
    case PackedZSetKind::UuidFloat64:
      return "UUID_FLOAT64";
  }
  return "unknown";
}

struct PackedZSetAddOptions {
  bool nx{false};
  bool xx{false};
  bool gt{false};
  bool lt{false};
  bool increment{false};
};

struct PackedZSetAddItem {
  double score{0.0};
  std::string_view member;
};

struct PackedZSetAddResult {
  long long added{0};
  long long changed{0};
  std::optional<double> increment_score;
  bool invalid_member{false};
  bool invalid_score{false};
};

struct PackedZSetLookup {
  bool valid_member{true};
  std::optional<double> score;
};

struct PackedZSetRankLookup {
  bool valid_member{true};
  std::optional<std::size_t> rank;
};

struct PackedZSetRemoveResult {
  std::size_t removed{0};
  bool invalid_member{false};
};

struct PackedZSetEntry {
  std::string member;
  double score{0.0};
};

struct PackedZSetScanResult {
  std::uint64_t next{0};
  std::vector<PackedZSetEntry> entries;
};

namespace detail {

struct PackedUuid {
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] friend bool operator==(const PackedUuid&,
                                       const PackedUuid&) noexcept = default;
};
static_assert(sizeof(PackedUuid) == 16);
static_assert(std::is_trivially_copyable_v<PackedUuid>);

// Swiss uses high hash bits for bucket selection and low bits for fingerprints.
// Integer std::hash can be the identity, so both halves need an avalanche.
[[nodiscard]] inline constexpr std::uint64_t packed_hash_mix64(
    std::uint64_t value) noexcept {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

struct PackedUuidHash {
  [[nodiscard]] std::size_t operator()(const PackedUuid& value) const noexcept {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    std::memcpy(&low, value.bytes.data(), sizeof(low));
    std::memcpy(&high, value.bytes.data() + sizeof(low), sizeof(high));
    // The two-round mix avoids the weak low-bit fingerprints that sequential
    // UUIDs otherwise give a Swiss table.
    low ^= high + 0x9e3779b97f4a7c15ULL + (low << 6) + (low >> 2);
    return static_cast<std::size_t>(packed_hash_mix64(low));
  }
};

[[nodiscard]] inline int hex_nibble(char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

[[nodiscard]] inline std::optional<PackedUuid> parse_packed_uuid(
    std::string_view text) noexcept {
  const bool dashed = text.size() == 36;
  if (!dashed && text.size() != 32) return std::nullopt;
  constexpr std::array<std::size_t, 4> kDashes{8, 13, 18, 23};
  PackedUuid result;
  std::size_t source = 0;
  for (std::size_t index = 0; index < result.bytes.size(); ++index) {
    if (dashed && std::find(kDashes.begin(), kDashes.end(), source) !=
                      kDashes.end()) {
      if (text[source++] != '-') return std::nullopt;
    }
    const auto high = hex_nibble(text[source++]);
    const auto low = hex_nibble(text[source++]);
    if (high < 0 || low < 0) return std::nullopt;
    result.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return source == text.size() ? std::optional<PackedUuid>{result}
                               : std::nullopt;
}

[[nodiscard]] inline std::string format_packed_uuid(
    const PackedUuid& value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(36, '-');
  std::size_t output = 0;
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    if (output == 8 || output == 13 || output == 18 || output == 23) ++output;
    result[output++] = kHex[value.bytes[index] >> 4];
    result[output++] = kHex[value.bytes[index] & 0x0f];
  }
  return result;
}

template <class Integer>
struct PackedIntegerTraits {
  using Key = Integer;
  struct Hash {
    [[nodiscard]] std::size_t operator()(Key value) const noexcept {
      return static_cast<std::size_t>(
          packed_hash_mix64(static_cast<std::uint64_t>(value)));
    }
  };

  [[nodiscard]] static std::optional<Key> parse(std::string_view text) noexcept {
    if (text.empty()) return std::nullopt;
    Key result = 0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] static bool less(Key lhs, Key rhs) noexcept { return lhs < rhs; }

  [[nodiscard]] static std::string format(Key value) {
    std::array<char, std::numeric_limits<Key>::digits10 + 3> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                      value);
    return std::string(buffer.data(), result.ptr);
  }

  [[nodiscard]] static std::uint8_t radix_byte(Key value,
                                                std::size_t pass) noexcept {
    using Unsigned = std::make_unsigned_t<Key>;
    constexpr auto kSign = Unsigned{1} << (sizeof(Key) * 8 - 1);
    const auto ordered = static_cast<Unsigned>(value) ^ kSign;
    return static_cast<std::uint8_t>(ordered >> (pass * 8));
  }

  static constexpr std::size_t kRadixPasses = sizeof(Key);
};

struct PackedUuidTraits {
  using Key = PackedUuid;
  using Hash = PackedUuidHash;

  [[nodiscard]] static std::optional<Key> parse(
      std::string_view text) noexcept {
    return parse_packed_uuid(text);
  }

  [[nodiscard]] static bool less(const Key& lhs, const Key& rhs) noexcept {
    return lhs.bytes < rhs.bytes;
  }

  [[nodiscard]] static std::string format(const Key& value) {
    return format_packed_uuid(value);
  }

  [[nodiscard]] static std::uint8_t radix_byte(
      const Key& value, std::size_t pass) noexcept {
    // UUID comparison is lexicographic, so LSD radix order visits the last
    // byte first and the first byte last.
    return value.bytes[value.bytes.size() - pass - 1];
  }

  static constexpr std::size_t kRadixPasses = 16;
};

template <class Score>
[[nodiscard]] std::optional<Score> narrow_packed_score(double value) noexcept {
  if (std::isnan(value)) return std::nullopt;
  const auto narrowed = static_cast<Score>(value);
  if (std::isfinite(value) && !std::isfinite(narrowed)) return std::nullopt;
  // Treat -0 and +0 as one score, matching ordinary sorted-set comparisons and
  // keeping the radix representation consistent with operator<.
  return narrowed == Score{0} ? Score{0} : narrowed;
}

template <class Score>
[[nodiscard]] auto ordered_score_bits(Score value) noexcept {
  using Bits = std::conditional_t<sizeof(Score) == 4, std::uint32_t,
                                  std::uint64_t>;
  constexpr auto kSign = Bits{1} << (sizeof(Bits) * 8 - 1);
  const auto bits = std::bit_cast<Bits>(value);
  return (bits & kSign) != 0 ? ~bits : bits ^ kSign;
}

template <class Traits, class Score>
class PackedZSetIndex {
 public:
  using Key = typename Traits::Key;
  using Order = PackedBPlusTree<Traits, Score>;
  using Entry = typename Order::Entry;

  explicit PackedZSetIndex(
      double merge_exponent = kDefaultPackedZSetMergeExponent)
      : order_(merge_exponent), merge_exponent_(merge_exponent) {
    if (!valid_packed_zset_merge_exponent(merge_exponent_)) {
      throw std::invalid_argument(
          "packed zset merge exponent must be between 0 and 1");
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return scores_.size(); }
  [[nodiscard]] bool empty() const noexcept { return scores_.empty(); }
  [[nodiscard]] std::size_t unsorted_size() const noexcept {
    return order_.dirty_size();
  }
  [[nodiscard]] std::size_t entry_count() const noexcept {
    return order_.entry_count();
  }
  [[nodiscard]] std::size_t sorted_entry_count() const noexcept {
    return order_.sorted_entry_count();
  }
  [[nodiscard]] std::size_t merge_threshold() const noexcept {
    return order_.merge_threshold();
  }
  [[nodiscard]] std::size_t leaf_capacity() const noexcept {
    return order_.leaf_capacity();
  }
  [[nodiscard]] std::size_t leaf_count() const noexcept {
    return order_.leaf_count();
  }
  [[nodiscard]] std::size_t branch_count() const noexcept {
    return order_.branch_count();
  }
  [[nodiscard]] std::size_t tree_height() const noexcept {
    return order_.tree_height();
  }
  [[nodiscard]] double merge_exponent() const noexcept {
    return merge_exponent_;
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return scores_.allocated_bytes() + order_.allocated_bytes();
  }

  [[nodiscard]] const Score* find(const Key& key) const noexcept {
    return scores_.find(key);
  }

  PackedZSetAddResult add(std::span<const PackedZSetAddItem> items,
                          PackedZSetAddOptions options) {
    if (items.size() == 1) {
      const auto parsed = parse_item(items.front(), options.increment);
      if (!parsed.key) return {.invalid_member = true};
      if (!options.increment && !parsed.score) return {.invalid_score = true};
      if (options.increment && std::isnan(items.front().score)) {
        return {.invalid_score = true};
      }
      return apply_one(Parsed{*parsed.key, items.front().score, parsed.score},
                       options);
    }

    std::vector<Parsed> parsed;
    reserve_memory_vector(parsed, items.size());
    for (const auto& item : items) {
      const auto value = parse_item(item, options.increment);
      if (!value.key) return {.invalid_member = true};
      if (!options.increment && !value.score) return {.invalid_score = true};
      if (options.increment && std::isnan(item.score)) {
        return {.invalid_score = true};
      }
      parsed.push_back(Parsed{*value.key, item.score, value.score});
    }
    // Keep batch map-allocation preflight, but count distinct actual misses.
    // Updates, XX misses and repeated new members need no extra map slots.
    if (!options.xx) {
      std::vector<Key> missing;
      for (const auto& item : parsed) {
        if (scores_.find(item.key) == nullptr) {
          reserve_memory_vector_for_push(missing, 16);
          missing.push_back(item.key);
        }
      }
      std::sort(missing.begin(), missing.end(), Traits::less);
      const auto last = std::unique(missing.begin(), missing.end());
      scores_.reserve_additional(static_cast<std::size_t>(last - missing.begin()));
    }

    PackedZSetAddResult result;
    for (const auto& item : parsed) {
      const auto one = apply_one(item, options);
      result.added += one.added;
      result.changed += one.changed;
      if (options.increment) result.increment_score = one.increment_score;
      if (one.invalid_score) {
        result.invalid_score = true;
        return result;
      }
    }
    return result;
  }

  PackedZSetRemoveResult remove(std::span<const std::string_view> members) {
    if (members.size() == 1) {
      const auto key = Traits::parse(members.front());
      if (!key) return {.invalid_member = true};
      return {.removed = remove_one(*key)};
    }
    std::vector<Key> parsed;
    reserve_memory_vector(parsed, members.size());
    for (const auto member : members) {
      const auto key = Traits::parse(member);
      if (!key) return {.invalid_member = true};
      parsed.push_back(*key);
    }
    PackedZSetRemoveResult result;
    for (const auto& key : parsed) result.removed += remove_one(key);
    return result;
  }

  [[nodiscard]] std::vector<Entry> range_by_score(
      double min, bool min_exclusive, double max, bool max_exclusive,
      bool reverse = false, std::size_t offset = 0,
      std::optional<std::size_t> limit = std::nullopt) const {
    return order_.range_by_score(min, min_exclusive, max, max_exclusive,
                                 reverse, offset, limit);
  }

  [[nodiscard]] std::vector<Entry> range_by_rank(long long start,
                                                  long long stop,
                                                  bool reverse) const {
    return order_.range_by_rank(start, stop, reverse);
  }

  [[nodiscard]] std::optional<std::size_t> rank(const Key& key,
                                                 bool reverse) const {
    const auto* score = scores_.find(key);
    return score == nullptr ? std::nullopt
                            : order_.rank(Entry{*score, key}, reverse);
  }

  [[nodiscard]] std::size_t remove_by_score(double min, bool min_exclusive,
                                             double max, bool max_exclusive) {
    return remove_entries(
        range_by_score(min, min_exclusive, max, max_exclusive));
  }

  [[nodiscard]] std::size_t remove_by_rank(long long start, long long stop) {
    return remove_entries(range_by_rank(start, stop, false));
  }

  [[nodiscard]] std::vector<Entry> pop(std::size_t count, bool maximum) {
    if (count == 0) return {};
    auto selected = range_by_rank(0, static_cast<long long>(count) - 1,
                                  maximum);
    (void)remove_entries(selected);
    return selected;
  }

  void force_merge() { order_.force_merge(); }

  [[nodiscard]] bool check_invariants() const {
    if (order_.size() != scores_.size() || !order_.check_invariants()) {
      return false;
    }
    const auto ordered = order_.range_by_rank(0, -1, false);
    if (ordered.size() != scores_.size()) return false;
    for (const auto& entry : ordered) {
      const auto* score = scores_.find(entry.key);
      if (score == nullptr || *score != entry.score) return false;
    }
    return true;
  }

 private:
  struct ParsedItem {
    std::optional<Key> key;
    std::optional<Score> score;
  };

  struct Parsed {
    Key key{};
    double input{0.0};
    std::optional<Score> score;
  };

  [[nodiscard]] static ParsedItem parse_item(const PackedZSetAddItem& item,
                                              bool increment) noexcept {
    return {.key = Traits::parse(item.member),
            .score = increment ? std::optional<Score>{}
                               : narrow_packed_score<Score>(item.score)};
  }

  PackedZSetAddResult apply_one(const Parsed& item,
                                PackedZSetAddOptions options) {
    auto slot = scores_.find_insert_slot(item.key);
    auto* old_ptr = slot.value;
    const bool existed = old_ptr != nullptr;
    const auto old_score = existed ? *old_ptr : Score{};
    Score candidate{};
    if (options.increment) {
      const double total = (existed ? static_cast<double>(old_score) : 0.0) +
                           item.input;
      const auto narrowed = narrow_packed_score<Score>(total);
      if (!narrowed) return {.invalid_score = true};
      candidate = *narrowed;
    } else {
      candidate = *item.score;
    }

    bool apply = true;
    if (options.nx && existed) apply = false;
    if (options.xx && !existed) apply = false;
    if (options.gt && existed && !(candidate > old_score)) apply = false;
    if (options.lt && existed && !(candidate < old_score)) apply = false;
    if (!apply) return {};

    PackedZSetAddResult result;
    if (!existed || candidate != old_score) {
      if (existed) {
        order_.replace(Entry{old_score, item.key}, Entry{candidate, item.key});
        // Tree changes cannot move Swiss slots. Publish only after success.
        *old_ptr = candidate;
      } else {
        scores_.reserve_insert_slot(slot);
        order_.insert(Entry{candidate, item.key});
        static_assert(std::is_nothrow_constructible_v<std::pair<Key, Score>,
                                                     const Key&, Score&&>);
        scores_.commit_insert_slot(slot, item.key, candidate);
      }
      result.added = existed ? 0 : 1;
      result.changed = 1;
    }
    if (options.increment) {
      result.increment_score = static_cast<double>(candidate);
    }
    return result;
  }

  [[nodiscard]] std::size_t remove_one(const Key& key) {
    const auto slot = scores_.find_insert_slot(key);
    if (slot.value == nullptr) return 0;
    order_.erase(Entry{*slot.value, key});
    // As on updates, tree mutation leaves the Swiss slot in place.
    (void)scores_.erase_slot(slot);
    return 1;
  }

  [[nodiscard]] std::size_t remove_entries(
      const std::vector<Entry>& selected) {
    std::size_t removed = 0;
    for (const auto& entry : selected) removed += remove_one(entry.key);
    return removed;
  }

  SwissTable<Key, Score, typename Traits::Hash, std::equal_to<Key>> scores_;
  Order order_;
  double merge_exponent_{kDefaultPackedZSetMergeExponent};
};

using PackedInt32Float32 =
    PackedZSetIndex<PackedIntegerTraits<std::int32_t>, float>;
using PackedInt32Float64 =
    PackedZSetIndex<PackedIntegerTraits<std::int32_t>, double>;
using PackedInt64Float32 =
    PackedZSetIndex<PackedIntegerTraits<std::int64_t>, float>;
using PackedInt64Float64 =
    PackedZSetIndex<PackedIntegerTraits<std::int64_t>, double>;
using PackedUuidFloat32 = PackedZSetIndex<PackedUuidTraits, float>;
using PackedUuidFloat64 = PackedZSetIndex<PackedUuidTraits, double>;

}  // namespace detail

// Runtime wrapper around the six compile-time layouts. Keyspace stores this as
// one pointer; the hot member/score paths still instantiate fixed-width code and
// contain no per-entry discriminant or heap object.
class PackedZSet {
 public:
  explicit PackedZSet(
      PackedZSetKind kind,
      double merge_exponent = kDefaultPackedZSetMergeExponent)
      : kind_(kind) {
    if (!valid_packed_zset_merge_exponent(merge_exponent)) {
      throw std::invalid_argument(
          "packed zset merge exponent must be between 0 and 1");
    }
    switch (kind) {
      case PackedZSetKind::Int32Float32:
        impl_.emplace<detail::PackedInt32Float32>(merge_exponent);
        break;
      case PackedZSetKind::Int32Float64:
        impl_.emplace<detail::PackedInt32Float64>(merge_exponent);
        break;
      case PackedZSetKind::Int64Float32:
        impl_.emplace<detail::PackedInt64Float32>(merge_exponent);
        break;
      case PackedZSetKind::Int64Float64:
        impl_.emplace<detail::PackedInt64Float64>(merge_exponent);
        break;
      case PackedZSetKind::UuidFloat32:
        impl_.emplace<detail::PackedUuidFloat32>(merge_exponent);
        break;
      case PackedZSetKind::UuidFloat64:
        impl_.emplace<detail::PackedUuidFloat64>(merge_exponent);
        break;
    }
  }

  PackedZSet(const PackedZSet&) = delete;
  PackedZSet& operator=(const PackedZSet&) = delete;
  PackedZSet(PackedZSet&&) noexcept = default;
  PackedZSet& operator=(PackedZSet&&) noexcept = default;

  [[nodiscard]] PackedZSetKind kind() const noexcept { return kind_; }
  [[nodiscard]] std::size_t size() const noexcept {
    return std::visit([](const auto& value) { return value.size(); }, impl_);
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] std::size_t unsorted_size() const noexcept {
    return std::visit([](const auto& value) { return value.unsorted_size(); },
                      impl_);
  }
  [[nodiscard]] std::size_t entry_count() const noexcept {
    return std::visit([](const auto& value) { return value.entry_count(); },
                      impl_);
  }
  [[nodiscard]] std::size_t sorted_entry_count() const noexcept {
    return std::visit(
        [](const auto& value) { return value.sorted_entry_count(); }, impl_);
  }
  [[nodiscard]] std::size_t merge_threshold() const noexcept {
    return std::visit([](const auto& value) { return value.merge_threshold(); },
                      impl_);
  }
  [[nodiscard]] std::size_t leaf_capacity() const noexcept {
    return std::visit([](const auto& value) { return value.leaf_capacity(); },
                      impl_);
  }
  [[nodiscard]] std::size_t leaf_count() const noexcept {
    return std::visit([](const auto& value) { return value.leaf_count(); },
                      impl_);
  }
  [[nodiscard]] std::size_t branch_count() const noexcept {
    return std::visit([](const auto& value) { return value.branch_count(); },
                      impl_);
  }
  [[nodiscard]] std::size_t tree_height() const noexcept {
    return std::visit([](const auto& value) { return value.tree_height(); },
                      impl_);
  }
  [[nodiscard]] double merge_exponent() const noexcept {
    return std::visit([](const auto& value) { return value.merge_exponent(); },
                      impl_);
  }
  [[nodiscard]] std::size_t allocated_bytes() const noexcept {
    return sizeof(PackedZSet) +
           std::visit([](const auto& value) { return value.allocated_bytes(); },
                      impl_);
  }

  [[nodiscard]] static bool valid_member(PackedZSetKind kind,
                                         std::string_view member) noexcept {
    switch (kind) {
      case PackedZSetKind::Int32Float32:
      case PackedZSetKind::Int32Float64:
        return detail::PackedIntegerTraits<std::int32_t>::parse(member)
            .has_value();
      case PackedZSetKind::Int64Float32:
      case PackedZSetKind::Int64Float64:
        return detail::PackedIntegerTraits<std::int64_t>::parse(member)
            .has_value();
      case PackedZSetKind::UuidFloat32:
      case PackedZSetKind::UuidFloat64:
        return detail::PackedUuidTraits::parse(member).has_value();
    }
    return false;
  }

  [[nodiscard]] PackedZSetAddResult add(
      std::span<const PackedZSetAddItem> items,
      PackedZSetAddOptions options = {}) {
    return std::visit([&](auto& value) { return value.add(items, options); },
                      impl_);
  }

  [[nodiscard]] PackedZSetRemoveResult remove(
      std::span<const std::string_view> members) {
    return std::visit([&](auto& value) { return value.remove(members); }, impl_);
  }

  [[nodiscard]] PackedZSetLookup score(std::string_view member) const {
    return std::visit(
        [&](const auto& value) -> PackedZSetLookup {
          using Index = std::decay_t<decltype(value)>;
          using Key = typename Index::Key;
          (void)sizeof(Key);
          const auto parsed = parse_for_index(value, member);
          if (!parsed) return {.valid_member = false};
          const auto* found = value.find(*parsed);
          return {.valid_member = true,
                  .score = found == nullptr
                               ? std::optional<double>{}
                               : std::optional<double>{
                                     static_cast<double>(*found)}};
        },
        impl_);
  }

  [[nodiscard]] PackedZSetRankLookup rank(std::string_view member,
                                          bool reverse = false) const {
    return std::visit(
        [&](const auto& value) -> PackedZSetRankLookup {
          const auto parsed = parse_for_index(value, member);
          if (!parsed) return {.valid_member = false};
          return {.valid_member = true, .rank = value.rank(*parsed, reverse)};
        },
        impl_);
  }

  [[nodiscard]] std::vector<PackedZSetEntry> range_by_score(
      double min, bool min_exclusive, double max, bool max_exclusive,
      bool reverse = false, std::size_t offset = 0,
      std::optional<std::size_t> limit = std::nullopt) const {
    return std::visit(
        [&](const auto& value) {
          return format_entries(value,
                                value.range_by_score(min, min_exclusive, max,
                                                     max_exclusive, reverse,
                                                     offset, limit));
        },
        impl_);
  }

  [[nodiscard]] std::vector<PackedZSetEntry> range_by_rank(
      long long start, long long stop, bool reverse = false) const {
    return std::visit(
        [&](const auto& value) {
          return format_entries(value,
                                value.range_by_rank(start, stop, reverse));
        },
        impl_);
  }

  [[nodiscard]] std::size_t count(double min, bool min_exclusive, double max,
                                  bool max_exclusive) const {
    return std::visit(
        [&](const auto& value) {
          return value
              .range_by_score(min, min_exclusive, max, max_exclusive)
              .size();
        },
        impl_);
  }

  [[nodiscard]] std::size_t remove_by_score(double min, bool min_exclusive,
                                             double max, bool max_exclusive) {
    return std::visit(
        [&](auto& value) {
          return value.remove_by_score(min, min_exclusive, max, max_exclusive);
        },
        impl_);
  }

  [[nodiscard]] std::size_t remove_by_rank(long long start, long long stop) {
    return std::visit(
        [&](auto& value) { return value.remove_by_rank(start, stop); }, impl_);
  }

  [[nodiscard]] std::vector<PackedZSetEntry> pop(std::size_t count,
                                                 bool maximum) {
    return std::visit(
        [&](auto& value) {
          auto entries = value.pop(count, maximum);
          return format_entries(value, entries);
        },
        impl_);
  }

  [[nodiscard]] PackedZSetScanResult scan(std::uint64_t cursor,
                                          std::size_t count) const {
    auto all = range_by_score(-std::numeric_limits<double>::infinity(), false,
                              std::numeric_limits<double>::infinity(), false);
    PackedZSetScanResult result;
    if (cursor >= all.size()) return result;
    const auto take = std::min(count == 0 ? std::size_t{10} : count,
                               all.size() - static_cast<std::size_t>(cursor));
    result.entries.reserve(take);
    auto begin = all.begin() + static_cast<std::ptrdiff_t>(cursor);
    result.entries.insert(result.entries.end(),
                          std::make_move_iterator(begin),
                          std::make_move_iterator(begin + take));
    const auto next = static_cast<std::size_t>(cursor) + take;
    result.next = next == all.size() ? 0 : static_cast<std::uint64_t>(next);
    return result;
  }

  void force_merge() {
    std::visit([](auto& value) { value.force_merge(); }, impl_);
  }

  [[nodiscard]] bool check_invariants() const {
    return std::visit([](const auto& value) { return value.check_invariants(); },
                      impl_);
  }

 private:
  using Implementation =
      std::variant<detail::PackedInt32Float32, detail::PackedInt32Float64,
                   detail::PackedInt64Float32, detail::PackedInt64Float64,
                   detail::PackedUuidFloat32, detail::PackedUuidFloat64>;

  template <class Index>
  [[nodiscard]] static std::optional<typename Index::Key> parse_for_index(
      const Index&, std::string_view member) {
    using Key = typename Index::Key;
    if constexpr (std::is_same_v<Key, std::int32_t>) {
      return detail::PackedIntegerTraits<std::int32_t>::parse(member);
    } else if constexpr (std::is_same_v<Key, std::int64_t>) {
      return detail::PackedIntegerTraits<std::int64_t>::parse(member);
    } else {
      return detail::PackedUuidTraits::parse(member);
    }
  }

  template <class Index>
  [[nodiscard]] static std::vector<PackedZSetEntry> format_entries(
      const Index&, const std::vector<typename Index::Entry>& entries) {
    std::vector<PackedZSetEntry> result;
    reserve_memory_vector(result, entries.size());
    for (const auto& entry : entries) {
      using Key = typename Index::Key;
      if constexpr (std::is_same_v<Key, std::int32_t>) {
        result.push_back({
            detail::PackedIntegerTraits<std::int32_t>::format(entry.key),
            static_cast<double>(entry.score)});
      } else if constexpr (std::is_same_v<Key, std::int64_t>) {
        result.push_back({
            detail::PackedIntegerTraits<std::int64_t>::format(entry.key),
            static_cast<double>(entry.score)});
      } else {
        result.push_back({detail::PackedUuidTraits::format(entry.key),
                          static_cast<double>(entry.score)});
      }
    }
    return result;
  }

  PackedZSetKind kind_;
  Implementation impl_;
};

}  // namespace goblin::core
