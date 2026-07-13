# Engineering note: 2 MB huge pages for the arenas

Goblin stores every zset member, hash field/value, and list element in a chunked arena —
large `mmap` blocks handed out sequentially. On a big collection that arena is hundreds of
megabytes, and a random read touches a random spot in it. With the default 4 KB pages, the
CPU needs one TLB entry per 4 KB, and the data-TLB only reaches ~6 MB (a few thousand
entries across L1+L2). A 200 MB arena is far past that, so a random-access workload misses
the TLB on nearly every lookup and pays a page-table walk each time.

`--arena-hugetlb` backs those arena blocks with 2 MB huge pages instead. One TLB entry now
covers 2 MB, so the same 200 MB arena needs ~100 entries rather than ~50,000 — it fits in
the TLB, and the walks go away. This note measures what that is worth.

## The knob

`--arena-hugetlb` is **off by default**, on purpose:

- It draws from the kernel's reserved huge-page pool (`vm.nr_hugepages`), which an operator
  must fill ahead of time. With an empty pool the allocator silently falls back to 4 KB
  pages, so the flag is safe to pass but does nothing until a pool exists.
- Goblin's snapshot path (`SAVE`) forks, and copy-on-write on a 2 MB page copies the whole
  2 MB on the first byte written — a write amplifier while a snapshot child is alive. So when
  huge pages are on, goblin makes `SAVE`/`LOAD` synchronous (no fork) rather than risk that.

Arena blocks default to a 2 MB chunk so each block is exactly one huge page. The huge-page
promotion happens at the point a block freezes to full size; the swiss member index is
deliberately *not* huge-backed (it reallocates on growth, which would waste a huge page to
fragmentation).

## Method

Load one hash large enough that its arena dwarfs the TLB reach, then measure random `HGET`
throughput — random field, so a random arena page each time. Two servers run concurrently,
one with `--arena-hugetlb` and one without, and the timed trials are **interleaved** (a
baseline trial, then a hugetlb trial, repeated) so that whatever else the machine is doing
hits both configs equally and cancels in the delta.

Memory is reported as `VmRSS + HugetlbPages`, because `MAP_HUGETLB` pages are not counted in
`VmRSS` — reading `VmRSS` alone would make the hugetlb config look ~200 MB leaner when it is
actually using the same memory in a different pool. The true resident figure is the sum.

Hardware: a 48-core x86-64 server, 2 MB huge pages, 4 KB base pages. The box was also busy
with unrelated I/O during the run, which is exactly why the measurement is interleaved.

## Result

8,000,000-field hash, ~214 MiB arena, 8 interleaved trials:

| config | huge-backed arena | resident (VmRSS + Hugetlb) | random `HGET` |
|---|---:|---:|---:|
| 4 KB pages (baseline) | 0 MiB | 353 MiB | 1.19 M ops/s |
| 2 MB hugetlb arena | 212 MiB | 353 MiB | 1.24 M ops/s |

Per-trial throughput delta: **+3.8% median** (range +3.1% to +5.2%). Resident delta: **0 MiB**.

## Reading it

Two things worth stating plainly.

It is memory-neutral. The whole 212 MiB arena moves from 4 KB pages to 2 MB pages and the
total resident set does not change — the pages just live in the huge-page pool instead of
ordinary RSS. Huge pages are not a memory tax here; they are the same bytes, mapped coarser.

The throughput win is real but modest — about 4% — and it is honest to say why it is not
larger. A random `HGET` first probes the swiss member index to find the field, then reads
the field and value bytes from the arena. Only the arena is huge-backed; the index is not.
So huge pages remove the arena's share of the TLB misses but not the index's, and the index
probe is a large part of a small-value lookup. On a workload that leans harder on the arena
(long values, large ranges, list scans) the arena's share is bigger and so is the win; on
this index-heavy point-lookup it is ~4%. The same 2 MB chunking applies to the zset and list
arenas, so the effect generalizes in proportion to how arena-bound the workload is.

The win is also floor-not-ceiling here: the client is Python, and end-to-end throughput is
partly client-bound, which compresses the visible delta. The server-side TLB improvement is
at least the measured 4%. A direct `dTLB-load-misses` count would show it more sharply, but
the test box has no `perf` access (`perf_event_paranoid=4`), so throughput is the proxy.

## Caveats

- Needs a reserved huge-page pool: `sudo sysctl -w vm.nr_hugepages=<N>` (bounded, compacts
  on demand) or a boot-time `hugepages=<N>` kernel argument (no fragmentation fight). With no
  pool the flag is a no-op.
- Off by default. Turn it on only with the pool filled and with the synchronous-save
  behavior understood.
- One machine, one workload. The +4% is this box on index-heavy point lookups; the direction
  (a free, memory-neutral win that grows with arena-boundedness) is general, the magnitude is
  not.

## Reproducing

```sh
# fill a 2 MB huge-page pool first (root)
sudo sysctl -w vm.nr_hugepages=512

# interleaved A/B: --arena-hugetlb on vs off, random HGET over a large hash
./benchmarks/tlb_arena_benchmark.py ./build-release/goblin-core --fields 8000000
```

The script starts both servers, loads each hash, reports huge-page-backed bytes (from
`/proc/<pid>/status` `HugetlbPages`), true resident, and the interleaved throughput delta.
`--fields`, `--value-bytes`, `--trials`, and `--seconds` are tunable; size the load under the
reserved pool (8 M fields ≈ 214 MiB ≈ 107 huge pages).
