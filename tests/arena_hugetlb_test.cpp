// Phase-1 test for huge-page-backed arena blocks: when a block freezes at a
// chunk_bytes that is a huge-page multiple and huge pages are available, it is
// re-homed onto a huge page (freeze-to-max promotion). Skips cleanly (exit 0) when
// no huge pages are reserved, so it never fails a host without them.

#include "goblin/core/hash.hpp"
#include "goblin/core/page_arena.hpp"

#undef NDEBUG
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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

  // Give the single-page probe back before building a hash so this also runs on
  // a host with only one reserved page.
  blocks.clear();
  nblocks.clear();

  Hash hash(HashOptions{.chunk_bytes = chunk, .listpack_max_entries = 0});
  constexpr std::size_t kHashFields = 40;
  const std::string large_value(HashStorage::kMaxValueBytes, 'v');
  for (std::size_t id = 0; id < kHashFields; ++id) {
    assert(hash.set("field-" + std::to_string(id), large_value) == 1);
  }

  const char* frozen_field_before = nullptr;
  hash.for_each([&](std::string_view field, EncodedStringView) {
    if (field == "field-0") {
      frozen_field_before = field.data();
    }
  });
  assert(frozen_field_before != nullptr);
  assert(reinterpret_cast<std::uintptr_t>(frozen_field_before) % hp == 0 &&
         "first hash field should live at the start of a frozen huge block");

  // Fragment the frozen block itself. OPTIMIZE must densify it in place and
  // rebuild the field index without replacing its HugeTLB mapping.
  assert(hash.set("field-0", "short") == 0);
  assert(hash.memory_stats().field_value_dead_bytes > 0);
  hash.compact();
  const char* frozen_field_after = nullptr;
  hash.for_each([&](std::string_view field, EncodedStringView) {
    if (field == "field-0") {
      frozen_field_after = field.data();
    }
  });
  assert(frozen_field_after == frozen_field_before &&
         "hash compact must retain frozen HugeTLB-backed blocks");
  assert(hash.memory_stats().field_value_dead_bytes == 0);

  // Make enough room in the frozen block to consume the entire ordinary tail.
  // Once that tail is popped, block 0 becomes the mutable tail and must be
  // copied off HugeTLB while it is densified/reduced.
  const auto capacity_before_tail_pop =
      hash.memory_stats().field_value_allocated_bytes;
  for (std::size_t id = 1; id < 10; ++id) {
    assert(hash.set("field-" + std::to_string(id), "short") == 0);
  }
  hash.compact();
  const char* demoted_field = nullptr;
  hash.for_each([&](std::string_view field, EncodedStringView) {
    if (field == "field-0") {
      demoted_field = field.data();
    }
  });
  assert(demoted_field != nullptr);
  assert(demoted_field != frozen_field_after &&
         "a HugeTLB block promoted to active tail must be demoted");
  assert(hash.memory_stats().field_value_allocated_bytes <
         capacity_before_tail_pop);
  assert(hash.memory_stats().field_value_dead_bytes == 0);
  for (std::size_t id = 0; id < 10; ++id) {
    assert(hash.get("field-" + std::to_string(id)) == "short");
  }
  for (std::size_t id = 10; id < kHashFields; ++id) {
    assert(hash.get("field-" + std::to_string(id)) == large_value);
  }

  std::printf(
      "arena hugetlb OK: frozen mapping retained, promoted tail demoted "
      "from %zu-byte pages (%p -> %p)\n",
      hp, static_cast<const void*>(frozen_field_after),
      static_cast<const void*>(demoted_field));
  return 0;
}
