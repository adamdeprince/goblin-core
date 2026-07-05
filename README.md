# Goblin Core

Goblin Core is a Redis-like server built to hold sorted sets in far less
memory than Redis — about `37%` of Redis's resident set for the same data —
while beating its throughput. The initial implementation
focuses on sorted sets with a vector-backed layout and a small RESP command
surface.

**Because no CTO ever wants to say: "We're letting you go — the cloud bill got too high."**


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
- Persistence is fast too: `GOBLIN.SAVE`/`--load` snapshots dump and restore the
  packed indexes, so at 9.5M members they save about `5x` and load about `5.7x`
  faster than a Redis RDB (`0.78s`/`0.69s` vs `3.9s`/`4.0s`).
- Hardware intrinsics, selected at compile time (no runtime CPU dispatch): the
  swiss-table member-index probe is a SIMD group scan (SSE2 on x86, NEON on
  AArch64); snapshot checksums use the CRC32C instruction (SSE4.2 on x86 —
  enabled automatically — the AArch64 CRC extension, or LoongArch); and range
  output uses two-stage software prefetch. Portable scalar fallbacks cover
  everything else, so it builds and runs anywhere.
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
implement automatic (background) persistence, replication, cluster mode,
pub/sub, Lua, transactions, ACLs, Redis modules, eviction policies, or general
Redis key types. Point-in-time snapshots are available on demand via
`GOBLIN.SAVE`/`GOBLIN.LOAD`, and a Redis `dump.rdb` can be imported to migrate
sorted sets (see Run and "Migrating from Redis" below).

Use the Redis differential tests and benchmark scripts when changing command
behavior. The goal is to keep the supported subset boringly compatible while
leaving room to optimize internal layouts aggressively. One deliberate
divergence: a single sorted-set member is capped at 64 KiB (Redis allows more),
which keeps the per-member reference two bytes smaller — well above any
realistic member.

## Design Priorities

Goblin Core is practical, not pure. It optimizes for low cost and high overall
throughput, and it deliberately declines guarantees that look good on paper but
that your users never feel.

The clearest example is tail latency. Goblin Core's Swiss-table member index
grows by "stop the world and reindex" — an amortized O(1) insert with an
occasional synchronous O(n) rehash. We measured it (see BENCHMARKS.md): through
p99.9, Goblin Core's write latency matches or slightly beats Redis; the cost
lands in the far tail, where a rehash during growth is a millisecond-scale pause
— about 19 spikes per million writes, worst ~30 ms — against Redis's incremental
rehash and its ~1.5 ms max. We take that trade on purpose. A rare spike while a
set is growing means one web page, once, loaded a little slowly — and
`GOBLIN.OPTIMIZE` moves the reindex out of the serving path entirely. Don't
optimize for a real-time guarantee your web app doesn't cash; you'd be paying RAM
rent and CPU for determinism your users never notice.

And RAM rent is not cheap. DRAM prices surged roughly 90% in Q1 2026 versus Q4
2025, server DRAM ran up over 60% quarter-over-quarter in Q2, and even now, in
the "cooling" phase, conventional DRAM contract prices are forecast to rise
13–18% quarter-over-quarter in Q3 2026. When a sorted set costs about 37% of what
Redis spends to hold it, that is money, not a benchmark curiosity. Goblin Core
spends the cheap resource — an occasional rehash pause — to save the expensive
one, memory.

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

`GOBLIN.SAVE <path>` snapshots every zset to a file. It `fork()`s a
copy-on-write child that writes the snapshot from a frozen image of the data, so
the command returns immediately — replying `Background saving started` — and the
server keeps serving while the child writes; completion or failure is logged, and
only one background save runs at a time (a second returns an error). The child
writes to a temp file and renames it into place, so a crash mid-save never
corrupts the previous snapshot. `GOBLIN.LOAD <path>` (or `--load <path>` at
startup) replaces the current data with a snapshot, replying with the number of
keys loaded. Snapshots are a portable canonical layer (members and scores) plus a
version-gated accelerator (the packed indexes); a snapshot always loads on any
build or machine, rebuilding the indexes from the canonical layer when the
accelerator cannot be trusted (a different `std::hash`, a changed index format).
Persistence is explicit and client-driven: a crash loses writes made since the
last successful `GOBLIN.SAVE`, so drive saves from your operations and `--load`
on startup.

By design, Goblin Core does not — and will not — offer an append-only write log
(AOF) or an internal snapshot scheduler, for two different reasons.

An append-only log adds latency to every write and still makes you choose an
`fsync` interval that can lose data on a crash anyway — a real hot-path cost for
a durability guarantee that is only ever partial. If you need durable, replayable
writes, that belongs in a system built for it: run them through Kafka (or
similar) ahead of Goblin Core, which replays them. This is the same principle as
the 64 KiB member cap — do the core sorted-set operations better than Redis
rather than reimplement a weaker version of a peripheral feature.

Scheduled snapshots are a lighter matter: `GOBLIN.SAVE` forks and returns
immediately, so it costs almost nothing and does not pause the server. Goblin
Core simply does not own the *policy* of when and how often to run it — drive
that from `cron` or your scheduler, where you already own the rest of your
operations. An internal timer would only move that trigger inside the process for
no real gain.

`GOBLIN.LOAD` and `--load` auto-detect the file by magic: a native Goblin
snapshot or a **Redis RDB file** (`dump.rdb`). This is the migration path — see
"Migrating from Redis" below.

The default (`GOBLIN.SAVE <path>`) is the everyday restart path: it dumps the
packed indexes so a same-build restart loads about `5.7×` faster than Redis by
copying them back instead of rebuilding. `GOBLIN.SAVE <path> NOACCEL` is the
upgrade/migration path — a smaller, canonical-only snapshot without the
accelerator, for moving a snapshot across Goblin versions, architectures, or C++
standard libraries, where the accelerator would be discarded and rebuilt on load
anyway. Its load rebuilds every index (slower than the default, still faster than
Redis). See BENCHMARKS.md for the numbers.

```sh
redis-cli -p 6379 GOBLIN.SAVE /var/lib/goblin/dump.gcsn          # default (fast load)
redis-cli -p 6379 GOBLIN.SAVE /var/lib/goblin/dump.gcsn NOACCEL  # upgrade/migration path
```

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

## Migrating from Redis

Point Goblin Core at a Redis `dump.rdb` and its sorted sets come across:

```sh
./build-release/goblin-core --port 6379 --load /path/to/dump.rdb
# or, against a running server:
redis-cli -p 6379 GOBLIN.LOAD /path/to/dump.rdb
```

The reader accepts RDB files from **Redis 2.6 through 7.2.x** (RDB versions
6–11). Sorted sets are imported; other key types (strings, lists, sets, hashes)
are skipped, since Goblin Core only stores sorted sets today. Streams and modules
abort the load with a message — migrate those separately. `+inf`/`-inf` scores
clamp to the largest finite double, and a member larger than 64 KiB aborts the
load.

Newer RDB versions (Redis 7.4+ / RDB 12+) are intentionally not read. Re-save the
dump under Redis ≤ 7.2, or migrate over the wire (`SCAN` + `ZRANGE ... WITHSCORES`
into `ZADD`). After a one-time import, `GOBLIN.SAVE` a native snapshot so
subsequent restarts use the faster native `--load`.

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
git checkout v0.3.0
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
