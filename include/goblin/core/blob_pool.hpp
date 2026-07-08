#pragma once

#include <cstddef>
#include <memory_resource>
#include <string>

namespace goblin::core {

// One process-wide pool for tiny-zset listpack blobs. Each blob was its own
// std::string malloc, so N tiny zsets paid N chunk headers (~16-24 B each) +
// size-class rounding + fragmentation -- the gap between in-process
// allocated_bytes and real RSS. The server is a single-threaded event loop, so
// an unsynchronized_pool_resource (the standard slab/pool allocator) is safe and
// fastest: it carves fixed-size cells out of larger chunks, amortizing the header
// over a whole chunk and reusing freed cells. Blocks larger than the cap fall
// through to malloc (rare -- only a listpack with a very large single member).
inline std::pmr::unsynchronized_pool_resource& blob_pool() noexcept {
  // Small chunks so a size class over-allocates at most one chunk's worth (the
  // default lets chunks grow geometrically, over-allocating enough to eat the
  // header amortization at these scales).
  static std::pmr::unsynchronized_pool_resource pool(
      std::pmr::pool_options{.max_blocks_per_chunk = 64,
                             .largest_required_pool_block = 4096});
  return pool;
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
