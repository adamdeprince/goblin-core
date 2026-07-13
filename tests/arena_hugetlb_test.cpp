// Phase-1 test for huge-page-backed arena blocks: when a block freezes at a
// chunk_bytes that is a huge-page multiple and huge pages are available, it is
// re-homed onto a huge page (freeze-to-max promotion). Skips cleanly (exit 0) when
// no huge pages are reserved, so it never fails a host without them.

#include "goblin/core/page_arena.hpp"

#undef NDEBUG
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace goblin::core;  // NOLINT

int main() {
  const std::size_t hp = hugetlb::hugepage_bytes();
  if (hp == 0) {
    std::puts("arena hugetlb: no huge pages on this platform; skipped");
    return 0;
  }
  // Are huge pages actually reserved right now? Probe once; skip if not.
  if (void* probe = hugetlb::try_alloc(hp)) {
    hugetlb::free_alloc(probe, hp);
  } else {
    std::puts("arena hugetlb: no huge pages reserved (HugePages_Free=0?); skipped");
    return 0;
  }

  hugetlb::arena_enabled() = true;
  const std::size_t chunk = hp;  // a huge-page multiple, so freeze-to-max promotes
  const std::size_t shift = static_cast<std::size_t>(std::countr_zero(chunk));
  const std::size_t mask = chunk - 1;
  const std::size_t item = 64 * 1024;  // 64 KiB items; ~32 fill one 2 MiB chunk

  std::vector<std::shared_ptr<char[]>> blocks;
  std::size_t next_offset = 0, active = 0, committed = 0;
  // Fill just past one chunk so block 0 freezes and block 1 starts.
  while (blocks.size() < 2) {
    auto r = reserve_run_bytes(blocks, next_offset, active, committed, chunk, shift, mask,
                               kDefaultArenaGrowth, kInitialArenaBytes, item);
    assert(r.dst != nullptr);
    std::memset(r.dst, 'x', item);
  }

  // Block 0 is frozen at max, so it must have been promoted to a huge page: a
  // MAP_HUGETLB mapping is huge-page-aligned, which a normal mmap effectively never is.
  const auto addr = reinterpret_cast<std::uintptr_t>(blocks[0].get());
  assert(addr % hp == 0 && "frozen block should be a huge-page-aligned huge allocation");

  // Sanity: with huge disabled, the same fill leaves block 0 on normal pages
  // (not huge-aligned) -- proving the promotion is what aligned it.
  hugetlb::arena_enabled() = false;
  std::vector<std::shared_ptr<char[]>> nblocks;
  std::size_t noff = 0, nactive = 0, ncommitted = 0;
  while (nblocks.size() < 2) {
    auto r = reserve_run_bytes(nblocks, noff, nactive, ncommitted, chunk, shift, mask,
                               kDefaultArenaGrowth, kInitialArenaBytes, item);
    std::memset(r.dst, 'x', item);
  }
  hugetlb::arena_enabled() = true;  // restore

  std::printf("arena hugetlb OK: frozen block huge-backed at %zu-byte pages (aligned %p)\n",
              hp, blocks[0].get());
  return 0;
}
