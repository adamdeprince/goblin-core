#pragma once

// Page-aligned, individually-reclaimable arena blocks.
//
// A block >= one page is allocated with mmap (page-aligned; freeing it munmaps,
// so the OS actually reclaims the pages — the whole point). A sub-page block (a
// brand-new collection's tiny first block) is a plain malloc: too small to page-
// align, and too small to matter. A custom deleter carried in the shared_ptr
// frees each the right way, so blocks of both kinds live in one vector.
//
// Growing a block = allocate a new one + memcpy + drop the old (see the arena
// callers); this header only owns the allocation policy and the runtime page
// size (sysconf, not a hardcoded 4 KiB -- Apple/LoongArch use 16 KiB pages).

#include "goblin/core/hugetlb.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#if defined(__APPLE__) || defined(__GLIBC__)
#include <sys/mman.h>
#include <unistd.h>
#define GOBLIN_HAVE_MMAP 1
#endif

namespace goblin::core {

// Geometric growth ratio for an arena's active (last) block -- the same
// conservative 2^0.25 the swiss table uses (kDefaultGrowth), so one
// --member-index-growth knob can tune both.
inline constexpr double kDefaultArenaGrowth = 1.1892071150027210;

// A brand-new collection's first block starts this small (sub-page) and grows.
inline constexpr std::size_t kInitialArenaBytes = 64;

// Runtime OS page size, queried once. 16 KiB on Apple silicon and LoongArch,
// 4 KiB on x86 -- never assume.
[[nodiscard]] inline std::size_t page_bytes() noexcept {
#if defined(GOBLIN_HAVE_MMAP)
  static const std::size_t cached = [] {
    const long value = ::sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<std::size_t>(value) : std::size_t{4096};
  }();
  return cached;
#else
  return 4096;
#endif
}

[[nodiscard]] inline std::size_t round_up_to_page(std::size_t bytes) noexcept {
  const auto page = page_bytes();
  return ((bytes + page - 1) / page) * page;
}

// The bytes actually committed for a requested size: page-rounded when the block
// is mmap-backed (>= a page), exact otherwise. Callers use this for accounting so
// GOBLIN.MEMORY reports the true resident-capable footprint.
[[nodiscard]] inline std::size_t page_block_alloc_bytes(std::size_t bytes) noexcept {
#if defined(GOBLIN_HAVE_MMAP)
  if (bytes >= page_bytes()) {
    return round_up_to_page(bytes);
  }
#endif
  return bytes;
}

// Deleter for a block: munmap if it was mmap'd, else free. Carries the mapped
// length (munmap needs it) and which path allocated it.
struct PageBlockDeleter {
  std::size_t bytes{0};
  bool mapped{false};

  void operator()(char* ptr) const noexcept {
    if (ptr == nullptr) {
      return;
    }
#if defined(GOBLIN_HAVE_MMAP)
    if (mapped) {
      ::munmap(ptr, bytes);
      return;
    }
#endif
    std::free(ptr);
  }
};

// Allocate a block able to hold at least `bytes`. >= a page -> mmap (page-aligned,
// reclaimable on free); smaller -> malloc. The returned shared_ptr frees it the
// right way. Uninitialized (like the previous new char[] / make_unique_for_overwrite);
// only written bytes fault in.
[[nodiscard]] inline std::shared_ptr<char[]> alloc_page_block(std::size_t bytes) {
#if defined(GOBLIN_HAVE_MMAP)
  if (bytes >= page_bytes()) {
    const std::size_t mapped_bytes = round_up_to_page(bytes);
    void* ptr = ::mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
      throw std::bad_alloc();
    }
    return std::shared_ptr<char[]>(static_cast<char*>(ptr),
                                   PageBlockDeleter{mapped_bytes, true});
  }
#endif
  void* ptr = std::malloc(bytes);
  if (ptr == nullptr) {
    throw std::bad_alloc();
  }
  return std::shared_ptr<char[]>(static_cast<char*>(ptr),
                                 PageBlockDeleter{bytes, false});
}

// Try to re-home a just-frozen block onto a huge page: allocate a huge block of
// exactly `chunk_bytes`, copy the `used` live bytes over, and return it -- or nullptr
// if `chunk_bytes` is not a huge-page multiple or huge pages are unavailable right
// now, in which case the caller leaves the block at its grown (normal-page) size.
// This is the "eat the memcpy" promotion, paid once when a block freezes at max, so
// a frozen block becomes a single huge page. munmap frees a huge mapping the same
// way, so PageBlockDeleter needs no change. Best-effort and non-latching: a later
// freeze retries, since the huge-page pool restocks as blocks are freed.
[[nodiscard]] inline std::shared_ptr<char[]> try_promote_to_huge(
    const std::shared_ptr<char[]>& block, std::size_t used, std::size_t chunk_bytes) {
#if defined(GOBLIN_HAVE_MMAP)
  // Gate on the process switch first: is_hugepage_multiple hits a cached sysfs
  // size, but try_alloc is the expensive path; skip all of it when disabled.
  if (!hugetlb::arena_enabled() || !hugetlb::is_hugepage_multiple(chunk_bytes)) {
    return nullptr;
  }
  if (void* hp = hugetlb::try_alloc(chunk_bytes)) {
    auto huge = std::shared_ptr<char[]>(static_cast<char*>(hp),
                                        PageBlockDeleter{chunk_bytes, true});
    if (used != 0 && block) {
      std::memcpy(huge.get(), block.get(), used);
    }
    return huge;
  }
#else
  (void)block;
  (void)used;
  (void)chunk_bytes;
#endif
  return nullptr;
}

