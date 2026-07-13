#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>

namespace goblin::core {

struct BlobPoolStats {
  std::size_t requested_live_bytes{0};
  std::size_t upstream_capacity_bytes{0};
  std::size_t live_allocations{0};
  std::size_t upstream_allocations{0};

  [[nodiscard]] std::size_t overhead_bytes() const noexcept {
    return upstream_capacity_bytes > requested_live_bytes
               ? upstream_capacity_bytes - requested_live_bytes
               : 0;
  }
};

namespace detail {

class TrackingMemoryResource final : public std::pmr::memory_resource {
 public:
  [[nodiscard]] std::size_t current_bytes() const noexcept {
    return current_bytes_;
  }
  [[nodiscard]] std::size_t allocations() const noexcept {
    return allocations_;
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    void* p = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    current_bytes_ += bytes;
    ++allocations_;
    return p;
  }

  void do_deallocate(void* p, std::size_t bytes,
                     std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    current_bytes_ -= bytes;
  }

  [[nodiscard]] bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::size_t current_bytes_{0};
  std::size_t allocations_{0};
};

class BlobPoolResource final : public std::pmr::memory_resource {
 public:
  static constexpr std::size_t kLargestPooledBlob = 1024;

  BlobPoolResource()
      : pool_(std::pmr::pool_options{.max_blocks_per_chunk = 64,
                                     .largest_required_pool_block =
                                         kLargestPooledBlob},
              &upstream_) {}

  [[nodiscard]] BlobPoolStats stats() const noexcept {
    return {.requested_live_bytes = requested_live_bytes_,
            .upstream_capacity_bytes = upstream_.current_bytes(),
            .live_allocations = live_allocations_,
            .upstream_allocations = upstream_.allocations()};
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
#if defined(__SANITIZE_ADDRESS__)
    void* p = upstream_.allocate(bytes, alignment);
#else
    void* p = pool_.allocate(bytes, alignment);
#endif
    requested_live_bytes_ += bytes;
    ++live_allocations_;
    return p;
  }

  void do_deallocate(void* p, std::size_t bytes,
                     std::size_t alignment) override {
    requested_live_bytes_ -= bytes;
    --live_allocations_;
#if defined(__SANITIZE_ADDRESS__)
    upstream_.deallocate(p, bytes, alignment);
#else
    pool_.deallocate(p, bytes, alignment);
#endif
  }

  [[nodiscard]] bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  TrackingMemoryResource upstream_;
  std::pmr::unsynchronized_pool_resource pool_;
  std::size_t requested_live_bytes_{0};
  std::size_t live_allocations_{0};
};

inline BlobPoolResource& blob_pool_resource() noexcept {
  static BlobPoolResource pool;
  return pool;
}

}  // namespace detail

// One process-wide pool for compact zset and list blobs. Each blob was its own
// malloc, so N compact values paid N chunk headers (~16-24 B each) +
// size-class rounding + fragmentation -- the gap between in-process
// allocated_bytes and real RSS. The server is a single-threaded event loop, so
// an unsynchronized_pool_resource (the standard slab/pool allocator) is safe and
// fastest for small blobs: it carves fixed-size cells out of larger chunks,
// amortizing the header over a whole chunk and reusing freed cells. Blocks over
// 1 KiB fall through to the upstream allocator. In particular, mutable
// segmented-list leaves are about 2 KiB; pooling them retained every transient
// size class after endpoint churn and cost more memory than direct allocation.
inline std::pmr::memory_resource& blob_pool() noexcept {
  // Under ASan the wrapper delegates each blob directly to new/delete so every
  // allocation retains its own redzone. Production uses the pooled path. Both
  // paths retain the same requested-vs-upstream accounting.
  return detail::blob_pool_resource();
}

[[nodiscard]] inline BlobPoolStats blob_pool_stats() noexcept {
  return detail::blob_pool_resource().stats();
}

// Stateless allocator over the shared blob pool, so a BlobString stays the exact
// size of a std::string (no per-string memory_resource pointer, unlike pmr).
template <class T>
struct BlobAllocator {
  using value_type = T;

  BlobAllocator() = default;
  template <class U>
  constexpr BlobAllocator(const BlobAllocator<U>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t n) {
    return static_cast<T*>(blob_pool().allocate(n * sizeof(T), alignof(T)));
  }
  void deallocate(T* p, std::size_t n) noexcept {
    blob_pool().deallocate(p, n * sizeof(T), alignof(T));
  }

  template <class U>
  constexpr bool operator==(const BlobAllocator<U>&) const noexcept {
    return true;
  }
};

using BlobString =
    std::basic_string<char, std::char_traits<char>, BlobAllocator<char>>;

}  // namespace goblin::core
