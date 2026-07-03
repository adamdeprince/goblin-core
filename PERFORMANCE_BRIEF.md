# Goblin Core Performance Brief

This document is written for reviewers, including LLMs, who want to look for
performance wins in Goblin Core without first reconstructing the project
history.

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

Plus two Goblin-specific maintenance commands:

- `GOBLIN.MEMORY key`: reports per-zset allocation and the active rank-cache mode.
- `GOBLIN.OPTIMIZE key`: compacts a zset in place to reclaim insertion slack,
  returning the bytes reclaimed.

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
- `--max-output-buffer-mib`: per-client queued output limit.
- `--initial-output-buffer-kib`: optional per-client output-buffer reserve.

## Current Benchmark Snapshot

**Memory is the story.** Goblin Core holds a sorted set in roughly `53` bytes
per member versus Redis at roughly `130`, i.e. about `40%` of Redis's resident
memory for the same data — and that ratio is flat from 250K to 4M members.
Throughput is a secondary, nice-to-have benefit; Goblin Core also happens to be
faster than Redis on the supported operations.

Snapshot host (the deployment-relevant environment):

- Host: `Linux-7.0.0-1004-aws-x86_64`, Intel Xeon 6975P-C, 4 vCPU on AWS/KVM.
- Redis: `8.0.5` with jemalloc `5.3.0`. Python: `3.14.4`.
- Build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

### Memory (the headline)

Resident-set (RSS) delta over baseline, with exact loaded member counts:

| Members | Goblin Core B/member | Redis B/member | Goblin Core / Redis |
| ---: | ---: | ---: | ---: |
| 250K | `53.2` | `132.0` | `40.3%` |
| 1M | `52.6` | `133.2` | `39.5%` |
| 4M | `52.5` | `128.8` | `40.7%` |

Goblin Core's internally tracked zset allocation (`~53` B/member via
`GOBLIN.MEMORY`) is within ~2% of its RSS delta — almost no allocator slack. At
4M members Goblin Core's resident set is about `291` MiB smaller than Redis's,
and the gap grows linearly with member count.

`GOBLIN.OPTIMIZE <key>` compacts a zset in place to reclaim insertion slack
(score-index block capacity and geometric vector over-allocation), returning the
bytes reclaimed. It takes a favorable-sized 1M set from `~53` to `~50`
B/member and, more importantly, rescues sets that land just past a power-of-two
boundary (where the ref vector alone can double); run it on read-mostly sets
after loading.

### Throughput (secondary)

Measured with `redis-benchmark` (a C load generator; see the methodology note)
against a loaded 1M-member set, pinned server/client, `-c 1 -P 256`:

| Operation | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZSCORE` | `1,347,795` | `1,032,120` | `1.31x` |
| `ZRANK` | `900,958` | `396,256` | `2.27x` |
| `ZREVRANK` | `893,712` | `405,900` | `2.20x` |
| `ZADD` (score update) | `554,512` | `220,180` | `2.52x` |
| `ZRANGE` (16) | `961,600` | `709,392` | `1.36x` |
| `ZRANGE WITHSCORES` (16) | `601,361` | `414,898` | `1.45x` |

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
  aggregate returns. That is not a performance bug, but it is noise for Linux
  reviewers.

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
```

## Review Guidance

Prefer changes that keep the supported Redis subset correct and measurable.
Before proposing a data-structure rewrite, identify which benchmark metric
should improve, which memory metric might regress, and which test or benchmark
would validate the change. Avoid adding architecture-specific intrinsics unless
the same idea cannot be represented in portable C++ that compilers can
auto-vectorize.