// Freeze-to-max helper for the append paths: when a block can no longer accept
// the next item, optionally re-home it onto one huge page and account the
// capacity jump in `committed_bytes`. Returns true if promotion happened.
// Callers that are about to *evacuate* the block (compaction) must not call
// this -- promoting a block you are about to free wastes a huge page and a
// full-chunk memcpy.
inline bool maybe_freeze_block_to_huge(std::shared_ptr<char[]>& block,
                                       std::size_t used, std::size_t chunk_bytes,
                                       std::size_t current_capacity,
                                       std::size_t& committed_bytes) {
  if (auto huge = try_promote_to_huge(block, used, chunk_bytes)) {
    committed_bytes += chunk_bytes - current_capacity;
    block = std::move(huge);
    return true;
  }
  return false;
}

// Grow a block to `new_bytes`: allocate a fresh block, copy `used` bytes over,
// return it. The old block's shared_ptr is released by the caller -- if another
// arena (a COW split) still holds it, it survives; otherwise it's freed/munmapped.
// This is the one place growth costs a memcpy, and it is also what makes growth
// COW-safe (never writes a shared/frozen buffer).
[[nodiscard]] inline std::shared_ptr<char[]> grow_page_block(
    const std::shared_ptr<char[]>& old, std::size_t used, std::size_t new_bytes) {
  auto fresh = alloc_page_block(new_bytes);
  if (used != 0 && old) {
    std::memcpy(fresh.get(), old.get(), used);
  }
  return fresh;
}

// Next active-block size when growing: geometric on `growth`, at least `needed`,
// page-rounded once it reaches a page (so a sub-page first block grows byte-wise,
// then in whole-page steps).
[[nodiscard]] inline std::size_t next_grow_bytes(std::size_t current,
                                                 std::size_t needed,
                                                 double growth) noexcept {
  auto want = static_cast<std::size_t>(static_cast<double>(current) * growth);
  if (want < needed) {
    want = needed;
  }
  if (want <= current) {
    want = current + 1;
  }
  if (want >= page_bytes()) {
    want = round_up_to_page(want);
  }
  return want;
}

struct RunReservation {
  std::uint32_t offset{0};
  char* dst{nullptr};
};

// Reserve `size` contiguous bytes in a growable, page-aligned byte run and return
// {global offset, write pointer}. The addressing invariant is preserved: each
// block owns a `chunk_bytes` logical slot, so a member's offset still resolves via
// `blocks[offset >> chunk_shift].get() + (offset & chunk_mask)`. The active (last)
// block is physically `active_bytes <= chunk_bytes` and grows in place
// (realloc+memcpy) to fit; when an item can't fit even a full block the current
// one freezes and a fresh tiny block (`initial_bytes`, up to `chunk_bytes`) starts.
// A block still shared after a COW fork is copied private before any in-place write
// -- this closes the fork tail-aliasing bug and keeps frozen bytes immutable.
// Advances `next_offset` and `committed_bytes`/`active_bytes`; the CALLER writes
// `size` bytes at `dst` and advances its own used-bytes counter. `size == 0`
// returns a null `dst` (nothing to write).
[[nodiscard]] inline RunReservation reserve_run_bytes(
    std::vector<std::shared_ptr<char[]>>& blocks, std::size_t& next_offset,
    std::size_t& active_bytes, std::size_t& committed_bytes,
    std::size_t chunk_bytes, std::size_t chunk_shift, std::size_t chunk_mask,
    double growth, std::size_t initial_bytes, std::size_t size) {
  if (size == 0) {
    return {static_cast<std::uint32_t>(next_offset), nullptr};
  }
  std::size_t block_offset = next_offset & chunk_mask;
  std::size_t block_index = next_offset >> chunk_shift;

  if (block_index < blocks.size()) {  // writing into the active (last) block
    if (block_offset + size <= active_bytes) {
      if (blocks[block_index].use_count() > 1) {  // COW a fork-shared block
        blocks[block_index] =
            grow_page_block(blocks[block_index], block_offset, active_bytes);
      }
    } else if (block_offset + size <= chunk_bytes) {  // grow to fit (also COWs)
      const std::size_t want = std::min(
          chunk_bytes, next_grow_bytes(active_bytes, block_offset + size, growth));
      blocks[block_index] =
          grow_page_block(blocks[block_index], block_offset, want);
      const std::size_t committed = page_block_alloc_bytes(want);
      committed_bytes += committed - active_bytes;
      active_bytes = committed;
    } else {  // would straddle chunk_bytes; freeze this block, take the next slot
      // Freeze-to-max: re-home onto one huge page when enabled (eat the memcpy once).
      // A normal frozen block stays at its grown size; only a huge block reaches max.
      (void)maybe_freeze_block_to_huge(blocks[block_index], block_offset,
                                       chunk_bytes, active_bytes, committed_bytes);
      next_offset += chunk_bytes - block_offset;
      block_offset = 0;
      block_index += 1;
    }
  }

  if (block_index >= blocks.size()) {  // fresh active block from a tiny floor
    const std::size_t want = std::min(chunk_bytes, std::max(initial_bytes, size));
    blocks.push_back(alloc_page_block(want));
    const std::size_t committed = page_block_alloc_bytes(want);
    committed_bytes += committed;
    active_bytes = committed;
    block_offset = 0;
    block_index = blocks.size() - 1;
  }

  const auto offset = static_cast<std::uint32_t>(next_offset);
  char* dst = blocks[block_index].get() + block_offset;
  next_offset += size;
  return {offset, dst};
}

