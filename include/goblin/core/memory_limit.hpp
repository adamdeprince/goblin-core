#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <vector>

namespace goblin::core {

// Raised before a persistent store allocation would cross --maxmemory. Keep it
// distinct from allocator exhaustion so protocol dispatch can return the Redis
// OOM contract while genuine std::bad_alloc failures retain their old handling.
class MaxMemoryExceeded final : public std::bad_alloc {
 public:
  [[nodiscard]] const char* what() const noexcept override {
    return "maxmemory limit exceeded";
  }
};

// Allocation sites know the exact persistent-capacity delta they are about to
// add. The Store supplies the current aggregate footprint lazily, avoiding a
// second shadow accounting system that could drift from INFO memory reporting.
class MemoryCeiling {
 public:
  using UsageFn = std::size_t (*)(const void*) noexcept;

  explicit MemoryCeiling(std::size_t limit = 0) noexcept : limit_(limit) {}

  void bind(const void* context, UsageFn usage) noexcept {
    context_ = context;
    usage_ = usage;
  }

  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }
  [[nodiscard]] bool enabled() const noexcept { return limit_ != 0; }
  [[nodiscard]] std::size_t current_usage() const noexcept {
    return usage_ == nullptr ? std::size_t{0} : usage_(context_);
  }

  void ensure_growth(std::size_t bytes) const {
    if (bytes == 0 || limit_ == 0) {
      return;
    }
    const auto used = current_usage();
    if (used >= limit_ || bytes > limit_ - used) {
      throw MaxMemoryExceeded();
    }
  }

  void ensure_current_fits() const {
    if (limit_ != 0 && usage_ != nullptr && usage_(context_) > limit_) {
      throw MaxMemoryExceeded();
    }
  }

 private:
  std::size_t limit_{0};
  const void* context_{nullptr};
  UsageFn usage_{nullptr};
};

namespace detail {
struct ActiveMemoryBudget {
  const MemoryCeiling* ceiling{nullptr};
  std::size_t used{0};
  std::size_t reserved_growth{0};
  bool used_loaded{false};

  void load_usage() noexcept {
    if (!used_loaded) {
      used = ceiling == nullptr ? 0 : ceiling->current_usage();
      used_loaded = true;
    }
  }

  void ensure(std::size_t bytes) {
    if (bytes == 0 || ceiling == nullptr || !ceiling->enabled()) {
      return;
    }
    load_usage();
    const auto limit = ceiling->limit();
    if (used >= limit || reserved_growth > limit - used ||
        bytes > limit - used - reserved_growth) {
      throw MaxMemoryExceeded();
    }
    reserved_growth += bytes;
  }

  [[nodiscard]] bool allows(std::size_t bytes) noexcept {
    if (bytes == 0 || ceiling == nullptr || !ceiling->enabled()) {
      return true;
    }
    load_usage();
    const auto limit = ceiling->limit();
    return used < limit && reserved_growth <= limit - used &&
           bytes <= limit - used - reserved_growth;
  }
};

inline thread_local ActiveMemoryBudget* active_memory_budget = nullptr;
}  // namespace detail

class MemoryCeilingScope {
 public:
  explicit MemoryCeilingScope(const MemoryCeiling* ceiling) noexcept
      : prior_(detail::active_memory_budget) {
    if (prior_ != nullptr && prior_->ceiling == ceiling) {
      active_ = prior_;
      return;
    }
    local_.ceiling = ceiling;
    active_ = &local_;
    detail::active_memory_budget = active_;
  }
  ~MemoryCeilingScope() {
    if (active_ == &local_) {
      detail::active_memory_budget = prior_;
    }
  }

  MemoryCeilingScope(const MemoryCeilingScope&) = delete;
  MemoryCeilingScope& operator=(const MemoryCeilingScope&) = delete;

 private:
  detail::ActiveMemoryBudget* prior_{nullptr};
  detail::ActiveMemoryBudget* active_{nullptr};
  detail::ActiveMemoryBudget local_{};
};

inline void ensure_memory_growth(std::size_t bytes) {
  if (detail::active_memory_budget != nullptr) {
    detail::active_memory_budget->ensure(bytes);
  }
}

[[nodiscard]] inline bool memory_growth_allowed(std::size_t bytes) noexcept {
  return detail::active_memory_budget == nullptr ||
         detail::active_memory_budget->allows(bytes);
}

[[nodiscard]] inline bool memory_ceiling_active() noexcept {
  return detail::active_memory_budget != nullptr &&
         detail::active_memory_budget->ceiling != nullptr &&
         detail::active_memory_budget->ceiling->enabled();
}

template <class T, class Allocator>
void reserve_memory_vector(std::vector<T, Allocator>& values,
                           std::size_t capacity) {
  if (capacity <= values.capacity()) {
    return;
  }
  if (capacity > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
    throw std::bad_array_new_length();
  }
  ensure_memory_growth((capacity - values.capacity()) * sizeof(T));
  values.reserve(capacity);
}

template <class T, class Allocator>
void reserve_memory_vector_for_push(std::vector<T, Allocator>& values,
                                    std::size_t initial_capacity = 8) {
  if (values.size() < values.capacity()) {
    return;
  }
  const auto current = values.capacity();
  const auto target = current == 0
                          ? std::max<std::size_t>(1, initial_capacity)
                          : current <= std::numeric_limits<std::size_t>::max() / 2
                                ? current * 2
                                : current + 1;
  reserve_memory_vector(values, target);
}

}  // namespace goblin::core
