# Goblin Core

Goblin Core is a Redis-like server that beats **every** Redis implementation we
benchmarked — **Redis 7.2.4, Redis 8.8, Valkey 9.1, and Dragonfly** — on both
memory consumption and single-core throughput. It holds a sorted set in **roughly
half** the resident set of legacy Redis, stays the leanest of the field at every
scale, and leads every sorted-set and hash operation measured. See the
[x86 benchmarks](BENCHMARKS.md) and [Loongson 3A6000 benchmarks](LOONGSON_BENCHMARKS.md).
The initial implementation focuses on sorted sets and hashes with a vector-backed
layout and a small RESP command surface.

**Because no CTO ever wants to say: "We're letting you go — the cloud bill got too high."**


Goblin Core is licensed under the Apache License, Version 2.0. See
[LICENSE](https://github.com/adamdeprince/goblin-core/blob/main/LICENSE) and
[NOTICE](https://github.com/adamdeprince/goblin-core/blob/main/NOTICE).

Source: [github.com/adamdeprince/goblin-core](https://github.com/adamdeprince/goblin-core)

## Quick Summary

- Source-only C++23 Redis-like server from [Goblin Reactor](https://goblinreactor.com).
- Current scope: sorted sets, hashes, and `PING`, not full Redis compatibility.
- Primary design: vector-backed zset indexes and compact hash/member storage
  instead of pointer-heavy skiplist layouts.
- Memory is the point: after a load-then-`GOBLIN.OPTIMIZE` sequence, Goblin Core
  holds a sorted set in about `51` bytes per member — flat from 250K to 4M
  members — versus about `80` for Redis 8.8, `84` for Valkey 9.1, and `110` for
  Redis 7.2.4: **roughly half** of legacy Redis and ~`35%` under the modern
  engines. Every engine on the same jemalloc `5.3.0` and shared config, one quiet
  dedicated host.
- Hashes get the same memory treatment: built on the same tuned Swiss table and
  packed arena as the zset but with no scores and no ordering, a hash holds a
  field in about `45` bytes — flat across sizes (16-byte field plus 16-byte
  value), a constant `~13.4` bytes of it per-field overhead. That is **roughly
  half** of Redis `7.2.4`, but the lead over modern engines is smaller — about
  `20%` leaner than Redis `8.8` and `29%` leaner than Valkey `9.1`, and smaller
  values widen it — with every engine on the same jemalloc `5.3.0` and shared
  config.
- Sorted-set throughput leads across the board: `ZADD` `+64%`, `ZRANK` `+41–46%`,
  `ZSCORE` `+19–22%`, `ZRANGE` `+24–29%` over the Redis-family engines on pipelined
  `redis-benchmark`.
- Hashes lead too: `HSET` `+13–26%`, `HGET` `+18–22%`, `HGETALL` `+30–58%`, with
  depth-1 `HGET` latency a near-tie (~`21` µs). See BENCHMARKS.md.
- Latency too: writing each reply immediately makes a depth-1 `ZSCORE`/`ZADD`
  round trip about `1` µs (~`5–8%`) faster than the Redis family — a small,
  consistent edge with no penalty at high fan-out (see Design Priorities).
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
  [BENCHMARKS.md](BENCHMARKS.md) (x86) and
  [LOONGSON_BENCHMARKS.md](LOONGSON_BENCHMARKS.md) (Loongson 3A6000).
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
- `HSET key field value [field value ...]`
- `HSETNX key field value`
- `HGET key field`
- `HMGET key field [field ...]`
- `HDEL key field [field ...]`
- `HGETALL key`
- `HKEYS key`
- `HVALS key`
- `HLEN key`
- `HEXISTS key field`
- `HSTRLEN key field`
- `HINCRBY key field increment`

The protocol handler accepts RESP array commands and a basic inline command
format for local testing.

## Compatibility Scope

Goblin Core is not yet a full Redis replacement. This release is scoped to the
sorted-set and hash command surfaces above plus `PING` for liveness checks. It
does not implement automatic (background) persistence, replication, cluster mode,
pub/sub, Lua, transactions, ACLs, Redis modules, eviction policies, or Redis key
types beyond sorted sets and hashes. Point-in-time snapshots are available on demand via
`GOBLIN.SAVE`/`GOBLIN.LOAD`, and a Redis `dump.rdb` can be imported to migrate
sorted sets (see Run and "Migrating from Redis" below).

Following Redis's single-namespace keyspace, a key holds at most one type: a hash
command against a sorted-set key (or a sorted-set command against a hash key)
returns the standard `WRONGTYPE Operation against a key holding the wrong kind of
value` error rather than coercing the value.

Use the Redis differential tests and benchmark scripts when changing command
behavior. The goal is to keep the supported subset boringly compatible while
leaving room to optimize internal layouts aggressively. One deliberate
divergence: a single sorted-set member — and, in a hash, each field and each
value — is capped at 64 KiB (Redis allows more), which keeps the per-entry
reference two bytes smaller and stays well above any realistic member. A value
larger than that belongs in a blob store (goblin-store.dev), with the hash holding
the returned key.

## Design Priorities

Goblin Core is practical, not pure. It optimizes for low cost and high overall
throughput, and it deliberately declines guarantees that look good on paper but
that your users never feel.

The clearest example is tail latency. Goblin Core's Swiss-table member index
grows by "stop the world and reindex" — an amortized O(1) insert with an
occasional synchronous O(n) rehash. We measured it (see BENCHMARKS.md): through
p99.9, Goblin Core's write latency matches or slightly beats Redis; the cost
lands in the far tail, where a rehash during growth is a millisecond-scale pause
— about 19 spikes per million writes, worst ~28 ms — against Redis's incremental
rehash and its ~0.4 ms max. We take that trade on purpose. A rare spike while a
set is growing means one web page, once, loaded a little slowly — and
`GOBLIN.OPTIMIZE` moves the reindex out of the serving path entirely. Don't
optimize for a real-time guarantee your web app doesn't cash; you'd be paying RAM
rent and CPU for determinism your users never notice.

And RAM rent is not cheap. DRAM prices surged roughly 90% in Q1 2026 versus Q4
2025, server DRAM ran up over 60% quarter-over-quarter in Q2, and even now, in
the "cooling" phase, conventional DRAM contract prices are forecast to rise
13–18% quarter-over-quarter in Q3 2026. When a sorted set costs a third to a half
less RAM than Redis or Valkey to hold, that is money, not a benchmark curiosity.
Goblin Core spends the cheap resource — an occasional rehash pause — to save the
expensive one, memory.

Single-op latency is a smaller, honest win of the same design. Goblin Core writes
each reply the instant it is ready; Redis and Valkey defer to the event-loop
boundary. On a depth-1 connection that one saved hop makes a single `ZSCORE` or
`ZADD` round trip about `1` µs — roughly `5–8%` — faster than the Redis family,
and, unlike a reply-batching design, it costs nothing at fan-out: at hundreds of
saturating clients Goblin Core ties the pack, no overload penalty. A modest,
consistent edge that never goes negative. Memory and throughput are the headline;
latency simply comes along for free.

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

`GOBLIN.OPTIMIZE` and `GOBLIN.MEMORY` work on hash keys too. On a hash,
`GOBLIN.OPTIMIZE <key>` reclaims dead arena bytes left by value updates and field
deletes and repacks the field index. Both sorted sets and hashes also
auto-compact: once dead (reclaimable) arena bytes exceed the live bytes past a
~`1` MiB floor, the structure rebuilds itself, so a value-update/delete-heavy hash
(or a `ZREM`-heavy zset) bounds its own footprint — about twice the live bytes —
without a manual `GOBLIN.OPTIMIZE`.

`--member-index-growth <factor>` sets how much the member index grows on each
rehash (default `2^0.25 ≈ 1.19`), for both the zset member index and the hash
field index. A smaller factor keeps the never-compacted load factor high (memory)
at the cost of more frequent rehashes during writes; `2.0` is the classic
doubling that favors write throughput.

`--zset-chunk-bytes <bytes>` and `--hash-chunk-bytes <bytes>` set the
packed-arena chunk size per type. Each must be a power of two and at least the
per-type minimum — `64` KiB for zsets, `128` KiB for hashes, since one chunk must
hold the largest possible entry — and both default to `1` MiB. Larger chunks
trade granularity for fewer allocations on big structures; the defaults suit
typical workloads.

`GOBLIN.SAVE <path>` snapshots every zset and hash to a file (hashes ride in a
new section, so newer snapshots stay backward compatible with older zset-only
ones). It `fork()`s a
copy-on-write child that writes the snapshot from a frozen image of the data, so
the command returns immediately — replying `Background saving started` — and the
server keeps serving while the child writes; completion or failure is logged, and
only one background save runs at a time (a second returns an error). The child
writes to a temp file and renames it into place, so a crash mid-save never
corrupts the previous snapshot. `GOBLIN.LOAD <path>` (or `--load <path>` at
startup) replaces the current data with a snapshot, replying with the number of
keys loaded. Snapshots are a portable canonical layer (zset members and scores, hash fields
and values) plus a version-gated accelerator (the packed indexes); a snapshot always loads on any
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
6–11). Sorted sets are imported; other RDB key types (strings, lists, sets, and —
for now — hashes) are parsed and skipped. RDB hash import is not yet wired up, so
a hash in a `dump.rdb` does not come across; carry Goblin Core's own hashes
between restarts with a native `GOBLIN.SAVE` snapshot instead. Streams and modules
abort the load with a message — migrate those separately. `+inf`/`-inf` scores
clamp to the largest finite double, and a member larger than 64 KiB aborts the
load.

Newer RDB versions (Redis 7.4+ / RDB 12+) are intentionally not read. Re-save the
dump under Redis ≤ 7.2, or migrate over the wire (`SCAN` + `ZRANGE ... WITHSCORES`
into `ZADD`). After a one-time import, `GOBLIN.SAVE` a native snapshot so
subsequent restarts use the faster native `--load`.

## Benchmark

Memory is the headline: after a load-then-`GOBLIN.OPTIMIZE` sequence, Goblin
Core stores a sorted set in about `51` RSS bytes per member versus about `80` for
Redis 8.8, `84` for Valkey 9.1, and `110` for Redis 7.2.4 — **roughly half** of
legacy Redis and ~`35%` under the modern engines, consistently from 250K to 4M
members and even just past a power of two, with every engine measured on the same
jemalloc `5.3.0`. See BENCHMARKS.md. Throughput leads too: on pipelined
`redis-benchmark`, `ZADD` `+64%`, `ZRANK` `+41–46%`, `ZSCORE` `+19–22%`, and
`ZRANGE` `+24–29%` over the Redis-family engines.

Hashes tell the same story in miniature: about `45` RSS bytes per field, flat
across sizes — **roughly half** of Redis 7.2.4, but a narrower `20–29%` lead over
Redis 8.8 and Valkey 9.1 (the margin grows as values shrink). Hash throughput was
measured on the quiet dedicated host, where Goblin Core leads every op (`HSET`
+13–26%, `HGET` +18–22%, `HGETALL` +30–58%). See BENCHMARKS.md.

See the [x86 benchmark report](BENCHMARKS.md) and
[Loongson 3A6000 benchmark report](LOONGSON_BENCHMARKS.md) for full results,
methodology, and reproducible benchmark commands.

## Source Releases

Goblin Core releases are source-only: a release is the git tag and the source
archive generated by the hosting service.

Build the server from a release checkout:

```sh
git clone https://github.com/adamdeprince/goblin-core.git
cd goblin-core
git checkout v0.4.0
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
