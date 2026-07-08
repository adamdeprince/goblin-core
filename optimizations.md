# Optimizations

A running log of Goblin Core's memory and speed optimizations: what changed, why,
the measured impact, and what's queued next. Numbers here were measured under the
same allocator/config parity as [BENCHMARKS.md](BENCHMARKS.md); where an
optimization did *not* fully win, that's stated plainly.

---

## 1. Perfect-hash command dispatch — *shipped (0.4.1)*

Command dispatch was a linear chain of ~25 case-insensitive string comparisons
(O(commands) per request, re-folding case per byte). Replaced with a `gperf`
perfect hash over the upper-cased command name for O(1) dispatch. Scalar and
portable — a SIMD tokenizer prototype was measured and dropped (short frames
can't fill wide vectors; the perfect hash is the real, ISA-independent win). The
`.gperf` source and generated header are checked in.

---

## 2. Growable, page-aligned arenas — *done, this cycle*

### Problem
Every zset/hash stored its member/field bytes in a chunked arena whose blocks
were a fixed `chunk_bytes` (default **1 MiB**). Blocks are lazily demand-paged,
so a small collection's *resident* arena was ~1 page rather than 1 MiB — but that
still made **a whole page the per-collection floor**: 16 KiB on Apple/LoongArch,
4 KiB on x86, plus a second page for the score-text cache when enabled. Across a
many-small-collections workload that is real waste, and on shrink the OS only got
memory back at whole-block (1 MiB) granularity.

### Approach
- **Only the last block per collection is variable.** It starts *sub-page* and
  grows geometrically (page-quantized once past a page) up to `chunk_bytes`, then
  freezes; every earlier block stays full and uniform. Because the frozen blocks
  are uniform, the hot read path keeps its **O(1) single-shift addressing**
  (`blocks[offset >> chunk_shift] + (offset & chunk_mask)`) unchanged — a small
  zset is just one small growable block.
- **Page-aligned, reclaimable allocation.** Blocks ≥ a page are `mmap`-backed and
  page-aligned, so freeing one `munmap`s and the OS actually reclaims the pages;
  sub-page first blocks are a plain `malloc`. Page size comes from
  `sysconf(_SC_PAGESIZE)` at runtime — never a hardcoded 4 KiB (Apple/LoongArch
  are 16 KiB). POSIX-only, behind the same `__APPLE__`/`__GLIBC__` ladder as the
  existing heap-trim.
- **Geometric growth** reuses the swiss table's conservative `2^0.25` ratio via
  the existing `--member-index-growth` knob — one shared parameter. Block size
  stays on `--zset-chunk-bytes`/`--hash-chunk-bytes` for sweeps.
- **Committed-byte accounting** replaced `blocks × chunk_bytes`, so `GOBLIN.MEMORY`
  and `GOBLIN.OPTIMIZE` report the true footprint.
- **Closed a latent copy-on-write corruption bug** found along the way: `fork()`
  shallow-copied the shared tail block and both sides kept the same `next_offset`,
  so two keys sharing a member layer and each doing a *structural* append clobbered
  each other's bytes. The growth path now copies a fork-shared active block private
  before any in-place write; a regression test reproduces the exact sequence and
  fails without the fix.

Persistence needed **no change**: the arena layout is serialized in neither the
canonical nor the accelerator layer (canonical stores portable `(score, member)`
pairs and rebuilds via `push_back`; the accelerator dumps only the swiss table).

### Results (measured)
Many small zsets, 200k × 4 *distinct* members each (distinct so each zset gets its
own arena rather than collapsing to one via the shared-member-layer optimization):

| workload | old (fixed 1 MiB) | new (growable) | change |
| --- | ---: | ---: | ---: |
| small zset RSS, naamah / x86 4 KiB pages | 5492 B/zset | **1510 B/zset** | **3.6× leaner** |
| small zset RSS, 16 KiB-page hosts (est.) | ~17.8 KB/zset | ~1.5 KB/zset | ~12× leaner |

Large zsets — **no regression**:

| members | old | new | Redis 7.2.4 | Valkey 9.1.0 |
| --- | ---: | ---: | ---: | ---: |
| 1,000,000 | 51.0 | **51.1** | 109.8 | 84.5 |
| 4,000,000 | 51.0 | **50.9** | 103.2 | 84.3 |

Reclaim-on-shrink (the win condition): deleting 95% of a large zset returned
**20–33 MB** to the OS (LoongArch / x86), with the arena's committed bytes
dropping to fit the survivors. Validated on macOS/16 KiB, LoongArch/16 KiB, and
x86/4 KiB; all tests plus ASan+UBSan clean on each.

### Honest gap
The redesign is a real win but does **not** by itself beat the incumbents on
small-collection memory — goblin is still ~10× heavier per small zset
(1510 B vs Redis 171 / Valkey 145 / Dragonfly 144). The arena is no longer the
cost; the per-zset **index machinery** is — the swiss member index, the score
index, the SoA vectors, and the COW layer, spread across ~10 separate allocations,
where a listpack is one. That motivates the next item. Benchmark:
`benchmarks/small_zsets_memory.py`.

---

## 3. Compact small-collection representation — *shipped, this cycle*

Goal: beat the incumbents on *small*-collection memory too, so goblin wins at both
ends — the small/large duality Redis gets from listpack↔skiplist, done our own way.
(License firewall: we design our **own** compact format; we do not read or copy
Redis's listpack code.)

Two orthogonal promotion axes, each **compact until a value forces a one-way
rebuild** — no demotion:

- **Score width.** Track the narrowest type that represents *every* score in the
  set exactly: **16-bit int** (chess scores/evaluations, magnitude < 64K),
  **32-bit int** (< 4G), else **f64**. Store scores at that width instead of an
  always-8-byte double — a 4× score saving in the common small-integer case. A
  `ZADD` of a **fractional** score, or one **past the current width's range**,
  triggers a rebuild one rung wider (u16→u32→f64). (Open detail: signed vs
  unsigned encoding — chess evaluations are signed.)
- **Container.** A small zset lives in **one contiguous blob** — entries packed
  back-to-back, no index — and is found by **linear scan**, which is cache-optimal
  at small *n* (no hashing, no probing, no pointer-chasing; hardware prefetch eats
  it). It converts to the full swiss+score structure past a threshold
  (~128 entries or a member > 64 bytes), on the crossing `ZADD`.

All command paths must be smart about **stopping and rebuilding** when a value
violates the current specialization (fractional / >64K / >4G score, or over the
count/size threshold). The same pattern applies to small hashes. Persistence stays
unchanged — the canonical layer rebuilds into the right representation, at the
right score width, on load.

### Score-width narrowing — shipped

Signed **i16 (±32,767) / i32 (±2.147G) / f64**, chosen at the narrowest width that
holds every score exactly. It lives in the member SoA *and* the sorted score index
(decode-to-`double` at the comparator, so the index algorithm is untouched); widens
one-way on a value that doesn't fit (`-0.0`/NaN/±inf stay f64), including a direct
i16→f64 jump; snapshots record the width and write scores at it (format **v2**), so
integer-scored snapshots shrink; and `GOBLIN.OPTIMIZE` rescans and **demotes** to the
narrowest fitting width. Measured **~12.4 B/member leaner** for a large i16 zset.

### Listpack (tiny-zset blob) — shipped, on by default, with caveats

Our own format: entries sorted by `(score, member)`, each `[enc][score][member][backlen]`
— `enc` a member-length varint, `score` inline at the zset's narrow width, `backlen`
the same varint reversed so a reverse walk is O(1). A tiny zset lives in one blob (no
swiss index / score index / SoA / CoW layers) and promotes to the full structure when
it outgrows the limit, a member is empty (no length encoding) or too long, or the blob
would exceed 64 KiB. `GOBLIN.OPTIMIZE` demotes the blob's width too.

**Measured reality (be honest about it).** With *distinct* members (the realistic
case — identical members let the full zsets share one member layer and look
artificially lean), the blob saves a **flat ~1.5× per zset** across sizes. The blob
*structure* is ~7× leaner than the full machinery, but at the store level that dilutes
to ~1.5× because the per-zset **`ZSet` handle + key string + swiss slot (~220 B)** are
the same either way — so goblin narrows but does **not yet beat** Redis's ~150 B
tiny-zset total. **First lever pulled:** `ZSet` now holds a `variant<listpack, full>`
instead of an `optional` listpack + two shared_ptrs (only one rep is ever live), so
`sizeof(ZSet)` **112 → 80 B** and every zset — stored by value in the swiss table —
shrinks; store-level per-zset dropped ~50 B (4-member listpack 319 → **266 B**).
Remaining levers to close on Redis: share `ZSetOptions` (a 32 B per-zset copy), and the
big one — store the blob directly / pointer-indirect the slot so a tiny zset's slot
isn't sized for the full alternative.

**CPU knee → threshold 32.** The blob is an O(n) scan, so the promotion threshold is a
memory-vs-CPU dial. Memory saving is ~flat with size, but `ZSCORE` goes ~2.5× at 32
entries and **perverse (>6×) by 128**; build stays ≤1.2× to 32. So the default is
**32** (`StoreOptions::zset_listpack_max_entries`), not 128. Enabling the score-string
cache (exact-text preservation, which the numeric blob can't do) keeps zsets full.
Validated on x86_64/4K, LoongArch/16K, arm64/16K + ASan.

**Danger — promotion during a copy-on-write write.** Widening the score width
rebuilds the score array O(n). When a *full* zset is CoW-shared (many keys with
identical members + scores share one member layer), the write that promotes it *first
forks* that layer O(n), then rebuilds the width O(n) — so a single `ZADD` of a
fractional or out-of-range score to a large shared set is a latency and
transient-memory spike. (Listpack zsets are standalone blobs and never share, so this
applies only after promotion to full.) Widening never happens on the read path and is
one-way; `GOBLIN.OPTIMIZE` reclaims an over-wide width by demoting.
