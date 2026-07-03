# Goblin Core

Goblin Core is a C++23 Redis-like server built to hold sorted sets in far less
memory than Redis — about `42%` of Redis's resident set for the same data —
while still matching or beating its throughput. The initial implementation
focuses on sorted sets with a vector-backed layout and a small RESP command
surface.

Goblin Core is licensed under the Apache License, Version 2.0. See `LICENSE` and
`NOTICE`.

## Quick Summary

- Source-only C++23 Redis-like server from Goblin Reactor.
- Current scope: sorted sets plus `PING`, not full Redis compatibility.
- Primary design: vector-backed zset indexes and compact hash/member storage
  instead of pointer-heavy skiplist layouts.
- Memory is the point: Goblin Core holds a sorted set in about `55` bytes per
  member versus Redis at about `130` — roughly `42%` of Redis's resident memory
  — and that ratio is flat from 250K to 4M members (avx10 Intel Linux, Redis
  `8.0.5`). At 4M members it saves about `281` MiB of RSS.
- Throughput is a secondary, nice-to-have win: measured with `redis-benchmark`,
  Goblin Core is `1.31x` `ZSCORE`, `2.27x` `ZRANK`, `2.52x` `ZADD`, and `1.36x`
  `ZRANGE` versus Redis.
- Build locally with CMake; benchmark instructions live in
  [BENCHMARKS.md](BENCHMARKS.md).
- A performance-oriented project brief for outside review lives in
  [PERFORMANCE_BRIEF.md](PERFORMANCE_BRIEF.md).

## Current Commands

- `PING [message]`
- `ZCARD key`
- `ZADD key score member [score member ...]`
- `ZRANGE key start stop [WITHSCORES]`
- `ZRANK key member`
- `ZREVRANGE key start stop [WITHSCORES]`
- `ZREVRANK key member`
- `ZREM key member [member ...]`
- `ZSCORE key member`

The protocol handler accepts RESP array commands and a basic inline command
format for local testing.

## Compatibility Scope

Goblin Core is not a full Redis replacement yet. This release is scoped to the
sorted-set command surface above plus `PING` for liveness checks. It does not
implement persistence, replication, cluster mode, pub/sub, Lua, transactions,
ACLs, Redis modules, eviction policies, or general Redis key types.

Use the Redis differential tests and benchmark scripts when changing command
behavior. The goal is to keep the supported subset boringly compatible while
leaving room to optimize internal layouts aggressively.

## Build From Source

Goblin Core is distributed as source at this stage. Build it locally with CMake
and a C++23 compiler.

Prerequisites:

- CMake 3.25 or newer
- A C++23 compiler such as recent Clang, GCC, or MSVC
- Python 3 for tests and generated HTML docs
- Redis only when running Redis differential tests or Redis comparison benchmarks

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

Redis differential tests are optional because they start local Goblin Core and
Redis processes. The test script is deterministic: pass the same seed,
pipeline depth, and workload arguments to reproduce a failure.

```sh
cmake -S . -B build-redis-tests -DGOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS=ON
cmake --build build-redis-tests
ctest --test-dir build-redis-tests --output-on-failure
```

Run the pipelined differential test directly:

```sh
python3 tests/redis_differential.py \
  --goblin-bin build-release/goblin-core \
  --pipeline-depth 32 \
  --seed 12345
```

Install into a prefix:

```sh
cmake --install build-release --prefix /usr/local
```

## Run

```sh
./build-release/goblin-core --port 6379
```

`--rank-cache` enables the exact member-id-to-score-location cache for faster
`ZRANK` lookups at roughly 4 bytes per member. It is off by default because it
adds update/remove maintenance work. `--rank-cache-mode off|exact|block-hint`
selects the cache explicitly; `block-hint` stores only member-to-score-block
hints, trading some `ZRANK` read speed for much lower write maintenance.
Block hints start as 16-bit ids for lower memory and promote to 32-bit ids
automatically if a larger block-id space is needed.
`GOBLIN.MEMORY <key>` reports the active mode as `rank_cache_mode`.

`--score-string-cache` enables an experimental RESP-ready score text cache for
range output benchmarking. It is off by default because it adds a packed side
arena and an 8-byte score-text reference per member; measured default workloads
prefer direct stack-buffer score serialization.

`--max-output-buffer-mib` bounds per-client queued response bytes before the
server pauses reads from that socket. The default is `1`; `0` disables the
limit for comparison runs. Reads resume after queued output drains below one
quarter of the configured limit.

`--initial-output-buffer-kib` reserves per-client response-buffer capacity at
accept time. The default is `0`; keep it off unless a cold first-burst workload
benefits enough to justify the per-connection memory.

Example:

```sh
redis-cli -p 6379 ZADD leaders 42 alice 17 bob
redis-cli -p 6379 ZRANGE leaders 0 -1 WITHSCORES
```

## Benchmark

Memory is the headline: Goblin Core stores a sorted set in about `55` RSS bytes
per member versus Redis at about `130` — roughly `42%` of Redis's resident
memory — consistently from 250K to 4M members. Throughput is a secondary
benefit; measured with `redis-benchmark` (a single Python client is client-bound
and understates both servers), Goblin Core is faster than Redis on every
supported operation, for example `1.31x` `ZSCORE` and `2.27x` `ZRANK`.

See the [benchmark report](BENCHMARKS.md) for full results, methodology, and
reproducible benchmark commands.

## Source Releases

Goblin Core releases are source-only for now. A release consists of the git tag
and the source archive generated by the hosting service. Do not publish compiled
`.tar.gz`, `.zip`, Homebrew, `.deb`, or `.rpm` artifacts until the operational
contract is broader.

Users build the server from a release checkout:

```sh
git clone https://github.com/adamdeprince/goblin-core.git
cd goblin-core
git checkout v0.1.0
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
cmake --install build-release --prefix /usr/local
```

Use the [release checklist](RELEASE.md) before publishing a tag.

## HTML Docs

The build converts root Markdown docs into static HTML under `html/`.
`README.md` becomes `html/index.html`, and Markdown links between docs are
rewritten to HTML links with human-visible labels that do not end in `.md`.

```sh
python3 scripts/build_html_docs.py --output html
```

The `html/` directory is generated and ignored. If a binary asset under `html/`
is intentionally force-added later, `.gitattributes` routes it through Git LFS.

## Layout

- `include/goblin/core/resp_parser.hpp`: incremental RESP and inline parser
- `include/goblin/core/resp_writer.hpp`: RESP response serialization
- `include/goblin/core/command.hpp`: command parsing and dispatch
- `include/goblin/core/store.hpp`: vector-backed sorted set store
- `include/goblin/core/chunked_sorted_list.hpp`: block-based sorted-list index for zsets
- `include/goblin/core/swiss_table.hpp`: portable Swiss-table hash map
- `include/goblin/core/zset_member_index.hpp`: packed member lookup index for zsets
- `include/goblin/core/zset_score_index.hpp`: packed score/member-id index for zsets
- `include/goblin/core/server.hpp`: nonblocking TCP server
- `include/goblin/core/simd.hpp`: SIMD capability scaffolding
- `scripts/benchmark_smoke.sh`: CI-style benchmark smoke runner
- `scripts/build_html_docs.py`: Markdown to HTML documentation generator
- `tests/redis_differential.py`: Redis-backed zset differential test
- `benchmarks/run_benchmarks.py`: scripted build/test/benchmark/report workflow
- `benchmarks/zset_benchmark.py`: Goblin Core vs Redis zset benchmark
- `PERFORMANCE_BRIEF.md`: architecture and benchmark context for performance review
