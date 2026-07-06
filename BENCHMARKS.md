# Benchmarks

Goblin Core versus Redis 7.2.4, Redis 8.8, and Valkey 9.1, with an emphasis on
memory. Every number below was measured under **allocator and configuration
parity** (see [Methodology](#methodology)) so the only variable is the engine —
and, for Goblin Core, the data structure. The three Redis-family engines all run
their own bundled **jemalloc 5.3.0** with a [shared config](redis-parity.conf);
Goblin Core runs its native glibc allocator (its best — see
[Allocators](#allocators)). Host: Ubuntu, Intel Xeon 6975P-C, server and client
pinned to separate cores.

License note: **Redis 7.2.4** is BSD-3-Clause (the last open-source Redis) and is
the format Goblin Core imports; **Redis 8.8** is RSALv2/SSPL (source-available);
**Valkey 9.1** is BSD-3-Clause (the community fork).

## Memory (the headline)

RSS per member after a load-then-`GOBLIN.OPTIMIZE` sequence, across a size sweep.
Goblin Core is flat and holds a sorted set in **roughly half** the resident set
of any competitor.

**RSS bytes/member:**

| members | Goblin (glibc) | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| 250K | `49.7` | `103.8` | `78.7` | `85.0` |
| 500K | `49.2` | `107.6` | `82.5` | `84.9` |
| 1M | `49.2` | `109.7` | `82.5` | `84.5` |
| 2M | `49.0` | `106.5` | `80.3` | `84.4` |
| 4M | `48.8` | `103.2` | `79.2` | `84.3` |

**Goblin Core RSS as a fraction of each engine:** ~`46%` of Redis 7.2.4, ~`60%`
of Redis 8.8, ~`58%` of Valkey 9.1 — stable at every size. Redis 8.8 is markedly
leaner than 7.2.4, so the headline against the leanest competitor is "roughly
half."

**Fragmentation (RSS / used_memory):** Goblin Core `1.0–1.2`, Redis/Valkey
`1.0–1.4`. Goblin Core's RSS tracks its actual allocation because it returns
freed pages with `malloc_trim` after `OPTIMIZE`; there is little allocator slack
to reclaim.

The advantage is structural — a handful of big contiguous allocations (the
member-bytes arena, the Swiss index, the score-index blocks, the struct-of-arrays
offset/length/score columns) instead of a skiplist node + dict entry + string per
member. It does not depend on the allocator (see below).

## Allocators

A memory comparison is only fair if the allocator is held constant, because
Redis's footprint depends heavily on jemalloc. So all three Redis-family engines
are built with their bundled **jemalloc 5.3.0** (identical version), and Goblin
Core is shown on both its default glibc and on the same jemalloc:

| Goblin Core allocator | RSS bytes/member | fragmentation |
| --- | ---: | ---: |
| glibc + `malloc_trim` (default) | `49.2` | `1.08` |
| jemalloc 5.3.0 (via `LD_PRELOAD`) | `97.6` | `2.08` |

jemalloc **doubles** Goblin Core's RSS. `GOBLIN.OPTIMIZE` frees and reallocates
the packed indexes, and Goblin Core hands those pages back with `malloc_trim` — a
glibc call that is a no-op under jemalloc, which retains the pages instead. So
each engine is measured on its *optimal* allocator: Redis/Valkey on jemalloc
(their default, their best), Goblin Core on glibc. The glibc number is Goblin
Core's real footprint, not an allocator trick — forcing jemalloc on would make it
look twice as large.

## Throughput

On pipelined `redis-benchmark -P 16 -c 1`, Goblin Core is broadly competitive
with Redis 8.8 and Valkey 9.1, and consistently ahead on `ZADD` (writes) across
every run.

Precise per-operation ops/sec figures are omitted here on purpose. The benchmark
host is a shared cloud VM, and these short microbenchmarks show 2–2.5× run-to-run
variance — enough to invert same-engine op orderings (a run where `ZRANK`'s
skiplist traversal "beats" the O(1) `ZSCORE` hash lookup is noise, not a result).
Any single ops/sec number would report the host's jitter, not the server. Unlike
throughput, the memory, latency, and persistence figures in this document are
footprint- or large-sample-based and stable across runs. Reliable per-operation
throughput will be published from a quiet, dedicated host.

## Latency

Goblin Core writes each reply the instant it is ready; Redis and Valkey defer
replies to the event-loop boundary. On a single connection that gives Goblin Core
a real latency edge — and under heavy fan-out it costs a little.

**Round trip by client count** — `redis-benchmark` PING (rps / p50):

| clients | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| 1 | `57.6K` / `7µs` | `33.6K` / `31µs` | `46.1K` / `15µs` | `42.6K` / `31µs` |
| 50 | `227K` / `119µs` | `183K` / `143µs` | `183K` / `143µs` | `213K` / `119µs` |
| 500 | `164K` / `1.56ms` | `171K` / `1.44ms` | `182K` / `1.375ms` | `181K` / `1.39ms` |

At one client Goblin Core answers in `7` µs — about `2×` the leaner Redis 8.8 and
`4×` Redis 7.2.4 and Valkey — and it still leads at 50. At **500 concurrent
clients saturating the single core it trails by ~11%** (1.56 ms vs Redis 8.8's
1.375 ms): its immediate per-reply `write()` is one syscall per reply, while Redis
batches replies with `writev`, amortizing syscalls at high fan-out. This is a
deliberate trade — the immediate-write design optimizes the common single-op round
trip (a web-app page load) and slips only slightly under heavy overload.

## Persistence

Same ~950K-member dataset loaded into each engine; each saves and loads its own
format. "load" is startup-to-ready minus the empty-start baseline.

| engine | save | file | load |
| --- | ---: | ---: | ---: |
| **Goblin Core** | `0.116s` | `39.1` MB | **`0.058s`** |
| Redis 7.2.4 | `0.159s` | `26.6` MB | `0.262s` |
| Redis 8.8 | `0.114s` | `26.6` MB | `0.282s` |
| Valkey 9.1 | `0.119s` | `26.6` MB | `0.205s` |

Goblin Core **loads ~3.5–4.9× faster** — its default snapshot dumps the packed
indexes (the "accelerator") and `memcpy`s them back rather than rebuilding. Save
is comparable to modern Redis now (8.8 caught up), but Goblin Core's save is a
**non-blocking background `fork()`** — the command returns immediately and the
server keeps serving while the child writes. The cost is a **~1.5× larger file**
(the accelerator); `GOBLIN.SAVE <path> NOACCEL` drops it to near Redis's size at
the cost of a rebuild-on-load. Goblin Core also imports a Redis RDB
(`--load dump.rdb`, Redis 2.6–7.2.x) for migration.

## Write-Path Tail Latency

The memory win comes from a Swiss-table member index that grows by reindexing: an
amortized O(1) insert with an occasional synchronous O(n) rehash. That rehash is
a pause, visible in the extreme tail of write latency. 1M individually-timed
`ZADD`s growing one set, microseconds:

| | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Goblin Core | `9.9` | `40.0` | `46.2` | `29,856` |
| Redis 8.8 | `33.7` | `45.8` | `52.6` | `1,439` |
| Valkey 9.1 | `33.6` | `45.5` | `53.0` | `1,133` |

Goblin Core is faster through p99.9; the rehash appears at the very tail — of 1M
writes, ~19 crossed 1 ms, the largest (the final ~1M-member reindex) at ~30 ms,
versus Redis/Valkey's flat ~1.5 ms max. Rare (beyond p99.99) and the price of the
memory layout; `GOBLIN.OPTIMIZE` performs the reindex on demand, moving it out of
the serving path.

## Methodology

**Parity — the point of this whole document.**

- **Allocator:** all three Redis-family engines built with their bundled
  jemalloc 5.3.0 (`make MALLOC=jemalloc`, identical version). Goblin Core on its
  native glibc (its optimal allocator; jemalloc doubles its RSS, see above). Both
  `used_memory` and RSS are reported, plus the fragmentation ratio.
- **Config:** all three Redis-family engines load the same
  [`benchmarks/redis-parity.conf`](redis-parity.conf) — persistence off,
  `maxmemory-policy noeviction`, `activedefrag no`, `io-threads 1`, and the
  listpack/intset thresholds pinned to shared defaults. Differing configs are the
  most common way a benchmark gets contested; the file is published here.
- **Host:** Ubuntu, Intel Xeon 6975P-C. Server pinned to one core, client to
  another (`taskset`), min of repeated runs.
- **RSS** via `ps -o rss`, measured after load and `GOBLIN.OPTIMIZE`;
  `used_memory` via `INFO memory` (Redis/Valkey) and `GOBLIN.MEMORY` (Goblin
  Core).

**Reproduce:**

- Memory + throughput: `benchmarks/zset_benchmark.py --target {goblin,redis}
  --redis-server <path> --members N` (`--format json`). It loads the parity
  config for Redis automatically.
- Depth-1 and PING latency: `benchmarks/write_tail_latency.cpp`
  (`... <host> <port> <n> [--ping]`).
- Concurrency: `redis-benchmark -c <clients> -P 1 -n N ping`.
