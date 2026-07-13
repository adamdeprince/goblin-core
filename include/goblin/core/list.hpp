#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "goblin/core/adaptive_pma.hpp"
#include "goblin/core/list_listpack.hpp"
#include "goblin/core/list_value_arena.hpp"
#include "goblin/core/segmented_list.hpp"

namespace goblin::core {

enum class ListImplementation : std::uint8_t {
  Pma,
  Segmented,
};

[[nodiscard]] constexpr std::string_view list_implementation_name(
    ListImplementation implementation) noexcept {
  switch (implementation) {
    case ListImplementation::Pma:
      return "pma";
    case ListImplementation::Segmented:
      return "segmented";
  }
  return "unknown";
}

struct ListOptions {
  ListImplementation implementation{ListImplementation::Pma};
  std::size_t chunk_bytes{ListValueArena::kDefaultChunkBytes};
  std::size_t listpack_max_entries{32};
  double max_density{AdaptivePma::kDefaultMaxDensity};
  double resize_growth{AdaptivePma::kDefaultResizeGrowth};
  StringEncodingOptions string_encoding{};
  StringCompressionMode string_compression{StringCompressionMode::AllowLz4};
};

struct ListMemoryStats {
  ListImplementation implementation{ListImplementation::Pma};
  std::size_t element_count{0};
  std::size_t object_allocated_bytes{0};
  std::size_t value_live_bytes{0};
  std::size_t value_dead_bytes{0};
  std::size_t value_allocated_bytes{0};
  std::size_t order_capacity{0};
  std::size_t order_front_slack{0};
  std::size_t order_back_slack{0};
  std::size_t order_allocated_bytes{0};
  std::size_t total_allocated_bytes{0};
};

class List {
 public:
  explicit List(ListOptions options = {})
      : options_(std::make_shared<const ListOptions>(std::move(options))) {
    init_empty();
  }
  explicit List(std::shared_ptr<const ListOptions> options)
      : options_(options ? std::move(options)
                         : std::make_shared<const ListOptions>()) {
    init_empty();
  }

  List(const List&) = delete;
  List& operator=(const List&) = delete;
  List(List&&) noexcept = default;
  List& operator=(List&&) noexcept = default;

