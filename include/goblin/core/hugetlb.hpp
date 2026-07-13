#pragma once

// Best-effort huge-page (hugetlb) allocation for arena blocks. Linux-only; on other
// platforms hugepage_bytes() is 0 and try_alloc() always reports failure, so callers
// transparently fall back to normal pages.
//
// Huge pages cut TLB pressure for large, TLB-bound working sets -- the value arenas
// (a multi-GB sorted set walks thousands of base pages). Allocation is anonymous
// MAP_HUGETLB: process-private, so unlike the shared-memory ring it needs no
// hugetlbfs mount or symlink -- just reserved pages in the pool.
//
// It is best-effort and NEVER latches failure. Every call re-attempts the huge
// mapping, because the per-node huge-page pool restocks as blocks (huge or normal)
// are freed and physical memory coalesces -- so a block that fell back to normal
// pages does not doom later blocks to the same.

#include <cstddef>

#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <bit>
#include <cstdio>
#include <dirent.h>
#endif

namespace goblin::core::hugetlb {

#if defined(__linux__)

// Some libc headers gate these behind feature macros (off under strict -std=c++23);
// the constants are ABI-stable on Linux, so define them if absent.
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x20
#endif
#endif
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

// Smallest huge-page size strictly larger than the base page, in bytes, or 0 if the
// kernel offers none. Read once from /sys/kernel/mm/hugepages/hugepages-<N>kB -- so
// 2 MiB on 4 KiB-page x86 and the 32 MiB-class size on a 16 KiB-page box, no
// hardcoding.
[[nodiscard]] inline std::size_t hugepage_bytes() noexcept {
  static const std::size_t cached = []() -> std::size_t {
    DIR* d = ::opendir("/sys/kernel/mm/hugepages");
    if (d == nullptr) {
      return 0;
    }
    const auto base = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    std::size_t best = 0;
    for (const dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
      unsigned long long kib = 0;
      if (std::sscanf(e->d_name, "hugepages-%llukB", &kib) != 1) {
        continue;
      }
      const auto bytes = static_cast<std::size_t>(kib) * 1024;
      if (bytes > base && (best == 0 || bytes < best)) {
        best = bytes;
      }
    }
    ::closedir(d);
    return best;
  }();
  return cached;
}

// True when `bytes` is a nonzero whole multiple of the huge-page size (so it can be
// backed by huge pages under the power-of-two chunk addressing).
[[nodiscard]] inline bool is_hugepage_multiple(std::size_t bytes) noexcept {
  const std::size_t hp = hugepage_bytes();
  return hp != 0 && bytes != 0 && (bytes & (hp - 1)) == 0;
}

// Attempt an anonymous huge-page mapping of exactly `bytes` (must be a multiple of
// hugepage_bytes()). Returns the pointer, or nullptr if huge pages are unavailable
// right now -- the caller then uses normal pages and simply tries again on the next
// block. Never disables future attempts.
[[nodiscard]] inline void* try_alloc(std::size_t bytes) noexcept {
  if (!is_hugepage_multiple(bytes)) {
    return nullptr;
  }
  const int shift = std::countr_zero(hugepage_bytes());  // log2(huge-page size)
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB |
                       (shift << MAP_HUGE_SHIFT),
                   -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

// Release a mapping obtained from try_alloc(). munmap is the same call as for a
// normal anonymous mapping; the length must be the huge-page-multiple that was
// requested.
inline void free_alloc(void* ptr, std::size_t bytes) noexcept {
  if (ptr != nullptr) {
    ::munmap(ptr, bytes);
  }
}

#else  // not Linux: no hugetlb, so every huge allocation "fails" into normal pages.

[[nodiscard]] inline std::size_t hugepage_bytes() noexcept { return 0; }
[[nodiscard]] inline bool is_hugepage_multiple(std::size_t) noexcept { return false; }
[[nodiscard]] inline void* try_alloc(std::size_t) noexcept { return nullptr; }
inline void free_alloc(void*, std::size_t) noexcept {}

#endif

}  // namespace goblin::core::hugetlb
