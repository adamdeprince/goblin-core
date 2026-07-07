# Benchmarks

Goblin Core versus Redis 7.2.4, Redis 8.8, Valkey 9.1, and Dragonfly, with an
emphasis on memory. Every number below was measured under **allocator and
configuration parity** (see [Methodology](#methodology)) so the only variable is
the engine — and, for Goblin Core, the data structure.

| Engine | Version | Serving threads | Allocator |
| --- | --- | ---: | --- |
| Goblin Core | `0.4.0` | 1 | glibc + `malloc_trim` |
| Redis | `7.2.4` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Redis | `8.8.0` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Valkey | `9.1.0` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Dragonfly | `v1.39.4` (`bc82e529`) | 1 (`--proactor_threads=1`) | mimalloc `2.2.4` |

Each engine runs the allocator it ships with and is tuned for: the three
Redis-family engines on their bundled jemalloc 5.3.0 under a
[shared config](redis-parity.conf), Goblin Core on its native glibc +
`malloc_trim` (see [Allocators](#allocators)), Dragonfly on its bundled mimalloc.
Dragonfly is multi-threaded by design; it is run with a single proactor
(`--proactor_threads=1`) so it is measured on the same one-core budget as the
single-threaded engines, and it manages its own CPU affinity, so it is tested as
shipped. Host: a quiet, dedicated 128-core Linux box (GCC 16.1.0); server and
client on separate cores.

License note: **Redis 7.2.4** is BSD-3-Clause (the last open-source Redis) and is
the format Goblin Core imports; **Redis 8.8** is RSALv2/SSPL (source-available);
**Valkey 9.1** is BSD-3-Clause (the community fork); **Dragonfly** is BSL 1.1
(source-available).

## Memory (the headline)

RSS per member after a load-then-`GOBLIN.OPTIMIZE` sequence, across a size sweep.
Goblin Core is flat and holds a sorted set in **roughly half** the resident set
of legacy Redis, and comfortably under every modern engine.

**RSS bytes/member:**

| members | Goblin (glibc) | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 250K | `52.2` | `103.8` | `79.1` | `85.0` | `56.8` |
| 500K | `51.4` | `107.6` | `82.8` | `84.9` | `55.5` |
| 1M | `51.0` | `109.7` | `82.6` | `84.5` | `55.0` |
| 2M | `50.8` | `106.5` | `80.4` | `84.3` | `54.8` |
| 4M | `50.9` | `103.2` | `78.2` | `84.3` | `54.6` |

**Goblin Core RSS as a fraction of each engine:** ~`48%` of Redis 7.2.4, ~`63%`
of Redis 8.8, ~`61%` of Valkey 9.1, and ~`93%` of Dragonfly — stable at every
size. So Goblin Core is **roughly half** of legacy Redis and ~`37%` leaner than
Redis 8.8. Dragonfly is the closest competitor: its dashtable holds a sorted set
in ~`55` B/member — well under the Redis family — but still a step (~`7%`) behind
Goblin Core's ~`51`.

**Fragmentation (RSS / used_memory):** Goblin Core `1.1–1.3`, Redis/Valkey
`1.0–1.5`, both tightening toward `1.0` at scale. Dragonfly's ratio is inflated at
small N by mimalloc's pre-reserved arenas (~`2.7` at 250K) but amortizes to ~`1.1`
by 4M — which is why the per-member table above, measured as an RSS *delta* over
the empty-server baseline, is the fair read of its footprint. Goblin Core's
`used_memory` — its actual structure bytes — holds at ~`49` B/member; `malloc_trim`
after `OPTIMIZE` keeps the resident set a hair behind.

The advantage is structural — a handful of big contiguous allocations (the
member-bytes arena, the Swiss index, the score-index blocks, the struct-of-arrays
offset/length/score columns) instead of a skiplist node + dict entry + string per
member. It does not depend on the allocator (see below).

## Allocators

A memory comparison is only fair if the allocator is held constant, because an
engine's footprint depends heavily on it. So the three Redis-family engines run
their bundled **jemalloc 5.3.0** (identical version), Goblin Core its default
**glibc + `malloc_trim`**, and Dragonfly its bundled **mimalloc 2.2.4** — each
engine on the allocator it ships with and is tuned for.

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

| operation | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| ZADD (write) | `398K` | `239K` | `242K` | `242K` | `262K` |
| ZSCORE (point read) | `587K` | `504K` | `488K` | `495K` | `419K` |
| ZRANK | `491K` | `331K` | `342K` | `354K` | `329K` |
| ZRANGE (16 elems) | `456K` | `365K` | `368K` | `346K` | `268K` |
| ZSCORE depth-1 latency (µs) | `21.3` | `22.2` | `23.0` | `22.4` | `22.4` |

The standout is `ZADD` at **~`+52–67%`** over the field (`398K` vs `239–262K`) —
the immediate per-reply `write()` plus the packed Swiss member index make inserts
cheap — and `ZRANK` at **`+39–49%`**. Point reads (`ZSCORE`) lead by `+16–40%` and
16-element `ZRANGE` by `+24–70%`. Depth-1 latency is a near-tie at ~`21–23` µs
(redis-benchmark's own round-trip floor).

At one core Dragonfly is the fastest of the rest on `ZADD` (`262K`, above the Redis
family's ~`242K`) but the slowest on reads — `ZSCORE` `419K` and `ZRANGE` `268K`
are the lowest here. That is the expected profile of an engine built to shard
across many proactor threads, measured on one.

## Latency

Goblin Core writes each reply the instant it is ready; Redis and Valkey defer
replies to the event-loop boundary. That saves one event-loop hop per reply — a
small, consistent edge, with no penalty at high fan-out.

**Depth-1 round trip** — a lean C client (`write_tail_latency --ping`), one
connection, no pipelining, server and client pinned to separate cores, median of
1M PINGs:

| | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| PING p50 (µs) | `15.8` | `16.7` | `19.3` | `19.0` | `18.2` |
| PING p99 (µs) | `19.9` | `21.1` | `25.4` | `25.2` | `23.3` |

Goblin Core is fastest — ~`1` µs over Redis 7.2.4 and ~`3` µs (≈`15%`) over the
modern engines — the single deferred event-loop hop its immediate `write()`
avoids. Dragonfly sits in the middle (`18.2` µs), ahead of Redis 8.8/Valkey but
behind Goblin Core and legacy Redis. A localhost round trip is syscall-floor
dominated, so this is a modest edge, not a rout — but a consistent one.

**PING throughput / p50 by client count** (`redis-benchmark -P 1`; the absolute
p50 here carries redis-benchmark's own client overhead, so read it for *scaling*,
not the floor above):

| clients | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | `50K` / `23µs` | `48K` / `23µs` | `41K` / `23µs` | `40K` / `23µs` | `44K` / `23µs` |
| 50 | `103K` / `255µs` | `99K` / `263µs` | `91K` / `287µs` | `86K` / `295µs` | `89K` / `295µs` |
| 500 | `99K` / `2.57ms` | `91K` / `2.75ms` | `82K` / `3.05ms` | `85K` / `2.94ms` | `86K` / `2.90ms` |

Goblin Core leads at every concurrency and widens at 500 saturating clients
(`99K` ops / `2.57` ms versus the field's `82–91K` / `2.75–3.05` ms) — no overload
penalty. Dragonfly and the modern Redis engines land within a few percent of each
other at fan-out; the immediate-write design keeps Goblin Core ahead throughout.

## Persistence

Same 1M-member dataset loaded into each engine; each saves and loads its own
format. "load" is startup-to-ready minus the empty-start baseline.

| engine | save | file | load |
| --- | ---: | ---: | ---: |
| **Goblin Core** | `0.243s` | `37.0` MB | **`0.150s`** |
| Redis 7.2.4 | `0.358s` | `24.8` MB | `0.326s` |
| Redis 8.8 | `0.345s` | `24.8` MB | `0.377s` |
| Valkey 9.1 | `0.236s` | `24.8` MB | `0.348s` |
| Dragonfly | `0.258s` | `24.8` MB | `0.304s` |

Goblin Core **loads ~2–2.5× faster** than the field — its default snapshot dumps
the packed indexes (the "accelerator") and `memcpy`s them back rather than
rebuilding; Dragonfly loads next-fastest (`0.304s`). Goblin Core's save is also a
**non-blocking background `fork()`** — the command returns immediately and the
server keeps serving while the child writes the file; the others' `SAVE` blocks.
The cost is a **~1.5× larger file** (the accelerator) — Redis, Valkey, and
Dragonfly all land at ~`24.8` MB (Dragonfly's LZ4 `.dfs` matches here only because
these members are unique and incompressible); `GOBLIN.SAVE <path> NOACCEL` drops
Goblin's file to near that size at the cost of a rebuild-on-load. Goblin Core also
imports a Redis RDB (`--load dump.rdb`, Redis 2.6–7.2.x) for migration.

## Write-Path Tail Latency

The memory win comes from a Swiss-table member index that grows by reindexing: an
amortized O(1) insert with an occasional synchronous O(n) rehash. That rehash is
a pause, visible in the extreme tail of write latency. 1M individually-timed
`ZADD`s growing one set (same C client, server/client pinned), microseconds:

| | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Goblin Core | `16.6` | `21.3` | `23.7` | `31,810` |
| Redis 7.2.4 | `20.6` | `28.3` | `43.4` | `4,186` |
| Redis 8.8 | `21.3` | `29.2` | `42.7` | `748` |
| Valkey 9.1 | `20.7` | `29.1` | `41.8` | `543` |
| Dragonfly | `20.9` | `30.4` | `46.8` | `51,954` |

Goblin Core is faster through p99.9 — ahead at p50, p99, and p99.9 by a clear
margin — but its tail max is the Swiss-table rehash: the largest write (the final
~1M-member reindex) is ~`32` ms. Dragonfly's tail is fatter still (~`52` ms max),
from its own background fiber scheduling; the Redis family, by contrast, holds a
sub-millisecond-to-few-ms max. So Goblin Core wins the body of the distribution
and pays for it only at the extreme tail — rare (beyond p99.99) and the price of
the packed layout; `GOBLIN.OPTIMIZE` performs the reindex on demand, moving it out
of the serving path.

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

| fields | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 250K | `46.2` | `85.9` | `55.6` | `64.8` | `77.9` |
| 500K | `45.6` | `87.8` | `59.3` | `64.6` | `77.1` |
| 1M | `45.4` | `88.9` | `59.2` | `64.2` | `76.7` |
| 2M | `45.3` | `85.1` | `56.9` | `64.0` | `76.5` |
| 4M | `45.2` | `82.4` | `55.7` | `63.9` | `76.4` |

Goblin Core is flat at ~`45` bytes/field and leanest at every size: ~`51%` of
Redis 7.2.4 (**roughly half**), ~`20%` leaner than Redis 8.8, ~`29%` leaner than
Valkey 9.1, ~`41%` leaner than Dragonfly. Engine-reported `used_memory`
corroborates RSS (Goblin `45.4`, Redis 7.2.4 `80.6`, Redis 8.8 `54.3`, Valkey
`67.1`, Dragonfly `76.1` at 4M), so the advantage is structural, not an allocator
artifact.

Note the reversal from sorted sets: Dragonfly, the leanest of the rest on zsets,
is the **second-heaviest** on hashes (~`76` B/field, above Redis 8.8 and Valkey) —
its dashtable is tuned for the sorted-set case and its hash overhead does not
amortize the same way. The margin over *modern* Redis is smaller than the sorted
set's ~40%, and honestly so: a hashtable-encoded Redis hash carries no skiplist
and no scores, so it is already leaner than a Redis sorted set. Goblin Core still
wins because its per-field overhead — the 8-byte reference
(`offset u32 + field_len u16 + value_len u16`) plus a Swiss slot, over one packed
field+value arena — is about half Redis 8.8's (a dict entry plus two `sds`
strings). The ratio tracks field/value size: with the 32-byte content here the
fixed overhead is ~40% of the total, so smaller values widen the lead and larger
ones narrow it (see [Hash value-size sensitivity](#hash-value-size-sensitivity)).

**Throughput and depth-1 latency** — redis-benchmark, one pipelined connection at
depth 16 (throughput) and depth 1 (latency), big hash ~1M fields, HGETALL over a
20-field object, best of three runs on the quiet 128-core host:

| metric | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| HSET ops/s | `563K` | `480K` | `454K` | `451K` | `357K` |
| HGET ops/s | `591K` | `501K` | `480K` | `498K` | `393K` |
| HGETALL ops/s | `276K` | `212K` | `213K` | `172K` | `171K` |
| HGET depth-1 latency (µs) | `21.1` | `22.4` | `23.0` | `22.7` | `22.6` |

Goblin Core leads every hash throughput metric — `+17–58%` on HSET, `+18–50%` on
HGET, `+30–61%` on HGETALL. Dragonfly is the slowest of the field on hashes at one
core (`357K` HSET, `393K` HGET), consistent with its heavier hash representation.
Depth-1 HGET latency is a near-tie at ~`21–23` µs: at pipeline depth 1
redis-benchmark's own round-trip overhead dominates, compressing the
immediate-write edge that the leaner PING probe surfaces (see [Latency](#latency)).

### Hash value-size sensitivity

The memory ratio tracks field/value size, because Goblin Core's per-field
overhead is *constant* while Redis's is larger and grows. RSS bytes/field at 1M
fields, 16-byte field, varying the value:

| value | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8 B | `37.4` | `80.0` | `55.6` | `56.9` | `60.7` |
| 16 B | `45.4` | `88.9` | `59.2` | `64.2` | `76.7` |
| 64 B | `93.4` | `144.1` | `119.2` | `121.1` | `126.7` |

Subtract the content (field + value) and Goblin Core's overhead is a flat
~`13.4` bytes/field at every value size — the 8-byte reference plus an amortized
Swiss slot — versus Redis 8.8's ~`27–38` and Dragonfly's ~`37–47` (both grow with
the value: a dict/dashtable entry, string headers, and allocator rounding). So the
lead is widest on small values (~`32%` leaner than Redis 8.8, ~`38%` than Dragonfly
at 8 B) and narrows as the shared content dominates (~`21%` and ~`26%` at 64 B).
Goblin Core stays ahead of every engine at every size.

## Command parsing

Command dispatch is a gperf perfect hash: the name is upper-cased into a 16-byte
buffer and looked up in O(1), replacing the former 25-way linear `equals_ci` chain
where a late command (`GOBLIN.LOAD`) paid for every earlier comparison.
`parse_command` is now flat regardless of which command arrives — ~`23–28` ns/op
(`goblin_core_microbench --category resp_parse`, `dispatch`), independent of the
command's former position in the chain.

SIMD kernels for the inline tokenizer and the name upper-caser (SSE4.2 / AVX2 /
AVX512 / NEON / LoongArch LASX+LSX) were prototyped and measured on real hardware,
then removed — they didn't earn their place. Real command frames are short (~30 B),
so a line can't fill a 32- or 64-byte vector and the wide lanes fall to the scalar
tail plus setup overhead: on identical hardware SSE4.2 (16 B) beat AVX512 (64 B) at
tokenizing, and on a LoongArch 3A6000 plain scalar beat every SIMD lane. Since real
clients send length-framed RESP arrays (parsed scalar anyway) and inline frames are
short, SIMD bought nothing on typical traffic. The perfect hash — the actual,
portable, ISA-independent win — stays.

## Methodology

**Parity — the point of this whole document.**

- **Allocator:** the three Redis-family engines built with their bundled
  jemalloc 5.3.0 (`make MALLOC=jemalloc`, identical version), Goblin Core on its
  native glibc + `malloc_trim`, Dragonfly on its bundled mimalloc 2.2.4 — each on
  the allocator it ships with and is tuned for (see [Allocators](#allocators)).
  Both `used_memory` and RSS are reported, plus the fragmentation ratio.
- **Config:** the three Redis-family engines load the same
  [`benchmarks/redis-parity.conf`](redis-parity.conf) — persistence off,
  `maxmemory-policy noeviction`, `activedefrag no`, `io-threads 1` (one serving
  thread), and the listpack/intset thresholds pinned to shared defaults. Dragonfly
  is multi-threaded by design and is run with `--proactor_threads=1` — a single
  serving shard, the one-core comparison — snapshotting off, eviction disabled; it
  manages its own CPU affinity, so it is tested as shipped (no external pinning).
  Differing configs are the most common way a benchmark gets contested; the file
  is published here.
- **Host:** a single quiet, dedicated 128-core Linux box (GCC 16.1.0), every
  figure in this document. Server and client pinned to separate cores;
  throughput/latency are best-or-median of repeated runs on the idle machine.
- **RSS** via `ps -o rss`, measured after load and `GOBLIN.OPTIMIZE`;
  `used_memory` via `INFO memory` (Redis/Valkey) and `GOBLIN.MEMORY` (Goblin
  Core).

**Reproduce** — every driver takes repeatable `--engine LABEL:KIND:PATH`, where
KIND is `goblin` | `redis` | `dragonfly` (`redis` covers both Redis and Valkey,
which share the server flags):

- Sorted-set / hash memory + value-size: `benchmarks/zset_memory.py` and
  `benchmarks/hash_benchmark.py --engine ... --sizes N,... [--value-bytes B]`.
- Sorted-set / hash throughput + depth-1 latency: `benchmarks/zset_speed.py` and
  `benchmarks/hash_speed.py --engine ... --redis-benchmark <path>`.
- PING latency, concurrency, and write-path tail: `benchmarks/lat_tail.py
  --engine ... --probe ./write_tail_latency --redis-benchmark <path>` (build the
  probe with `c++ -O2 -std=c++20 -o write_tail_latency
  benchmarks/write_tail_latency.cpp`).
- Persistence: `benchmarks/persistence.py --engine ... --members N`.

Example engine set: `--engine goblin:goblin:build-release/goblin-core --engine
redis724:redis:<redis-7.2.4>/src/redis-server --engine
redis88:redis:<redis-8.8>/src/redis-server --engine
valkey:redis:<valkey>/src/valkey-server --engine
dragonfly:dragonfly:$(command -v dragonfly)`.
