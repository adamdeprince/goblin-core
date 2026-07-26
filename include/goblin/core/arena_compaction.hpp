#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <utility>

namespace goblin::core {

namespace detail {
inline std::atomic<std::size_t> arena_compaction_suspensions{0};
}  // namespace detail

// Forked snapshots share the parent's pages until the child exits. Arena
// compaction rewrites most live bytes and would therefore turn a compact
// snapshot into a large COW allocation. Keep the suppression process-wide:
// serving has one mutation thread, and this avoids a pointer in every object.
[[nodiscard]] inline bool arena_compaction_allowed() noexcept {
  return detail::arena_compaction_suspensions.load(std::memory_order_relaxed) ==
         0;
}

class ArenaCompactionSuspension {
 public:
  ArenaCompactionSuspension() noexcept {
    detail::arena_compaction_suspensions.fetch_add(1,
                                                    std::memory_order_relaxed);
  }

  ~ArenaCompactionSuspension() { release(); }

  ArenaCompactionSuspension(const ArenaCompactionSuspension&) = delete;
  ArenaCompactionSuspension& operator=(const ArenaCompactionSuspension&) =
      delete;

  ArenaCompactionSuspension(ArenaCompactionSuspension&& other) noexcept
      : active_(std::exchange(other.active_, false)) {}
  ArenaCompactionSuspension& operator=(
      ArenaCompactionSuspension&& other) noexcept {
    if (this != &other) {
      release();
      active_ = std::exchange(other.active_, false);
    }
    return *this;
  }

 private:
  void release() noexcept {
    if (!active_) {
      return;
    }
    const auto previous = detail::arena_compaction_suspensions.fetch_sub(
        1, std::memory_order_relaxed);
    assert(previous != 0);
    (void)previous;
    active_ = false;
  }

  bool active_{true};
};

}  // namespace goblin::core
