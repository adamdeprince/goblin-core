# Goblin Core Performance Brief

This document is an overview of Goblin Core's architecture and performance
characteristics: how sorted sets are stored, where the memory and throughput
wins over Redis come from, and how the benchmarks are run.

## Project Goal

Goblin Core is a source-only C++23 Redis-like server from Goblin Reactor. The
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
deployment path), Goblin Core holds a sorted set in roughly `49` bytes per
member versus about `80` for Redis 8.8, `84` for Valkey 9.1, and `110` for Redis
7.2.4 — **roughly half** the resident set, every engine measured on the same
jemalloc `5.3.0`, flat from 250K to 4M members, and flat even at counts just past
a power of two (the non-pow2 member index removes the boundary blowup). Throughput is a secondary benefit; Goblin Core also
happens to be faster than Redis on the supported operations.

Snapshot host (the deployment-relevant environment):

- Host: Ubuntu 26.04 LTS, kernel `7.0.0-1004-aws`, Intel Xeon 6975P-C, 4 vCPU (AWS).
- Redis: `8.0.5` with jemalloc `5.3.0`. Python: `3.14.4`.
- Build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

### Memory (the headline)

Resident-set (RSS) delta over baseline, with exact loaded member counts:

| Members | Goblin Core B/member | Redis B/member | Goblin Core / Redis |
| ---: | ---: | ---: | ---: |
| 250K | `49.5` | `131.0` | `37.8%` |
| 1M | `49.1` | `133.2` | `36.9%` |
| 1.08M (just past 2^20) | `49.1` | `147.8` | `33.2%` |
| 4M | `48.8` | `128.8` | `37.9%` |

Goblin Core's internally tracked zset allocation (`~49` B/member via
`GOBLIN.MEMORY`) is within ~2% of its RSS delta — almost no allocator slack. At
4M members Goblin Core's resident set is about `305` MiB smaller than Redis's,
and the gap grows linearly with member count. Members are stored in a packed
arena referenced by a 14-byte struct-of-arrays entry (`u32` offset + `u16`
length + `f64` score), which caps a single member at 64 KiB — far above any
realistic sorted-set member.

These figures are post-`GOBLIN.OPTIMIZE` (the benchmark repacks at density
`0.97` before measuring, matching load-then-serve). The member index is `5.2`
B/member at every size measured here, including the 1.08M count just past `2^20`
(a power-of-two-sized table would have been ~9.7 there) — the win compaction
alone could not deliver before the non-pow2 index, since it rebuilt at the same
power of two.

### Throughput (secondary)

Measured with `redis-benchmark` (a C load generator; see the methodology note)
against a loaded 1M-member set after `GOBLIN.OPTIMIZE` (the deployment path),
pinned server/client, `-c 1 -P 256`, median of 3 rounds. Absolute throughput is
host-dependent; the ratio is the stable takeaway:

| Operation | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZSCORE` | `2,083,467` | `1,602,667` | `1.30x` |
| `ZRANK` | `1,362,485` | `671,635` | `2.03x` |
| `ZREVRANK` | `1,354,183` | `675,035` | `2.01x` |
| `ZADD` (score update) | `840,743` | `376,388` | `2.23x` |
| `ZRANGE` (16) | `1,004,080` | `686,621` | `1.46x` |
| `ZRANGE WITHSCORES` (16) | `607,757` | `455,506` | `1.33x` |

Methodology note on read throughput: single-member read throughput must be
driven by a C load generator. A single Python pipelined connection is
client-bound near `~350K` ops/sec, so the Python harness measures the client,
not the server, and reports both systems as roughly equal. That is why an
earlier snapshot showed `ZSCORE` at `0.95x` — a client-bound artifact, not a
server result. `benchmarks/zset_benchmark.py` now drives `ZSCORE`/`ZRANK`/
`ZREVRANK` through `redis-benchmark`; memory is measured from process RSS and
was never affected.

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