// A {block, offset} pair returned by reserve_run_bytes_split: the block index and
// the in-block byte offset as two independent u32s, resolved as
// blocks[block].get() + offset.
struct RunReservationSplit {
  std::uint32_t block{0};
  std::uint32_t offset{0};
  char* dst{nullptr};
};

// Split-address variant of reserve_run_bytes for the unified keyspace arena. The
// packed variant above truncates the global position to a single u32, capping an
// arena at 4 GiB -- unacceptable here, where every key AND every string value
// share one arena. This returns the block index and in-block offset separately,
// leaving next_offset a full size_t, so total capacity is chunk_bytes << 32 (4
// PiB at the 1 MiB default) rather than 4 GiB. The grow/freeze/COW policy is
// otherwise identical to reserve_run_bytes.
[[nodiscard]] inline RunReservationSplit reserve_run_bytes_split(
    std::vector<std::shared_ptr<char[]>>& blocks, std::size_t& next_offset,
    std::size_t& active_bytes, std::size_t& committed_bytes,
    std::size_t chunk_bytes, std::size_t chunk_shift, std::size_t chunk_mask,
    double growth, std::size_t initial_bytes, std::size_t size) {
  if (size == 0) {
    return {static_cast<std::uint32_t>(next_offset >> chunk_shift),
            static_cast<std::uint32_t>(next_offset & chunk_mask), nullptr};
  }
  std::size_t block_offset = next_offset & chunk_mask;
  std::size_t block_index = next_offset >> chunk_shift;

  if (block_index < blocks.size()) {  // writing into the active (last) block
    if (block_offset + size <= active_bytes) {
      if (blocks[block_index].use_count() > 1) {  // COW a fork-shared block
        blocks[block_index] =
            grow_page_block(blocks[block_index], block_offset, active_bytes);
      }
    } else if (block_offset + size <= chunk_bytes) {  // grow to fit (also COWs)
      const std::size_t want = std::min(
          chunk_bytes, next_grow_bytes(active_bytes, block_offset + size, growth));
      blocks[block_index] =
          grow_page_block(blocks[block_index], block_offset, want);
      const std::size_t committed = page_block_alloc_bytes(want);
      committed_bytes += committed - active_bytes;
      active_bytes = committed;
    } else {  // would straddle chunk_bytes; freeze this block, take the next slot
      // Freeze-to-max: same policy as reserve_run_bytes (shared helper).
      (void)maybe_freeze_block_to_huge(blocks[block_index], block_offset,
                                       chunk_bytes, active_bytes, committed_bytes);
      next_offset += chunk_bytes - block_offset;
      block_offset = 0;
      block_index += 1;
    }
  }

  if (block_index >= blocks.size()) {  // fresh active block from a tiny floor
    const std::size_t want = std::min(chunk_bytes, std::max(initial_bytes, size));
    blocks.push_back(alloc_page_block(want));
    const std::size_t committed = page_block_alloc_bytes(want);
    committed_bytes += committed;
    active_bytes = committed;
    block_offset = 0;
    block_index = blocks.size() - 1;
  }

  char* dst = blocks[block_index].get() + block_offset;
  next_offset += size;
  return {static_cast<std::uint32_t>(block_index),
          static_cast<std::uint32_t>(block_offset), dst};
}

}  // namespace goblin::core
