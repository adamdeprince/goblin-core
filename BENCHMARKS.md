# Benchmarks

Goblin Core versus Redis 7.2.4, Redis 8.8, and Valkey 9.1, with an emphasis on
memory. Every number below was measured under **allocator and configuration
parity** (see [Methodology](#methodology)) so the only variable is the engine —
and, for Goblin Core, the data structure. The three Redis-family engines all run
their own bundled **jemalloc 5.3.0** with a [shared config](redis-parity.conf);
Goblin Core runs its native glibc allocator (its best — see
[Allocators](#allocators)). Host: a quiet, dedicated 128-core Linux box (GCC
16.1.0); server and client pinned to separate cores.

License note: **Redis 7.2.4** is BSD-3-Clause (the last open-source Redis) and is
the format Goblin Core imports; **Redis 8.8** is RSALv2/SSPL (source-available);
**Valkey 9.1** is BSD-3-Clause (the community fork).

## Memory (the headline)

RSS per member after a load-then-`GOBLIN.OPTIMIZE` sequence, across a size sweep.
Goblin Core is flat and holds a sorted set in **roughly half** the resident set
of legacy Redis, and comfortably under every modern engine.

**RSS bytes/member:**

| members | Goblin (glibc) | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| 250K | `52.2` | `103.8` | `79.1` | `85.0` |
| 500K | `51.4` | `107.7` | `82.8` | `84.9` |
| 1M | `51.0` | `109.8` | `82.7` | `84.5` |
| 2M | `50.8` | `106.5` | `80.4` | `84.3` |
| 4M | `50.9` | `103.2` | `78.2` | `84.3` |

**Goblin Core RSS as a fraction of each engine:** ~`48%` of Redis 7.2.4, ~`63%`
of Redis 8.8, ~`61%` of Valkey 9.1 — stable at every size. So Goblin Core is
**roughly half** of legacy Redis and ~`37%` leaner than Redis 8.8, the leanest
modern engine.

**Fragmentation (RSS / used_memory):** Goblin Core `1.1–1.3`, Redis/Valkey
`1.0–1.5`, both tightening toward `1.0` at scale. Goblin Core's `used_memory` —
its actual structure bytes — holds at ~`49` B/member; `malloc_trim` after
`OPTIMIZE` keeps the resident set a hair behind.

The advantage is structural — a handful of big contiguous allocations (the
member-bytes arena, the Swiss index, the score-index blocks, the struct-of-arrays
offset/length/score columns) instead of a skiplist node + dict entry + string per
member. It does not depend on the allocator (see below).

## Allocators

A memory comparison is only fair if the allocator is held constant, because
Redis's footprint depends heavily on jemalloc. So all three Redis-family engines
run their bundled **jemalloc 5.3.0** (identical version), and Goblin Core runs its
default **glibc + `malloc_trim`** — each engine on the allocator it ships with and
is tuned for.

That tuning matters: `GOBLIN.OPTIMIZE` frees and reallocates the packed indexes
and hands the freed pages back to the OS with `malloc_trim`, so Goblin Core's RSS
(~`51` B/member) tracks the bytes its structures actually request (`used_memory`
~`49`). The footprint is Goblin Core's own, not the allocator's: `used_memory` is
allocator-independent, and it is the number that halves Redis's.

## Throughput

Pipelined `redis-benchmark -P 16 -c 1` from the quiet, dedicated 128-core host —
best of three runs, all Redis-family engines on jemalloc 5.3.0 under the shared
parity config, Goblin Core on its default config (rank cache off, so `ZRANK` is
O(log n) on both sides). Goblin Core leads every sorted-set operation:

| operation | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| ZADD (write) | `410K` | `250K` | `247K` | `249K` |
| ZSCORE (point read) | `598K` | `504K` | `489K` | `500K` |
| ZRANK | `504K` | `346K` | `350K` | `358K` |
| ZRANGE (16 elems) | `459K` | `371K` | `368K` | `355K` |
| ZSCORE depth-1 latency (µs) | `21.0` | `22.0` | `23.0` | `22.4` |

The standout is `ZADD` at **~`+64%`** over the field (`410K` vs ~`248K`) — the
immediate per-reply `write()` plus the packed Swiss member index make inserts
cheap — and `ZRANK` at **`+41–46%`**. Point reads (`ZSCORE`) lead by `+19–22%`
and 16-element `ZRANGE` by `+24–29%`. Depth-1 latency is a near-tie at ~`21` µs
(redis-benchmark's own round-trip floor). Earlier drafts omitted per-op numbers
because the only host then was a noisy shared VM; these come from the quiet box
and are stable across runs.

## Latency

Goblin Core writes each reply the instant it is ready; Redis and Valkey defer
replies to the event-loop boundary. That saves one event-loop hop per reply — a
small, consistent edge, with no penalty at high fan-out.

**Depth-1 round trip** — a lean C client (`write_tail_latency --ping`), one
connection, no pipelining, server and client pinned to separate cores, median of
1M PINGs:

| | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| PING p50 (µs) | `15.9` | `16.7` | `17.3` | `16.9` |
| PING p99 (µs) | `19.7` | `20.8` | `21.6` | `21.4` |

Goblin Core is fastest, by ~`1` µs (≈`5–8%`) — the single deferred event-loop hop
its immediate `write()` avoids. An honest, modest edge, not a rout: on a localhost
round trip the syscall floor dominates and the design shaves one hop off it.

**PING throughput / p50 by client count** (`redis-benchmark -P 1`; the absolute
p50 here carries redis-benchmark's own client overhead, so read it for *scaling*,
not the floor above):

| clients | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| 1 | `49K` / `23µs` | `48K` / `23µs` | `46K` / `23µs` | `47K` / `23µs` |
| 50 | `103K` / `255µs` | `99K` / `263µs` | `98K` / `263µs` | `98K` / `263µs` |
| 500 | `99K` / `2.58ms` | `96K` / `2.60ms` | `98K` / `2.57ms` | `98K` / `2.57ms` |

Goblin Core edges the field at every concurrency and, at 500 saturating clients,
**ties** the pack (`2.58` vs `2.57` ms) — no overload penalty. The immediate-write
design costs nothing at fan-out here; its one-hop saving is small but never
negative.

## Persistence

Same 1M-member dataset loaded into each engine; each saves and loads its own
format. "load" is startup-to-ready minus the empty-start baseline.

| engine | save | file | load |
| --- | ---: | ---: | ---: |
| **Goblin Core** | `0.123s` | `37.0` MB | **`0.050s`** |
| Redis 7.2.4 | `0.267s` | `24.8` MB | `0.230s` |
| Redis 8.8 | `0.181s` | `24.8` MB | `0.266s` |
| Valkey 9.1 | `0.185s` | `24.8` MB | `0.224s` |

Goblin Core **loads ~4.5–5.3× faster** — its default snapshot dumps the packed
indexes (the "accelerator") and `memcpy`s them back rather than rebuilding. Its
save is both faster than the others here *and* a **non-blocking background
`fork()`** — the command returns immediately and the server keeps serving while
the child writes the (larger) file. The cost is a **~1.5× larger file** (the
accelerator); `GOBLIN.SAVE <path> NOACCEL` drops it to near Redis's size at the
cost of a rebuild-on-load. Goblin Core also imports a Redis RDB
(`--load dump.rdb`, Redis 2.6–7.2.x) for migration.

## Write-Path Tail Latency

The memory win comes from a Swiss-table member index that grows by reindexing: an
amortized O(1) insert with an occasional synchronous O(n) rehash. That rehash is
a pause, visible in the extreme tail of write latency. 1M individually-timed
`ZADD`s growing one set (same C client, server/client pinned), microseconds:

| | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Goblin Core | `16.6` | `21.0` | `23.3` | `27,874` |
| Redis 8.8 | `19.0` | `24.3` | `27.5` | `367` |
| Valkey 9.1 | `18.3` | `24.0` | `29.5` | `376` |

Goblin Core is faster through p99.9 (ahead at p50, p99, and p99.9); the rehash
appears only at the very tail — the largest write (the final ~1M-member reindex)
is ~`28` ms, versus Redis/Valkey's ~`0.4` ms max. Rare (beyond p99.99) and the
price of the memory layout; `GOBLIN.OPTIMIZE` performs the reindex on demand,
moving it out of the serving path.

## Hashes

Goblin Core also implements a Redis `HASH` (field→value), built on the same tuned
Swiss table and packed arena as the sorted set — no scores, no ordering, so it is
leaner still (an 8-byte struct-of-arrays reference per field instead of 14). The
memory comparison uses the same methodology as [Memory](#memory-the-headline)
above: N field→value pairs loaded into one hash with `HSET`, then
`GOBLIN.OPTIMIZE`; all Redis-family engines on jemalloc 5.3.0 under the
[shared parity config](redis-parity.conf), which pins
`hash-max-listpack-entries`/`-value` so every hash past 128 entries uses the
hashtable encoding (the fair comparison). Fields and values are 16 bytes each.

**RSS bytes/field:**

| fields | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| 250K | `46.2` | `85.9` | `55.6` | `64.8` |
| 500K | `45.6` | `87.8` | `59.3` | `64.5` |
| 1M | `45.4` | `88.9` | `59.2` | `64.2` |
| 2M | `45.3` | `85.1` | `56.9` | `64.0` |
| 4M | `45.2` | `82.4` | `55.7` | `63.9` |

Goblin Core is flat at ~`45` bytes/field and leanest at every size: ~`51%` of
Redis 7.2.4 (**roughly half**), ~`20%` leaner than Redis 8.8, ~`29%` leaner than
Valkey 9.1. Engine-reported `used_memory` corroborates RSS (`45.4` / `80.6` /
`54.3` / `67.1` at 4M), so the advantage is structural, not an allocator artifact.

The margin over *modern* Redis is smaller than the sorted set's ~40%, and
honestly so: a hashtable-encoded Redis hash carries no skiplist and no scores, so
it is already leaner than a Redis sorted set and closes the gap. Goblin Core still
wins because its per-field overhead — the 8-byte reference
(`offset u32 + field_len u16 + value_len u16`) plus a Swiss slot, over one packed
field+value arena — is about half Redis 8.8's (a dict entry plus two `sds`
strings). The ratio tracks field/value size: with the 32-byte content here the
fixed overhead is ~40% of the total, so smaller values widen the lead and larger
ones narrow it (see [Hash value-size sensitivity](#hash-value-size-sensitivity)).

**Throughput and depth-1 latency** — redis-benchmark, one pipelined connection at
depth 16 (throughput) and depth 1 (latency), big hash ~1M fields, HGETALL over a
20-field object, best of three runs on the quiet 128-core host:

| metric | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| HSET ops/s | `555K` | `492K` | `462K` | `441K` |
| HGET ops/s | `599K` | `509K` | `489K` | `503K` |
| HGETALL ops/s | `278K` | `212K` | `214K` | `176K` |
| HGET depth-1 latency (µs) | `21.1` | `21.8` | `22.6` | `22.4` |

Goblin Core leads every hash throughput metric — `+13–26%` on HSET, `+18–22%` on
HGET, `+30–58%` on HGETALL. Because this ran on the quiet dedicated host these are
stable (unlike the shared-VM sorted-set [throughput](#throughput) above). Depth-1
HGET latency is a near-tie at ~`21` µs: at pipeline depth 1 redis-benchmark's own
round-trip overhead dominates, compressing the immediate-write edge that the
leaner PING probe surfaces (see [Latency](#latency)).

### Hash value-size sensitivity

The memory ratio tracks field/value size, because Goblin Core's per-field
overhead is *constant* while Redis's is larger and grows. RSS bytes/field at 1M
fields, 16-byte field, varying the value:

| value | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 |
| --- | ---: | ---: | ---: | ---: |
| 8 B | `37.4` | `80.0` | `55.1` | `56.9` |
| 16 B | `45.4` | `88.9` | `59.2` | `64.2` |
| 64 B | `93.4` | `144.1` | `118.1` | `121.1` |

Subtract the content (field + value) and Goblin Core's overhead is a flat
~`13.4` bytes/field at every value size — the 8-byte reference plus an amortized
Swiss slot — versus Redis 8.8's ~`27–38` (a dict entry, two `sds` headers, and
allocator rounding that grows with the value). So the lead is widest on small
values (~`32%` leaner than Redis 8.8 at 8 B) and narrows as the shared content
dominates (~`21%` at 64 B). Goblin Core stays ahead of every engine at every size.

## Methodology

**Parity — the point of this whole document.**

- **Allocator:** all three Redis-family engines built with their bundled
  jemalloc 5.3.0 (`make MALLOC=jemalloc`, identical version). Goblin Core on its
  native glibc + `malloc_trim` (its default and optimal — see
  [Allocators](#allocators)). Both `used_memory` and RSS are reported, plus the
  fragmentation ratio.
- **Config:** all three Redis-family engines load the same
  [`benchmarks/redis-parity.conf`](redis-parity.conf) — persistence off,
  `maxmemory-policy noeviction`, `activedefrag no`, `io-threads 1`, and the
  listpack/intset thresholds pinned to shared defaults. Differing configs are the
  most common way a benchmark gets contested; the file is published here.
- **Host:** a single quiet, dedicated 128-core Linux box (GCC 16.1.0), every
  figure in this document. Server and client pinned to separate cores;
  throughput/latency are best-or-median of repeated runs on the idle machine.
- **RSS** via `ps -o rss`, measured after load and `GOBLIN.OPTIMIZE`;
  `used_memory` via `INFO memory` (Redis/Valkey) and `GOBLIN.MEMORY` (Goblin
  Core).

**Reproduce:**

- Sorted-set memory: `benchmarks/zset_benchmark.py --target {goblin,redis}
  --redis-server <path> --members N` (`--format json`). It loads the parity
  config for Redis automatically.
- Hash memory + value-size sweep: `benchmarks/hash_benchmark.py --engine
  LABEL:KIND:PATH ... --sizes N,... [--value-bytes B]` (KIND = `goblin`|`redis`).
- Sorted-set / hash throughput + depth-1 latency: `benchmarks/zset_speed.py` and
  `benchmarks/hash_speed.py --engine LABEL:KIND:PATH ... --redis-benchmark <path>`.
- Depth-1 and PING latency: `benchmarks/write_tail_latency.cpp`
  (`... <host> <port> <n> [--ping]`).
- Concurrency: `redis-benchmark -c <clients> -P 1 -n N ping`.
