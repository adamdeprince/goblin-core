# Goblin Core

Goblin Core is a Redis-like server built to hold sorted sets in far less
memory than Redis — about `37%` of Redis's resident set for the same data —
while beating its throughput. The initial implementation
focuses on sorted sets with a vector-backed layout and a small RESP command
surface.

> **Alpha.** Goblin Core is alpha-stage, pre-1.0 software under active
> development. Expect breaking changes, and validate it against your workload
> before relying on it.

Goblin Core is licensed under the Apache License, Version 2.0. See
[LICENSE](https://github.com/adamdeprince/goblin-core/blob/main/LICENSE) and
[NOTICE](https://github.com/adamdeprince/goblin-core/blob/main/NOTICE).

Source: [github.com/adamdeprince/goblin-core](https://github.com/adamdeprince/goblin-core)

## Quick Summary

- Source-only C++23 Redis-like server from Goblin Reactor.
- Current scope: sorted sets plus `PING`, not full Redis compatibility.
- Primary design: vector-backed zset indexes and compact hash/member storage
  instead of pointer-heavy skiplist layouts.
- Memory is the point: after a load-then-`GOBLIN.OPTIMIZE` sequence, Goblin Core
  holds a sorted set in about `49` bytes per member versus Redis at about `130`
  — roughly `37%` of Redis's resident memory — flat from 250K to 4M members and
  even at counts just past a power of two (Ubuntu 26.04, Intel Xeon 6975P-C,
  Redis `8.0.5`). At 4M members it saves about `305` MiB of RSS.
- Throughput is a secondary win: measured with `redis-benchmark`,
  Goblin Core is `1.30x` `ZSCORE`, `2.03x` `ZRANK`, `2.23x` `ZADD`, and `1.46x`
  `ZRANGE` versus Redis.
- Build locally with CMake; benchmark instructions live in
  [BENCHMARKS.md](BENCHMARKS.md).
- A performance and architecture overview lives in
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

Goblin Core is not a full Redis replacement. This release is scoped to the
sorted-set command surface above plus `PING` for liveness checks. It does not
implement persistence, replication, cluster mode, pub/sub, Lua, transactions,
ACLs, Redis modules, eviction policies, or general Redis key types.

Use the Redis differential tests and benchmark scripts when changing command
behavior. The goal is to keep the supported subset boringly compatible while
leaving room to optimize internal layouts aggressively. One deliberate
divergence: a single sorted-set member is capped at 64 KiB (Redis allows more),
which keeps the per-member reference two bytes smaller — well above any
realistic member.

## Build From Source

Goblin Core is distributed as source. Build it locally with CMake
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

`GOBLIN.OPTIMIZE <key> [density]` compacts a zset in place to reclaim insertion
slack (score-index block capacity, geometric vector over-allocation) and repacks
the member index, returning the number of bytes reclaimed. Run it on read-mostly
sets after a bulk load. The optional `density` is the target member-index load
factor in `(0, 1]`; it defaults to `0.97`. Use `1.0` only for a set that is
truly read-only and never queried for absent members — a fully packed index has
no empty slot, so a lookup of a missing member scans the whole table.

`--member-index-growth <factor>` sets how much the member index grows on each
rehash (default `2^0.25 ≈ 1.19`). A smaller factor keeps the never-compacted
load factor high (memory) at the cost of more frequent rehashes during writes;
`2.0` is the classic doubling that favors write throughput.

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

Memory is the headline: after a load-then-`GOBLIN.OPTIMIZE` sequence, Goblin
Core stores a sorted set in about `49` RSS bytes per member versus Redis at about
`130` — roughly `37%` of Redis's resident memory — consistently from 250K to 4M
members and even just past a power of two. Throughput is a secondary benefit; measured
with `redis-benchmark` (a single Python client is client-bound and understates
both servers), Goblin Core is faster than Redis on every supported operation,
for example `1.30x` `ZSCORE` and `~2x` `ZRANK`/`ZADD`.

See the [benchmark report](BENCHMARKS.md) for full results, methodology, and
reproducible benchmark commands.

## Source Releases

Goblin Core releases are source-only: a release is the git tag and the source
archive generated by the hosting service.

Build the server from a release checkout:

```sh
git clone https://github.com/adamdeprince/goblin-core.git
cd goblin-core
git checkout v0.2.0
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

The build also copies `llms.txt` and `llms-full.txt` (an operator-focused
overview for LLMs) to the site root. Generated files under `html/` (`.html`,
`.css`, and the copied `.txt` files) are ignored; committed binary assets there,
such as images, are tracked via Git LFS.
