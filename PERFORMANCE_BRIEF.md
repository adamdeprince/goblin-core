# Goblin Core Performance Brief

This document is an overview of Goblin Core's architecture and performance
characteristics: how sorted sets are stored, where the memory and throughput
wins over Redis come from, and how the benchmarks are run.

## Project Goal

Goblin Core is a source-only C++23 Redis-like server from [Goblin Reactor](https://goblinreactor.com). The
long-term goal is a Redis-compatible server with better memory and performance
characteristics for cost-sensitive deployments. The current implementation is
intentionally narrow: sorted sets plus `PING`, with no persistence,
replication, cluster mode, Lua, transactions, modules, or general Redis key
types.

The design bias is to replace pointer-heavy Redis data structures with compact,
vector-oriented layouts that are friendly to cache locality and compiler
auto-vectorization. SIMD-specific code should be expressed as portable C++ when
possible.

## Supported Command Surface

- `PING [message]`
- `ZCARD key`
- `ZADD key score member [score member ...]`
- `ZRANGE key start stop [WITHSCORES]`
- `ZRANK key member`
- `ZREVRANGE key start stop [WITHSCORES]`
- `ZREVRANK key member`
- `ZREM key member [member ...]`
- `ZSCORE key member`

Plus Goblin-specific maintenance and persistence commands:

- `GOBLIN.MEMORY key`: reports per-zset allocation and the active rank-cache mode.
- `GOBLIN.OPTIMIZE key [density]`: compacts a zset in place and repacks the
  member index to `density` (target load factor in `(0, 1]`, default `0.97`),
  returning the bytes reclaimed.
- `GOBLIN.SAVE path` / `GOBLIN.LOAD path`: write/read a snapshot of all zsets
  (also `--load path` at startup). The format is typed sections, each a small
  instruction bytecode; a section carries a portable canonical layer (members +
  scores) plus a version- and hash-identity-gated accelerator (packed indexes
  dumped so load skips re-hashing/re-sorting). When the accelerator cannot be
  trusted (a changed index format, or a different `std::hash`) the indexes are
  rebuilt from canonical, so snapshots load across builds and architectures.
  Checksums are CRC32C (hardware instruction where available). See
  `include/goblin/core/snapshot.hpp`.
- `GOBLIN.LOAD` / `--load` also import a Redis RDB file (versions 6-11, Redis
  2.6-7.2.x), detected by magic: sorted sets are imported, other types skipped,
  streams/modules rejected. Clean-room from public format descriptions; BSD
  sources only (see `include/goblin/core/rdb.hpp`). This is the migration path.

The server accepts RESP array commands and a basic inline command format used by
tests.

## Current Architecture

- `include/goblin/core/swiss_table.hpp`: portable Swiss-table style hash map.
- `include/goblin/core/zset_member_storage.hpp`: packed member string storage.
- `include/goblin/core/zset_member_index.hpp`: member lookup metadata.
- `include/goblin/core/zset_score_index.hpp`: sorted score/member-id index.
- `include/goblin/core/store.hpp` and `src/store.cpp`: zset/store operations.
- `include/goblin/core/resp_writer.hpp` and `src/resp_writer.cpp`: RESP output.
- `include/goblin/core/command.hpp` and `src/command.cpp`: command dispatch.
- `include/goblin/core/server.hpp` and `src/server.cpp`: nonblocking TCP loop.
- `benchmarks/microbench.cpp`: in-process read-path benchmark.
- `benchmarks/run_benchmarks.py`: Redis comparison benchmark/report driver.

Zsets are not skiplists. Members are stored in packed storage and referenced by
member id. Score order uses a block-oriented sorted index inspired by Python
SortedContainers: inserts/removes touch compact vectors and blocks rather than
skiplist nodes. Lookups use a compact member index instead of general-purpose
node allocation.

## Tunables

- `--rank-cache-mode off`: default. No member-id-to-rank side structure.
- `--rank-cache-mode exact`: packed member-id-to-score-location cache. Faster
  raw `ZRANK` in microbenchmarks, but write/update maintenance can cost more.
- `--rank-cache-mode block-hint`: smaller member-id-to-score-block hint cache.
  It starts with 16-bit block ids and promotes to 32-bit ids if needed.
- `--score-string-cache`: optional RESP-ready score text cache. It is off by
  default; current default workloads prefer direct stack-buffer score formatting.
- `--member-index-growth`: member-index rehash growth factor. Default
  `2^0.25 ≈ 1.19`, which keeps the never-compacted load factor `≥ ~84%` at any
  size (the index never balloons to ~2x just past a power of two) at the cost of
  more frequent rehashes during load. `2.0` is the classic doubling that favors
  write throughput over always-on memory.
- `--max-output-buffer-mib`: per-client queued output limit.
- `--initial-output-buffer-kib`: optional per-client output-buffer reserve.

The member index is a Swiss table sized to a whole number of 16-byte groups
(not the next power of two) and bucketed with a multiply-shift reduction, so its
load factor stays near `31/32` at any member count rather than dropping to ~50%
just past a power of two. `GOBLIN.OPTIMIZE key [density]` repacks it (and the
score index) to a target load factor; the default `0.97` is tight while leaving
empty slots so absent-member lookups terminate quickly. Density `1.0` minimizes
memory but, with no empty slot, makes a lookup of a missing member scan the
whole table — reserve it for read-only sets queried only for present members.

## Current Benchmark Snapshot

**Memory is the story.** After a load-then-`GOBLIN.OPTIMIZE` sequence (the
deployment path), Goblin Core holds a sorted set in roughly `51` RSS bytes per
member versus about `78–83` for Redis 8.8, `84–85` for Valkey 9.1, and `103–110`
for Redis 7.2.4 — **roughly half** the resident set of legacy Redis. Dragonfly is
the nearest competitor at ~`55` B/member, with Goblin Core still about `6–7%`
lower. Throughput is a secondary benefit; Goblin Core also leads the supported
sorted-set operations on one core.

Snapshot host:

- Host: AMD Ryzen Threadripper PRO 5995WX, Ubuntu Linux, GCC 16.1.0.
- Engine set: Goblin Core, Redis 7.2.4, Redis 8.8, Valkey 9.1, Dragonfly v1.39.4.
- Build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

### Memory (the headline)

Resident-set (RSS) delta over baseline:

| Members | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 250K | `52.9` | `103.8` | `79.2` | `85.0` | `56.8` |
| 500K | `52.0` | `107.7` | `82.8` | `84.9` | `55.5` |
| 1M | `51.4` | `109.7` | `82.7` | `84.5` | `55.1` |
| 2M | `51.2` | `106.5` | `80.4` | `84.4` | `54.8` |
| 4M | `51.0` | `103.2` | `78.2` | `84.3` | `54.6` |

Goblin Core's internally tracked zset allocation is `48.4` B/member via
`GOBLIN.MEMORY`; the remaining RSS gap is allocator and process overhead. Members
are stored in a packed arena referenced by a struct-of-arrays entry
(`u32` offset + `u16` length + score), which caps a single member at 64 KiB —
far above any realistic sorted-set member.

The memory sweep uses the scattered 32-bit integer score generator from
`benchmarks/zset_memory.py`: values are in `[0, 2^32)`, and roughly half exceed
signed i32, so Goblin Core stores this dataset at `f64` score width. These are
not narrow-score-only numbers.

### Throughput (secondary)

Measured with `redis-benchmark` (a C load generator), one connection, pipeline
depth 16, 1M-member keyspace. The score field is `__rand_int__` with `-r 1000000`,
so this sweep exercises Goblin Core's signed `i32` score width.

| Operation | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZADD` | `392K` | `235K` | `232K` | `235K` | `251K` |
| `ZSCORE` | `581K` | `480K` | `470K` | `485K` | `406K` |
| `ZRANK` | `476K` | `332K` | `340K` | `353K` | `320K` |
| `ZRANGE` (16) | `433K` | `360K` | `365K` | `339K` | `257K` |
| `ZSCORE` depth-1 latency | `21.3µs` | `21.9µs` | `23.0µs` | `22.4µs` | `22.9µs` |

Methodology note on read throughput: single-member read throughput must be
driven by a C load generator. A single Python pipelined connection is
client-bound near `~350K` ops/sec, so the Python harness measures the client,
not the server, and reports both systems as roughly equal. That is why an
earlier snapshot showed `ZSCORE` at `0.95x` — a client-bound artifact, not a
server result. The current sorted-set speed harness uses `redis-benchmark`;
memory is measured from process RSS and was never affected.

The in-process microbench report was generated on the local macOS arm64
development machine. Treat these values as local baselines, not cross-machine
targets:

Read path (`--members 1000000`, rank cache off, integer scores):

- Multi-key `zscore_rotating` (`--keys 32`): `~51` ns/op.
- `execute_command_into_withscores` (range 16): `~271` ns/op.

Write path (`--members 100000`, rank cache off, integer scores; steady-state
remove benches zrem then immediately zadd-restore):

- `store_zadd_update`: `~34` ns/op.
- `raw_zset_remove`: `~340` ns/op.
- `store_zrem` (+ restore): `~355` ns/op.
- `store_zadd_new`: `~269` ns/op.

Older read-path baselines from the 1M-member report (still valid for rank-cache
comparisons):

- Raw default `ZRANK`: `376.51` ns/op.
- Exact rank-cache raw `ZRANK`: `200.08` ns/op.
- Default `ZRANGE` score-index traversal: `49.48` ns/op.
- Default `ZRANGE` member lookup: `134.46` ns/op.
- Default `ZRANGE` RESP append: `247.54` ns/op.
- Default `ZRANGE WITHSCORES` RESP append: `501.92` ns/op.

Read the generated reports for the full tables:

- [BENCHMARKS.md](BENCHMARKS.md)
- [MICROBENCHMARKS.md](MICROBENCHMARKS.md)

## Known Performance Questions

- `ZREM` remains the dominant write cost in microbenchmarks (~`340` ns/op raw,
  ~`355` ns/op through the store with immediate restore) versus ~`34` ns/op for
  score updates. Further wins must come from fewer score-index operations per
  remove, not auxiliary lookup caches (memory budget is fixed).
- Multi-key writes pay copy-on-write when a shared zset layer is first mutated
  per key; reads benefit from sharing, writes do not.
- `ZRANGE` and `ZRANGE WITHSCORES` are increasingly output/serialization bound.
  Avoid optimizing traversal without checking RESP append and score formatting.
- `ZRANK` raw data-structure cost is still meaningful with rank cache off. The
  exact cache helps raw rank but can hurt write-heavy workloads.
- `ZSCORE` per-command overhead: the RESP parse→dispatch path reuses a
  per-client field-view buffer and an arena-backed field pool, removing the two
  heap allocations that previously occurred per command (~`+3-6%` `ZSCORE`
  measured via `redis-benchmark`). The earlier `0.95x` reading was a
  client-bound Python-harness artifact, not a server deficit. Remaining cost is
  dominated by the random member lookup (a cache miss), not compute.
- Block-hint rank cache needs workload-sensitive validation. It is smaller than
  exact cache but not always faster in end-to-end runs.
- Member lookup is visible inside `ZRANGE`; changes to member storage/index
  locality may matter more than score traversal.
- GCC 16 currently emits missing-field-initializer warnings for designated
  aggregate returns. That is not a performance bug, but it is build-log noise
  on Linux.

## Reproduction Commands

Build and test:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Full Redis comparison:

```sh
python3 benchmarks/run_benchmarks.py \
  --redis-server /path/to/redis-server \
  --report BENCHMARKS.md \
  --name redis-goblin-core-1m-modes \
  --latency-samples 10000
```

Reproduce the memory and `redis-benchmark` throughput snapshot above (uses a C
load generator, pins the server and client to separate cores, and prints RSS
bytes per member plus per-op ratios):

```sh
python3 benchmarks/redis_benchmark_speed.py \
  --goblin-bin build-release/goblin-core \
  --redis-server "$(command -v redis-server)" \
  --members 1000000 --requests 2000000 --rounds 3 \
  --server-cpu 0 --client-cpu 1
```

Fast CI-style smoke:

```sh
scripts/benchmark_smoke.sh
```

In-process microbenchmark:

```sh
cmake --build build-release --target goblin_core_microbench
./build-release/goblin_core_microbench \
  --members 1000000 \
  --ops 1000000 \
  --score-shape integer \
  --format json \
  --output benchmark-results/microbench.json

# Write path only (smaller fixture; destructive benches reset state per run):
./build-release/goblin_core_microbench \
  --members 100000 \
  --ops 100000 \
  --category write_path \
  --format json \
  --output benchmark-results/microbench-write-path.json
```

## Design Principles

Changes keep the supported Redis subset correct and measurable. A data-structure
change identifies which benchmark metric it should improve, which memory metric
might regress, and which test or benchmark validates it. Architecture-specific
intrinsics are avoided unless the same idea cannot be expressed in portable C++
that compilers can auto-vectorize.