  [[nodiscard]] std::size_t size() const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->size();
    }
    if (const auto* segmented = segmented_ptr()) {
      return (*segmented)->size();
    }
    return full().order.size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] bool is_small() const noexcept {
    return std::holds_alternative<ListListpack>(rep_);
  }
  [[nodiscard]] const ListOptions& options() const noexcept {
    return *options_;
  }
  [[nodiscard]] ListImplementation implementation() const noexcept {
    return options_->implementation;
  }

  void clear() { init_empty(); }

  // Replace the list from values that are already in final logical order.
  // Snapshot/RDB restore uses this path so construction is one representation
  // build, rather than replaying one mutation per serialized element.
  void assign(std::span<const std::string_view> values) {
    List replacement(options_);
    (void)replacement.push_back(values);
    rep_ = std::move(replacement.rep_);
  }

  // Snapshot accelerator counterpart to assign(): all values are known to use
  // the raw representation, so their canonical bytes can be copied directly
  // into final listpacks or the PMA arena without re-running classification.
  void assign_raw(std::span<const std::string_view> values) {
    List replacement(options_);
    if (values.empty()) {
      rep_ = std::move(replacement.rep_);
      return;
    }
    if (values.size() <= options_->listpack_max_entries) {
      auto* packed = replacement.small_ptr();
      if (packed != nullptr &&
          packed->assign_raw(values, options_->listpack_max_entries)) {
        rep_ = std::move(replacement.rep_);
        return;
      }
    }

    replacement.ensure_full();
    if (auto* segmented = replacement.segmented_ptr()) {
      (*segmented)->assign_raw(values);
    } else {
      auto& state = replacement.full();
      const bool encoding_enabled =
          options_->string_encoding.encoding_enabled();
      const auto refs = state.values.assign_raw(values, encoding_enabled);
      state.order.insert_many(0, refs, AdaptivePma::EndpointBias::Back);
    }
    rep_ = std::move(replacement.rep_);
  }

  [[nodiscard]] std::size_t push_front(std::string_view value) {
    insert(0, value);
    return size();
  }
  [[nodiscard]] std::size_t push_back(std::string_view value) {
    insert(size(), value);
    return size();
  }

  [[nodiscard]] std::size_t push_front(
      std::span<const std::string_view> values) {
    if (values.empty()) {
      return size();
    }
    if (small_ptr() != nullptr &&
        values.size() <= options_->listpack_max_entries -
                             std::min(size(), options_->listpack_max_entries)) {
      for (std::size_t index = 0; index < values.size(); ++index) {
        auto* lp = small_ptr();
        if (lp != nullptr &&
            lp->insert(0, values[index], options_->listpack_max_entries,
                       options_->string_encoding,
                       options_->string_compression)) {
          continue;
        }
        ensure_full();
        insert_full_batch(values.subspan(index), true);
        return size();
      }
      return size();
    }
    ensure_full();
    insert_full_batch(values, true);
    return size();
  }

  [[nodiscard]] std::size_t push_back(
      std::span<const std::string_view> values) {
    if (values.empty()) {
      return size();
    }
    if (small_ptr() != nullptr &&
        values.size() <= options_->listpack_max_entries -
                             std::min(size(), options_->listpack_max_entries)) {
      for (std::size_t index = 0; index < values.size(); ++index) {
        auto* lp = small_ptr();
        if (lp != nullptr && lp->insert(lp->size(), values[index],
                                       options_->listpack_max_entries,
                                       options_->string_encoding,
                                       options_->string_compression)) {
          continue;
        }
        ensure_full();
        insert_full_batch(values.subspan(index), false);
        return size();
      }
      return size();
    }
    ensure_full();
    insert_full_batch(values, false);
    return size();
  }

  void insert(std::size_t index, std::string_view value) {
    assert(index <= size());
    if (auto* lp = small_ptr()) {
      if (lp->insert(index, value, options_->listpack_max_entries,
                     options_->string_encoding,
                     options_->string_compression)) {
        return;
      }
      ensure_full();
    }
    if (auto* segmented = segmented_ptr()) {
      (*segmented)->insert(index, value);
      return;
    }
    auto& state = full();
    const auto ref = state.values.append(value);
    try {
      state.order.insert(index, ref);
    } catch (...) {
      state.values.orphan(ref);
      throw;
    }
  }

  [[nodiscard]] std::optional<std::string> pop_front() {
    return empty() ? std::nullopt : std::optional<std::string>(erase(0));
  }
  [[nodiscard]] std::optional<std::string> pop_back() {
    return empty() ? std::nullopt
                   : std::optional<std::string>(erase(size() - 1));
  }

  [[nodiscard]] EncodedStringView at(std::size_t index) const noexcept {
    assert(index < size());
    if (const auto* lp = small_ptr()) {
      return lp->at(index, options_->string_encoding);
    }
    if (const auto* segmented = segmented_ptr()) {
      return (*segmented)->at(index);
    }
    const auto& state = full();
    return state.values.view(state.order.at(index));
  }

  void set(std::size_t index, std::string_view value) {
    assert(index < size());
    if (auto* lp = small_ptr()) {
      if (lp->set(index, value, options_->string_encoding,
                  options_->string_compression)) {
        return;
      }
      ensure_full();
    }
    if (auto* segmented = segmented_ptr()) {
      (*segmented)->set(index, value);
      return;
    }
    auto& state = full();
    const auto prior = state.order.at(index);
    const auto replacement = state.values.append(value);
    state.order.set(index, replacement);
    state.values.orphan(prior);
    maybe_compact_values();
  }

  [[nodiscard]] std::string erase(std::size_t index) {
    assert(index < size());
    if (auto* lp = small_ptr()) {
      return lp->erase(index, options_->string_encoding);
    }
    if (auto* segmented = segmented_ptr()) {
      auto removed = (*segmented)->erase(index);
      maybe_demote();
      return removed;
    }
    auto& state = full();
    const auto ref = state.order.at(index);
    std::string removed = state.values.view(ref).to_string();
    const auto erased = state.order.erase(index);
    assert(erased == ref);
    state.values.orphan(erased);
    maybe_compact_values();
    maybe_demote();
    return removed;
  }

  // Redis LREM semantics: positive count scans from the head, negative from the
  // tail, and zero removes every matching element.
  [[nodiscard]] std::size_t remove(std::string_view value, long long count) {
    std::size_t removed = 0;
    if (count >= 0) {
      const auto limit = count == 0
                             ? std::numeric_limits<std::size_t>::max()
                             : static_cast<std::size_t>(count);
      std::size_t first = 0;
      while (removed < limit) {
        const auto match = find_first(value, first);
        if (!match) {
          break;
        }
        (void)erase(*match);
        first = *match;
        ++removed;
      }
    } else {
      const auto magnitude = count == std::numeric_limits<long long>::min()
                                 ? std::numeric_limits<unsigned long long>::max()
                                 : static_cast<unsigned long long>(-count);
      const auto limit = magnitude > std::numeric_limits<std::size_t>::max()
                             ? std::numeric_limits<std::size_t>::max()
                             : static_cast<std::size_t>(magnitude);
      std::size_t end = size();
      while (removed < limit) {
        const auto match = find_last(value, end);
        if (!match) {
          break;
        }
        (void)erase(*match);
        end = *match;
        ++removed;
      }
    }
    return removed;
  }

  // Keep [first, first+count), discarding everything else.
  void trim(std::size_t first, std::size_t count) {
    const auto keep_end = count > size() - std::min(first, size())
                              ? size()
                              : first + count;
    while (size() > keep_end) {
      (void)erase(size() - 1);
    }
    while (first != 0 && !empty()) {
      (void)erase(0);
      --first;
    }
  }

  template <class Fn>
  void for_each(Fn&& fn) const {
    if (const auto* lp = small_ptr()) {
      lp->for_each(std::forward<Fn>(fn), options_->string_encoding);
      return;
    }
    if (const auto* segmented = segmented_ptr()) {
      (*segmented)->for_each(std::forward<Fn>(fn));
      return;
    }
    const auto& state = full();
    state.order.for_each(
        [&state, &fn](ListValueRef ref) { fn(state.values.view(ref)); });
  }

  template <class Fn>
  void for_range(std::size_t first, std::size_t count, Fn&& fn) const {
    if (first >= size()) {
      return;
    }
    if (const auto* lp = small_ptr()) {
      // Linear scan once; avoid at()-per-index which rewalks the blob.
      lp->for_range(first, count, std::forward<Fn>(fn),
                    options_->string_encoding);
      return;
    }
    if (const auto* segmented = segmented_ptr()) {
      (*segmented)->for_range(first, count, std::forward<Fn>(fn));
      return;
    }
    const auto& state = full();
    state.order.for_range(first, count, [&state, &fn](ListValueRef ref) {
      fn(state.values.view(ref));
    });
  }

  [[nodiscard]] std::optional<std::size_t> find_first(
      std::string_view value, std::size_t first = 0) const {
    if (const auto* lp = small_ptr()) {
      return lp->find_first(value, first, options_->string_encoding);
    }
    if (const auto* segmented = segmented_ptr()) {
      return (*segmented)->find_first(value, first);
    }
    const auto& state = full();
    return state.order.find_first(first, [&state, value](ListValueRef ref) {
      return state.values.view(ref) == value;
    });
  }

  [[nodiscard]] std::optional<std::size_t> find_last(
      std::string_view value, std::size_t end) const {
    end = std::min(end, size());
    if (const auto* lp = small_ptr()) {
      return lp->find_last(value, end, options_->string_encoding);
    }
    if (const auto* segmented = segmented_ptr()) {
      return (*segmented)->find_last(value, end);
    }
    const auto& state = full();
    return state.order.find_last(end, [&state, value](ListValueRef ref) {
      return state.values.view(ref) == value;
    });
  }

  void compact() {
    if (is_small()) {
      return;
    }
    if (auto* segmented = segmented_ptr()) {
      (*segmented)->compact();
      maybe_demote();
      return;
    }
    compact_values();
    full().values.shrink_to_fit();
    full().order.compact();
    maybe_demote();
  }

  [[nodiscard]] ListMemoryStats memory_stats() const noexcept {
    ListMemoryStats stats;
    stats.implementation = implementation();
    stats.element_count = size();
    stats.object_allocated_bytes = sizeof(List);
    if (const auto* lp = small_ptr()) {
      stats.value_live_bytes = lp->allocated_bytes();
      stats.value_allocated_bytes = lp->allocated_bytes();
      stats.total_allocated_bytes =
          stats.object_allocated_bytes + lp->allocated_bytes();
      return stats;
    }
    if (const auto* segmented = segmented_ptr()) {
      stats.object_allocated_bytes += sizeof(SegmentedList);
      stats.value_live_bytes = (*segmented)->value_allocated_bytes();
      stats.value_allocated_bytes = stats.value_live_bytes;
      stats.order_capacity = (*segmented)->leaf_count();
      stats.order_allocated_bytes = (*segmented)->index_allocated_bytes();
      stats.total_allocated_bytes = stats.object_allocated_bytes +
                                    stats.value_allocated_bytes +
                                    stats.order_allocated_bytes;
      return stats;
    }
    const auto& state = full();
    stats.object_allocated_bytes += sizeof(FullState);
    stats.value_live_bytes = state.values.live_bytes();
    stats.value_dead_bytes = state.values.dead_bytes();
    stats.value_allocated_bytes = state.values.allocated_bytes();
    stats.order_capacity = state.order.capacity();
    stats.order_front_slack = state.order.front_slack();
    stats.order_back_slack = state.order.back_slack();
    stats.order_allocated_bytes = state.order.allocated_bytes();
    stats.total_allocated_bytes = stats.object_allocated_bytes +
                                  stats.value_allocated_bytes +
                                  stats.order_allocated_bytes;
    return stats;
  }

  [[nodiscard]] bool check_invariants() const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->size() <= options_->listpack_max_entries;
    }
    if (const auto* segmented = segmented_ptr()) {
      return (*segmented)->check_invariants();
    }
    return full().order.check_invariants();
  }

 private:
  struct FullState {
    explicit FullState(const ListOptions& options)
        : values(options.chunk_bytes, options.resize_growth,
                 options.string_encoding, options.string_compression),
          order(options.max_density, options.resize_growth,
                options.chunk_bytes) {}

    ListValueArena values;
    AdaptivePma order;
  };
  using FullStatePtr = std::unique_ptr<FullState>;
  using SegmentedStatePtr = std::unique_ptr<SegmentedList>;

  [[nodiscard]] ListListpack* small_ptr() noexcept {
    return std::get_if<ListListpack>(&rep_);
  }
  [[nodiscard]] const ListListpack* small_ptr() const noexcept {
    return std::get_if<ListListpack>(&rep_);
  }
  [[nodiscard]] SegmentedStatePtr* segmented_ptr() noexcept {
    return std::get_if<SegmentedStatePtr>(&rep_);
  }
  [[nodiscard]] const SegmentedStatePtr* segmented_ptr() const noexcept {
    return std::get_if<SegmentedStatePtr>(&rep_);
  }
  [[nodiscard]] FullState& full() noexcept {
    return *std::get<FullStatePtr>(rep_);
  }
  [[nodiscard]] const FullState& full() const noexcept {
    return *std::get<FullStatePtr>(rep_);
  }

  void init_empty() {
    if (options_->listpack_max_entries == 0) {
      if (options_->implementation == ListImplementation::Segmented) {
        rep_.template emplace<SegmentedStatePtr>(
            std::make_unique<SegmentedList>(
                options_->string_encoding, options_->string_compression));
      } else {
        rep_.template emplace<FullStatePtr>(
            std::make_unique<FullState>(*options_));
      }
    } else {
      rep_.template emplace<ListListpack>();
    }
  }

  void ensure_full() {
    auto* lp = small_ptr();
    if (lp == nullptr) {
      return;
    }
    if (options_->implementation == ListImplementation::Segmented) {
      auto state = std::make_unique<SegmentedList>(
          options_->string_encoding, options_->string_compression);
      // Move the compact blob into the segmented representation without
      // decoding/re-encoding every entry (common case: one leaf).
      state->adopt_listpack(std::move(*lp));
      rep_.template emplace<SegmentedStatePtr>(std::move(state));
      return;
    }

    auto state = std::make_unique<FullState>(*options_);
    std::vector<ListValueRef> refs;
    refs.reserve(lp->size());
    lp->for_each(
        [&state, &refs](EncodedStringView value) {
          refs.push_back(state->values.append_encoded(value));
        },
        options_->string_encoding);
    state->order.insert_many(0, refs);
    rep_.template emplace<FullStatePtr>(std::move(state));
  }

  void insert_full_batch(std::span<const std::string_view> values,
                         bool front) {
    if (auto* segmented = segmented_ptr()) {
      if (front) {
        std::vector<std::string_view> reversed(values.rbegin(), values.rend());
        (*segmented)->insert_many(0, reversed);
      } else {
        (*segmented)->insert_many((*segmented)->size(), values);
      }
      return;
    }
    auto& state = full();
    std::vector<ListValueRef> refs;
    refs.reserve(values.size());
    try {
      if (front) {
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
          refs.push_back(state.values.append(*it));
        }
      } else {
        for (const auto value : values) {
          refs.push_back(state.values.append(value));
        }
      }
      state.order.insert_many(
          front ? 0 : state.order.size(), refs,
          front ? AdaptivePma::EndpointBias::Front
                : AdaptivePma::EndpointBias::Back);
    } catch (...) {
      for (const auto ref : refs) {
        state.values.orphan(ref);
      }
      throw;
    }
  }

  void maybe_demote() {
    if (is_small() || options_->listpack_max_entries == 0 ||
        size() > options_->listpack_max_entries) {
      return;
    }
    ListListpack packed;
    bool fits = true;
    if (const auto* segmented = segmented_ptr()) {
      (*segmented)->for_each([&](EncodedStringView value) {
        if (fits && !packed.insert_encoded(
                        packed.size(), value,
                        options_->listpack_max_entries)) {
          fits = false;
        }
      });
    } else {
      auto& state = full();
      state.order.for_each([&](ListValueRef ref) {
        if (fits && !packed.insert_encoded(
                        packed.size(), state.values.view(ref),
                        options_->listpack_max_entries)) {
          fits = false;
        }
      });
    }
    if (fits) {
      rep_.template emplace<ListListpack>(std::move(packed));
    }
  }

  void maybe_compact_values() {
    if (!is_small() && segmented_ptr() == nullptr &&
        full().values.should_compact()) {
      compact_values();
    }
  }

  void compact_values() {
    auto& state = full();
    if (state.values.dead_bytes() == 0) {
      return;
    }
    ListValueArena fresh(options_->chunk_bytes, options_->resize_growth,
                         options_->string_encoding,
                         options_->string_compression);
    std::vector<ListValueRef> refs;
    refs.reserve(state.order.size());
    state.order.for_each([&](ListValueRef ref) {
      refs.push_back(fresh.append_encoded(state.values.view(ref)));
    });
    fresh.shrink_to_fit();
    for (std::size_t rank = 0; rank < refs.size(); ++rank) {
      state.order.set(rank, refs[rank]);
    }
    state.values = std::move(fresh);
  }

  std::shared_ptr<const ListOptions> options_;
  std::variant<ListListpack, FullStatePtr, SegmentedStatePtr> rep_;
};

}  // namespace goblin::core
