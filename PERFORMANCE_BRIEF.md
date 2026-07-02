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

Absolute rates and `ns/op` timings are hardware-specific. The snapshot below is
from the checked-in reports generated on the local macOS arm64 development
machine:

- Host recorded by `BENCHMARKS.md`: `macOS-26.5.1-arm64-arm-64bit-Mach-O arm64`.
- Python: `3.14.5`.
- Build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`.

The checked-in 1M-member server benchmark reports the default mode at:

- Goblin Core RSS delta: `67.37` bytes per loaded member.
- Redis RSS delta: `92.65` bytes per loaded member.
- `ZADD`: Goblin Core `945,957` members/sec, Redis `547,592`.
- `ZRANK`: Goblin Core `461,684` ops/sec, Redis `319,609`.
- `ZRANGE`: Goblin Core `77,416` ops/sec, Redis `66,855`.
- `ZREM`: Goblin Core `1,068,245` members/sec, Redis `775,862`.

The in-process microbench report was generated on the same local development
machine. Treat these values as local baselines, not cross-machine targets:

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
- `ZSCORE` has command/dispatch overhead over raw lookup. Look for avoidable
  allocations, string copies, or redundant parsing work before changing data
  structures.
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
