#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
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

namespace goblin::core {

enum class ListImplementation : std::uint8_t {
  Pma,
};

[[nodiscard]] constexpr std::string_view list_implementation_name(
    ListImplementation implementation) noexcept {
  switch (implementation) {
    case ListImplementation::Pma:
      return "pma";
  }
  return "unknown";
}

struct ListOptions {
  std::size_t chunk_bytes{ListValueArena::kDefaultChunkBytes};
  std::size_t listpack_max_entries{32};
  double max_density{AdaptivePma::kDefaultMaxDensity};
  double resize_growth{AdaptivePma::kDefaultResizeGrowth};
};

struct ListMemoryStats {
  std::size_t element_count{0};
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
  explicit List(ListOptions options = {}) : options_(options) { init_empty(); }

  List(const List&) = delete;
  List& operator=(const List&) = delete;
  List(List&&) noexcept = default;
  List& operator=(List&&) noexcept = default;

  [[nodiscard]] std::size_t size() const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->size();
    }
    return full().order.size();
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] bool is_small() const noexcept {
    return std::holds_alternative<ListListpack>(rep_);
  }
  [[nodiscard]] const ListOptions& options() const noexcept { return options_; }

  void clear() { init_empty(); }

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
        values.size() <= options_.listpack_max_entries -
                             std::min(size(), options_.listpack_max_entries)) {
      for (std::size_t index = 0; index < values.size(); ++index) {
        auto* lp = small_ptr();
        if (lp != nullptr &&
            lp->insert(0, values[index], options_.listpack_max_entries)) {
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
        values.size() <= options_.listpack_max_entries -
                             std::min(size(), options_.listpack_max_entries)) {
      for (std::size_t index = 0; index < values.size(); ++index) {
        auto* lp = small_ptr();
        if (lp != nullptr && lp->insert(lp->size(), values[index],
                                       options_.listpack_max_entries)) {
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
      if (lp->insert(index, value, options_.listpack_max_entries)) {
        return;
      }
      ensure_full();
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

  [[nodiscard]] std::string_view at(std::size_t index) const noexcept {
    assert(index < size());
    if (const auto* lp = small_ptr()) {
      return lp->at(index);
    }
    const auto& state = full();
    return state.values.view(state.order.at(index));
  }

  void set(std::size_t index, std::string_view value) {
    assert(index < size());
    if (auto* lp = small_ptr()) {
      if (lp->set(index, value)) {
        return;
      }
      ensure_full();
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
      return lp->erase(index);
    }
    auto& state = full();
    const auto ref = state.order.at(index);
    std::string removed(state.values.view(ref));
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
      lp->for_each(std::forward<Fn>(fn));
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
      const auto stop = count > lp->size() - first ? lp->size() : first + count;
      for (auto index = first; index < stop; ++index) {
        fn(lp->at(index));
      }
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
      for (auto index = first; index < lp->size(); ++index) {
        if (lp->at(index) == value) {
          return index;
        }
      }
      return std::nullopt;
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
      while (end != 0) {
        --end;
        if (lp->at(end) == value) {
          return end;
        }
      }
      return std::nullopt;
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
    compact_values();
    full().order.compact();
    maybe_demote();
  }

  [[nodiscard]] ListMemoryStats memory_stats() const noexcept {
    ListMemoryStats stats;
    stats.element_count = size();
    if (const auto* lp = small_ptr()) {
      stats.value_live_bytes = lp->allocated_bytes();
      stats.value_allocated_bytes = lp->allocated_bytes();
      stats.total_allocated_bytes = lp->allocated_bytes();
      return stats;
    }
    const auto& state = full();
    stats.value_live_bytes = state.values.live_bytes();
    stats.value_dead_bytes = state.values.dead_bytes();
    stats.value_allocated_bytes = state.values.allocated_bytes();
    stats.order_capacity = state.order.capacity();
    stats.order_front_slack = state.order.front_slack();
    stats.order_back_slack = state.order.back_slack();
    stats.order_allocated_bytes = state.order.allocated_bytes();
    stats.total_allocated_bytes =
        stats.value_allocated_bytes + stats.order_allocated_bytes;
    return stats;
  }

  [[nodiscard]] bool check_invariants() const noexcept {
    if (const auto* lp = small_ptr()) {
      return lp->size() <= options_.listpack_max_entries;
    }
    return full().order.check_invariants();
  }

 private:
  struct FullState {
    explicit FullState(const ListOptions& options)
        : values(options.chunk_bytes, options.resize_growth),
          order(options.max_density, options.resize_growth) {}

    ListValueArena values;
    AdaptivePma order;
  };

  [[nodiscard]] ListListpack* small_ptr() noexcept {
    return std::get_if<ListListpack>(&rep_);
  }
  [[nodiscard]] const ListListpack* small_ptr() const noexcept {
    return std::get_if<ListListpack>(&rep_);
  }
  [[nodiscard]] FullState& full() noexcept { return std::get<FullState>(rep_); }
  [[nodiscard]] const FullState& full() const noexcept {
    return std::get<FullState>(rep_);
  }

  void init_empty() {
    if (options_.listpack_max_entries == 0) {
      rep_.template emplace<FullState>(options_);
    } else {
      rep_.template emplace<ListListpack>();
    }
  }

  void ensure_full() {
    auto* lp = small_ptr();
    if (lp == nullptr) {
      return;
    }
    FullState state(options_);
    std::vector<ListValueRef> refs;
    refs.reserve(lp->size());
    lp->for_each([&state, &refs](std::string_view value) {
      refs.push_back(state.values.append(value));
    });
    state.order.insert_many(0, refs);
    rep_.template emplace<FullState>(std::move(state));
  }

  void insert_full_batch(std::span<const std::string_view> values,
                         bool front) {
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
    if (is_small() || options_.listpack_max_entries == 0 ||
        size() > options_.listpack_max_entries) {
      return;
    }
    auto& state = full();
    ListListpack packed;
    bool fits = true;
    state.order.for_each([&](ListValueRef ref) {
      if (fits && !packed.insert(packed.size(), state.values.view(ref),
                                 options_.listpack_max_entries)) {
        fits = false;
      }
    });
    if (fits) {
      rep_.template emplace<ListListpack>(std::move(packed));
    }
  }

  void maybe_compact_values() {
    if (!is_small() && full().values.should_compact()) {
      compact_values();
    }
  }

  void compact_values() {
    auto& state = full();
    if (state.values.dead_bytes() == 0) {
      return;
    }
    ListValueArena fresh(options_.chunk_bytes, options_.resize_growth);
    std::vector<ListValueRef> refs;
    refs.reserve(state.order.size());
    state.order.for_each([&](ListValueRef ref) {
      refs.push_back(fresh.append(state.values.view(ref)));
    });
    for (std::size_t rank = 0; rank < refs.size(); ++rank) {
      state.order.set(rank, refs[rank]);
    }
    state.values = std::move(fresh);
  }

  ListOptions options_;
  std::variant<ListListpack, FullState> rep_;
};

}  // namespace goblin::core
