# Goblin Core

Goblin Core is a Redis-like server that beats **every** Redis implementation we
benchmarked — **Redis 7.2.4, Redis 8.8, Valkey 9.1, and Dragonfly** — on both
memory consumption and single-core throughput. It holds a sorted set in **roughly
half** the resident set of legacy Redis, stays the leanest of the field at every
scale, and leads every sorted-set and hash operation measured. See the
[x86 benchmarks](BENCHMARKS.md) and [Loongson 3A6000 benchmarks](LOONGSON_BENCHMARKS.md),
and the [ring latency shootout](LATENCY-SHOOTOUT.md) — a full request/reply round trip in
about 220 ns over a shared-memory ring (written up in [this post](blogs/ring-latency.md)).
The [Pub/Sub benchmark](PUBSUB-BENCHMARK.md) covers end-to-end delivery, fanout,
literal routing, glob matching, and subscription memory against the same field.
For a real-world run at scale, see the
[Lichess leaderboard replay](blogs/lichess-leaderboard.md): every rated game in Lichess
history, 14.3 billion `ZADD`s into one sorted set, held in about half the memory of Redis.
The implementation focuses first on sorted sets, hashes, strings, lists, and
Pub/Sub with compact layouts and a deliberately growing Redis-compatible command
surface.

**Because no CTO ever wants to say: "We're letting you go — the cloud bill got too high."**


Goblin Core is licensed under the Apache License, Version 2.0. See
[LICENSE](https://github.com/adamdeprince/goblin-core/blob/main/LICENSE) and
[NOTICE](https://github.com/adamdeprince/goblin-core/blob/main/NOTICE).

Source: [github.com/adamdeprince/goblin-core](https://github.com/adamdeprince/goblin-core)

## Quick Summary

- Source-only C++23 Redis-like server from [Goblin Reactor](https://goblinreactor.com).
- Current scope includes sorted sets, hashes, strings, lists, TTLs, counters,
  Pub/Sub, scripting, and `PING`; it is not yet the full Redis command surface.
- Primary design: vector-backed zset indexes and compact hash/member storage
  instead of pointer-heavy skiplist layouts.
- Memory is the point: after a load-then-`GOBLIN.OPTIMIZE` sequence, Goblin Core
  holds a sorted set in about `51` bytes per member — flat from 250K to 4M
  members — versus about `80` for Redis 8.8, `84` for Valkey 9.1, and `110` for
  Redis 7.2.4: **roughly half** of legacy Redis and ~`35%` under the modern
  Redis-family engines; it is also about `6–7%` leaner than Dragonfly. Every
  engine is measured under allocator/config parity on one quiet dedicated host.
- Hashes get the same memory treatment: built on the same tuned Swiss table and
  packed arena as the zset but with no scores and no ordering, a hash holds a
  field in about `45` bytes — flat across sizes (16-byte field plus 16-byte
  value), a constant `~13.4` bytes of it per-field overhead. That is **roughly
  half** of Redis `7.2.4`, but the lead over modern engines is smaller — about
  `20%` leaner than Redis `8.8` and `29%` leaner than Valkey `9.1`, and smaller
  values widen it — measured under the same allocator/config parity as the zset
  benchmarks.
- Sorted-set throughput leads across the board: `ZADD` `+67–69%`, `ZRANK`
  `+35–43%`, `ZSCORE` `+20–24%`, `ZRANGE` `+19–28%` over the Redis-family engines
  on pipelined `redis-benchmark`.
- Hashes lead too: `HSET` `+13–26%`, `HGET` `+18–22%`, `HGETALL` `+30–58%`, with
  depth-1 `HGET` latency a near-tie (~`21` µs). See BENCHMARKS.md.
- Latency too: writing each reply immediately makes a depth-1 `ZSCORE`/`ZADD`
  round trip about `1` µs (~`5–8%`) faster than the Redis family — a small,
  consistent edge with no penalty at high fan-out (see Design Priorities).
- Persistence is fast too: `GOBLIN.SAVE`/`--load` snapshots dump and restore the
  packed indexes, so at 9.5M members they save about `5x` and load about `5.7x`
  faster than a Redis RDB (`0.78s`/`0.69s` vs `3.9s`/`4.0s`).
- Live replication is transport-neutral: [`GOBLIN.FIREHOSE`](docs/replication.md)
  streams canonical writes over TCP, UDS, shared-memory rings, or RDMA, and
  replicas can feed replicas.
- Durable replay belongs to Kafka: [`--kafka`](docs/kafka.md) journals primary
  writes as canonical RESP2 mutations and recovers from the exact broker and
  logical offsets saved in a native snapshot.
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
  [PERFORMANCE_BRIEF.md](PERFORMANCE_BRIEF.md). The Redis list implementations
  are documented in [LISTS.md](LISTS.md), including the adaptive PMA and
  segmented-listpack algorithm classes, rank indexes, endpoint bias, and memory
  controls. Their repeatable
  cross-engine results are in [LIST-BENCHMARK.md](LIST-BENCHMARK.md).
- Pub/Sub results are in [PUBSUB-BENCHMARK.md](PUBSUB-BENCHMARK.md), including
  Goblin over RESP2/UDS and SBE over 4 KiB rings against every incumbent over
  RESP2/UDS.

## Current Commands

- `PING [message]`
- `AUTH [username] password`
- `HELLO [2|3 [AUTH username password] [SETNAME client-name]]`
- `COMMAND [INFO [command-name ...]]`
- `CONFIG GET pattern [pattern ...]`
- `CLIENT SETINFO LIB-NAME|LIB-VER value`
- `CLIENT SETNAME name`, `CLIENT GETNAME`, `CLIENT ID`
- `SELECT 0`
- `QUIT`
- `ECHO message`
- `TIME`, `ROLE`
- `SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]`
- `DBSIZE`, `RANDOMKEY`, `TOUCH key [key ...]`
- `RENAME source destination`, `RENAMENX source destination`
- `COPY source destination [DB 0] [REPLACE]`
- `GETEX key [EX seconds|PX milliseconds|EXAT unix-seconds|PXAT unix-ms|PERSIST]`
- `MSETNX key value [key value ...]`
- `ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]`
- `ZINCRBY key increment member`
- `ZCARD key`
- `ZCOUNT key min max`
- `ZRANGE key start stop [REV] [WITHSCORES]`
- `ZRANGE key min max BYSCORE [REV] [LIMIT offset count] [WITHSCORES]`
- `ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count]`
- `ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count]`
- `ZRANK key member`
- `ZREVRANK key member`
- `ZSCORE key member`
- `ZMSCORE key member [member ...]`
- `ZREM key member [member ...]`
- `ZREMRANGEBYSCORE key min max`
- `ZREMRANGEBYRANK key start stop`
- `ZINTERSTORE destination numkeys key [key ...] [WEIGHTS ...] [AGGREGATE SUM|MIN|MAX]`
- `ZUNIONSTORE destination numkeys key [key ...] [WEIGHTS ...] [AGGREGATE SUM|MIN|MAX]`
- `ZPOPMIN key [count]`, `ZPOPMAX key [count]`
- `ZSCAN key cursor [MATCH pattern] [COUNT count]`

See the [sorted-set command reference](docs/commands/sorted-sets.md) for option
compatibility, score-bound syntax, reply shapes, and storage behavior.
- `HSET key field value [field value ...]`
- `HMSET key field value [field value ...]`
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
- `HINCRBYFLOAT key field increment`
- `HRANDFIELD key [count [WITHVALUES]]`
- `HSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]`

Every hash command above is also available under `GOBLIN.RT.*` and
`GOBLIN.EFFICENT.*`. The qualified family selects the representation for a new
key; see [Real-time hash indexes](docs/real-time-hashes.md).

`SCAN`, `HSCAN`, `SSCAN`, and `ZSCAN` provide bounded cursor iteration. See the
[iteration command reference](docs/commands/iteration.md) for filtering, reply,
and mutation semantics.

- `LPUSH key value [value ...]`
- `RPUSH key value [value ...]`
- `LPUSHX key value [value ...]`
- `RPUSHX key value [value ...]`
- `LPOP key [count]`
- `RPOP key [count]`
- `LMOVE source destination LEFT|RIGHT LEFT|RIGHT`
- `RPOPLPUSH source destination`
- `BLPOP key [key ...] timeout`
- `BRPOP key [key ...] timeout`
- `BLMOVE source destination LEFT|RIGHT LEFT|RIGHT timeout`
- `LMPOP numkeys key [key ...] LEFT|RIGHT [COUNT count]`
- `BLMPOP timeout numkeys key [key ...] LEFT|RIGHT [COUNT count]`
- `LLEN key`
- `LINDEX key index`
- `LRANGE key start stop`
- `LSET key index value`
- `LTRIM key start stop`
- `LREM key count value`
- `LINSERT key BEFORE|AFTER pivot value`
- `SUBSCRIBE channel [channel ...]`
- `UNSUBSCRIBE [channel ...]`
- `PSUBSCRIBE pattern [pattern ...]`
- `PUNSUBSCRIBE [pattern ...]`
- `PUBLISH channel message`
- `PUBSUB CHANNELS [pattern]`
- `PUBSUB NUMSUB [channel ...]`
- `PUBSUB NUMPAT`

These standard list names resolve through `--list-implementation pma|segmented`
(`segmented` is the default). Both backends are also addressable directly with
`GOBLIN.PMA.` or `GOBLIN.SEGMENTED.` plus the command name, for example
`GOBLIN.PMA.LINDEX` and `GOBLIN.SEGMENTED.LPUSH`. Qualified command families let
multiple list implementations coexist and be benchmarked without changing the
standard compatibility surface.

The protocol handler accepts RESP array commands and a basic inline command
format for local testing. Connections start in RESP2; `HELLO 3` selects RESP3 for
that connection and `HELLO 2` switches it back. See
[the HELLO command](docs/commands/HELLO.md) for native RESP3 reply shapes.

Pub/Sub is available over RESP2, RESP3, and opt-in SBE on every transport. RESP2 uses
the traditional subscribed mode; RESP3 uses push frames and keeps ordinary
commands available while subscribed. Literal channels use direct lookup and
patterns use Redis-compatible glob matching. See the
[Pub/Sub command reference](docs/commands/pubsub.md) for delivery frames, SBE
templates, and slow-consumer behavior.

## Scripting

Goblin Core embeds **six independent scripting interpreters**, each reached through
its own command prefix, with its own VM and its own precompiled script cache; they
share nothing but the key space:

- **PUC-Lua 5.1** — `EVAL` / `EVALSHA` / `SCRIPT`, the dialect real Redis scripts target
- **Luau** — `LUAU.EVAL` …, Roblox's typed, sandboxed Lua
- **Wren** — `WREN.EVAL` …, a small class-based language
- **Jim Tcl** — `TCL.EVAL` …
- **MicroPython** — `UPYTHON.EVAL` …
- **QuickJS** — `QUICKJS.EVAL` …, JavaScript

Each is a *distinct* interpreter: a script written for one is a syntax error under
another. `SCRIPT LOAD` (or the first `EVAL`) compiles a script once and caches the
compiled artifact, so `EVALSHA` runs it with no re-parse or re-compile. There are
also native conditional-write commands — `GOBLIN.CAD` (compare-and-delete),
`GOBLIN.CAEXPIRE` (compare-and-expire), and `GOBLIN.CAS` (compare-and-set, keeping
the TTL) — for the Redlock lock-release, watchdog-renewal, and check-and-swap
idioms, plus `GOBLIN.INCREX` (fixed-window rate limit) and `GOBLIN.ZWINDOW`
(sliding-window limiter / mutex / counting semaphore) for the two classic
rate-limit shapes, `GOBLIN.INCRBOUND` (bounded increment / quota) and
`GOBLIN.DECRPOS` (decrement-if-positive / stock reservation) for capped counters,
the hash-field pair `GOBLIN.HCAD` (compare-and-delete a field) and `GOBLIN.HSETGT`
(set-if-greater — the `ZADD GT` that hashes lack, for watermarks), and
`GOBLIN.CLAIM` (an idempotency guard that claims work once with an expiring lease).
See [docs/commands](docs/commands/README.md) for the surface, and
**[BENCHMARK-LANGUAGES.md](BENCHMARK-LANGUAGES.md)** for how the six languages
compare on a trivial op (compare-and-delete) and a heavy one (real-time leaderboard
rescore) — the ranking flips between them.

## Compatibility Scope

Goblin Core is growing toward the full Redis command surface with tighter memory
layouts and faster execution paths. This build covers sorted sets, hashes,
strings, lists, TTLs, counters, Pub/Sub, and scripting. JavaScript and Python are
embedded for modern programmers, with Lua, Luau, Wren, and Tcl available when
another runtime fits the job.

The deliberate boundary is infrastructure policy. Goblin Core does not provide
an append-only write log or cluster mode. Point-in-time
`GOBLIN.SAVE`/`GOBLIN.LOAD` snapshots cover fast restarts;
[`GOBLIN.FIREHOSE`](docs/replication.md) provides live primary/replica streams;
and [`--kafka`](docs/kafka.md) journals and replays the durable RESP2 mutation
log, where logging is the product. Replicas automatically reconnect; Kafka
bridges any logged offset gap while `INFO` readiness stays down until the
handoff is complete. A Redis `dump.rdb` can be imported to migrate
sorted sets and lists (see "Migrating from Redis" below).

Following Redis's single-namespace keyspace, a key holds at most one type. A
type-specific command against a key of another type returns the standard
`WRONGTYPE Operation against a key holding the wrong kind of value` error rather
than coercing the value.

Use the Redis differential tests and benchmark scripts when changing command
behavior. The goal is to keep the supported subset boringly compatible while
leaving room to optimize internal layouts aggressively. One deliberate
divergence keeps length metadata at two bytes: keys, sorted-set members, and hash
fields are capped at 65,535 bytes. String values, hash values, and PMA list values
share an encoded representation capped at 65,535 bytes. An ordinary raw value
can contain up to 65,534 bytes because its `0xff` tag consumes one byte. With
`--use-lz4`, a compressible logical value can be larger, up to the encoding's
24-bit logical-length ceiling, provided the complete encoded form still fits.
`--disable-encoding` instead stores string, hash, and list bytes verbatim with no
tag or LZ4 pass, raising the direct value ceiling to 65,535 bytes for clients
that already provide compact binary data.
Larger objects belong in [Goblin Store](https://goblin-store.dev), with Goblin
Core holding the returned key.

## Design Priorities

Goblin Core is practical, not pure. It optimizes for low cost and high overall
throughput, and it deliberately declines guarantees that look good on paper but
that your users never feel.

The clearest example is tail latency. The default efficient Swiss-table member
index grows by "stop the world and reindex": amortized O(1) insertion with an
occasional synchronous O(n) rehash. We measured it (see BENCHMARKS.md): through
p99.9, Goblin Core's write latency matches or slightly beats Redis; its cost is
in the far tail, where growth can pause for milliseconds. That remains the right
memory-first trade for most web workloads.

It is not the only trade now. `--hash-implementation rt` uses linear hashing over
small Swiss buckets and advances at most one 16-slot physical bucket of a split
or reverse merge per mutation. Each slot caches its folded hash so migration
does not rehash field bytes. Buckets use stable 32-bit handles in a shared,
prefaulted fixed-record arena; a 160-handle inline prefix and flat 512-entry
directory blocks keep handle lookup constant-depth. `--rt-hash-index-bytes`
sizes the bucket-cell pool (16 MiB by default, rounded to 2 MiB slabs), and
Goblin derives fixed directory headroom from it. `--real-time` also applies the
index to the top-level keyspace.
The `GOBLIN.RT.*` and `GOBLIN.EFFICENT.*` hash families let benchmarks and
clients select explicitly. RT reserves more slack and skips compact hash
listpacks; in return, serving growth never performs a table-wide rehash. See the
[algorithm and operational contract](docs/real-time-hashes.md).
Array mode uses `GOBLIN.CLASSIC.AR*` / `GOBLIN.RT.AR*` (Classic ≈ Redis 8.8-style
arrays; not the hash EFFICENT family). RT arrays use fixed-capacity dense leaves
and a separate `--rt-array-arena-growth` policy (default `2.0`).
`GOBLIN.RT.ARRESERVE key max-index value-slots encoded-bytes` prefaults the
complete declared index and value budget before serving, then fails closed on
exhaustion instead of allocating in a mutation.

And RAM rent is not cheap. DRAM prices surged roughly 90% in Q1 2026 versus Q4
2025, server DRAM ran up over 60% quarter-over-quarter in Q2, and even now, in
the "cooling" phase, conventional DRAM contract prices are forecast to rise
13–18% quarter-over-quarter in Q3 2026. When a sorted set costs a third to a half
less RAM than Redis or Valkey to hold, that is money, not a benchmark curiosity.
Goblin Core spends the cheap resource — an occasional rehash pause — to save the
expensive one, memory.

That memory win comes with one string attached, and it is worth stating plainly.
Sorted-set members that share a score sort by name, and Goblin Core makes that
tie-break cheap by keeping each member's first four bytes inline, so the common
comparison never chases a pointer into the member arena. Think of it as a magic
memory fountain: free water in the desert. Just do not fill a swimming pool with
it. A *fixed shared prefix* — `user:1000001`, `user:1000002`, … — makes those four
bytes identical for every member, so the accelerator cannot tell them apart and
every same-score comparison falls back to the full-name scan. Worse, you have
stored that constant `user:` verbatim a million times. You pay twice — in memory
for the redundant bytes, and in speed for an accelerator that cannot help — for a
namespace that belongs in the key, not the member. Strip the shared part (store
`1000001`, and key by namespace) and the fountain works. If you came to Goblin Core
for the memory win, your UUIDs are already binary and your prefixes already gone.

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
- LZ4 headers and library (`liblz4-dev` or `lz4` from Homebrew)
- libsodium headers and library (`libsodium-dev` or `libsodium` from Homebrew)
- OpenSSL development headers and libraries (`libssl-dev` or `openssl@3` from
  Homebrew) for native TCP TLS; use `-DGOBLIN_CORE_ENABLE_TLS=OFF` to omit it
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

**Authentication.** Create an owner-only libsodium password file with the
installed editor, then require it for RESP clients:

```sh
goblin-core-auth --file /etc/goblin/core.auth add default 'long-secret'
./build-release/goblin-core --port 6379 --auth-file /etc/goblin/core.auth
```

Use `AUTH [username] password` or `HELLO 3 AUTH username password`. RESP over
TCP/UDS/ExaSock authenticates whenever `--auth-file` is set. RESP rings and RDMA
do too unless `--no-auth-ring` or `--no-auth-rdma` explicitly places that fabric
inside the trust boundary; those bypasses make AUTH optional rather than
unavailable. Non-loopback ordinary TCP additionally requires native TLS, while
the mandatory `127.0.0.1` endpoint stays plaintext. See
[Authentication](docs/authentication.md), including transport boundaries and
credential rotation rules.

**Transactions.** RESP clients can use `MULTI`, `EXEC`, `DISCARD`, `WATCH`, and
`UNWATCH`. Queued commands execute as one atomic block; `WATCH` is invalidated by
actual key writes and expiration without adding revision metadata to every key.
Each connection receives a fixed, anonymous transaction mapping. It defaults to
one native page and can be changed with `--transaction-buffer-bytes <bytes>`;
positive values round up to a whole page and the mapping never grows. An
overflowed transaction is drained and discarded instead of growing the heap.
See the [transaction command reference](docs/commands/transactions.md).

**TCP and TLS listeners.** Repeat `--listen <address>:<port>` and
`--uds-listen <path>` to serve the same store through multiple IPv4, IPv6, and
Unix-domain endpoints at once. IPv6 addresses use brackets. Every configured
TCP port also listens on plaintext `127.0.0.1`; non-loopback ordinary TCP
listeners require one shared TLS certificate identity:

```sh
./build-release/goblin-core \
  --listen 192.168.1.13:6379 \
  --listen '[2001:db8::13]:6379' \
  --tls-cert-file /etc/goblin/fullchain.pem \
  --tls-key-file /etc/goblin/private-key.pem \
  --uds-listen /run/goblin/client.sock \
  --uds-listen /run/goblin/admin.sock
```

`:PORT` is shorthand for `127.0.0.1:PORT`. The older `--tcp-listen` spelling is
an alias for `--listen`; `--bind <address> --port <port>` still configures one
TCP endpoint when no explicit socket listener is present. `--unixsocket <path>`
is a repeatable alias for `--uds-listen`. Even a transport-oriented deployment
with only UDS, rings, RDMA, or ExaSock configured retains its plaintext
localhost listener. See [TCP listeners and TLS](docs/tls.md) for wildcard rules,
the OpenSSL build switch, and the protocol security boundary.

**Shared-memory rings (the fast path).** Pass `--ring <path> <size>` (repeatable) to
accept requests over io_uring-style SQ/CQ ring buffers in shared memory instead of
sockets — no syscall, no network stack on the request path:

```sh
./build-release/goblin-core --port 6379 --ring /tmp/a 4kb --ring /tmp/b 1mb
redis-cli-ring /tmp/a SET foo bar     # the proof-of-concept ring client
```

**Polled RDMA rings (the fabric path).** On Linux builds with `libibverbs` and
`librdmacm`, `--rdma <address> <port> <size>` adds a receiver-polled one-sided
RDMA target. It may be repeated and interleaved with `--ring`; literal option
order is strict poll priority:

```sh
./build-release/goblin-core --cpu 77 \
  --ring /tmp/local 64kb --rdma 10.88.88.1 6380 64kb
redis-cli-rdma 10.88.88.1 6380 64kb SET foo bar
```

The established path is inline RC RDMA WRITE into fixed sequence-word slots.
The receiver polls local registered memory, and an explicit RDMA READ refreshes
credits only when the sender's cached view says the remote ring is full. See
**[docs/rdma-rings.md](docs/rdma-rings.md)** for the wire layout, NUMA rules,
flow control, and C++ RESP/SBE clients.

On Linux, Goblin resolves specific socket and RDMA bind addresses to their NICs
and NUMA nodes. It selects the slice automatically only when every hardware
endpoint agrees. If an ordinary NIC and an InfiniBand adapter are on different
nodes, startup stops and prints the available nodes, CPUs, NICs, and InfiniBand
devices. Choose deliberately with `--numa 1`, `--numa eth0`, `--numa mlx5_0`,
or the exact-core form `--cpu 77`. Unknown device locality on a multi-node host
also requires an explicit choice.

Transport and protocol are independent: ordinary sockets, shared-memory rings,
and RDMA rings support RESP and the SBE binary wire. SBE is disabled by default;
start the server with `--enable-sbe` to accept the one-time `GOBLINS!` handshake.
SBE is deliberately unauthenticated and belongs only on a trusted fabric.
Otherwise an endpoint speaks RESP2 by default and may select RESP3 with
`HELLO 3`.

With polled targets the server checks them in command-line order (the first can
starve the second, by design) and pegs a core at 100%; without one it stays
event-driven and idle-cheap. A one-header C++ client
(`goblin/core/ring_client.hpp`) drives a local ring in a few lines. Full details:
**[docs/ring-buffers.md](docs/ring-buffers.md)**.

`--unsolicited-output-buffer-bytes <bytes>` sizes each client's anonymous
`mmap`-backed Pub/Sub output FIFO. The default is one native page; every positive
value is rounded up to a whole number of pages. A client whose next complete
message cannot fit is removed from all subscriptions and disconnected, so slow
consumers cannot grow the heap or cause silent message loss.

`--transaction-buffer-bytes <bytes>` similarly sizes the anonymous queue used by
`MULTI`. It defaults to one native page and rounds up to pages. If a queued
command cannot fit, the server keeps draining the transaction, reports the
limit, and rejects the complete block at `EXEC`; `DISCARD` resets it immediately.

At startup the `goblin-core` executable calls
`mlockall(MCL_CURRENT | MCL_FUTURE)` so the heap and later allocations remain
resident. Arena blocks, ring regions, Pub/Sub FIFOs, and transaction mappings
also call `mlock` explicitly; rings, Pub/Sub FIFOs, and transaction mappings are
prefaulted before they enter service.
Linux deployments must raise `RLIMIT_MEMLOCK` or grant the process permission to
lock memory. A rejected or unimplemented process-wide lock emits a startup
warning; the explicit core-mapping locks are still attempted.

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

`GOBLIN.OPTIMIZE` and `GOBLIN.MEMORY` work on hash and list keys too. On a hash,
`GOBLIN.OPTIMIZE <key>` reclaims dead arena bytes left by value updates and field
deletes and repacks the field index. On a list it rebuilds the value arena and
packs the adaptive PMA at its configured density. Hash compaction keeps arena
blocks as a contiguous stack: it densifies a holey target, fills it from the last
block, deletes a fully drained tail, and repeats with the preceding block. The
first nonempty block promoted to tail is copied off HugeTLB and shrunk while it is
being compacted. Normal hash mutations advance selection and relocation with
fixed work/byte budgets. Default tail donation is an allocation-free streaming
first-fit pass: each inspected field id consumes one work unit, and mutations
that touch already-scanned ids enter a small fixed pending queue instead of
rewinding the monotonic cursor. Target densification still scans the hash and
remains the known tail-latency work; `GOBLIN.OPTIMIZE` drains the same algorithm
immediately and then repacks the field index.

Tail donation uses deterministic first-fit selection by default. Exact SIMD
subset-sum packing is available for experiments with
`--hash-compaction-knapsack`; `--no-hash-compaction-knapsack` selects first-fit
explicitly. Exact packing costs substantially more serving latency and is not the
default; unlike the streaming path, it materializes the complete donor set and is
not latency-bounded.

`--hash-compaction-work-budget <count>` sets how many chunk candidates or field
ids automatic hash maintenance may inspect after one mutating command (default
`32`). Copied donor data remains separately capped at 16 KiB per command. Raising
the scan budget makes large hashes reclaim churn faster; lowering it puts a
tighter ceiling on maintenance CPU.

`--member-index-growth <factor>` sets how much the member index grows on each
rehash (default `2^0.25 ≈ 1.19`), for both the zset member index and the hash
field index. A smaller factor keeps the never-compacted load factor high (memory)
at the cost of more frequent rehashes during writes; `2.0` is the classic
doubling that favors write throughput.

`--hash-implementation efficient|rt` selects the representation created by
standard hash commands. `--real-time` selects RT hashes and the incremental
linear-hash keyspace index together. Existing hashes keep their implementation;
selectors never convert a live key implicitly.

`--rt-hash-index-bytes <bytes>` sizes the fixed RT bucket-cell pool shared by
hashes, and the same size is used for the separate keyspace-index pool under
`--real-time`. The default is `16` MiB; bucket values round up to 2 MiB slabs.
Flat-directory blocks and per-index block tables add derived headroom; `INFO`
reports the complete committed size. Every part is allocated, prefaulted, and
locked before serving. Growth and reverse merges move at most one physical
bucket per mutation. A mutation that needs an unavailable bucket, directory
block, or table fails explicitly. The serving path never grows the pool.

`--maxmemory <bytes>` places a hard `noeviction` ceiling on persistent store
capacity. Byte values accept `k`, `m`, and `g` binary suffixes. Arena, index,
directory, object-slot, TTL, fixed RT-pool, and compact-blob-pool growth is
admitted before allocation; a write that would cross the limit returns `OOM`.
Deletes and writes that reuse existing capacity continue to work. `0` or
omission is unlimited. See [the maxmemory guide](docs/maxmemory.md) for the
accounting boundary and operational behavior.

`--zset-chunk-bytes <bytes>`, `--hash-chunk-bytes <bytes>`, and
`--list-chunk-bytes <bytes>` set the packed-arena chunk size per type. Each must
be a power of two and large enough to hold its largest entry; all three default
to `2` MiB. `--list-implementation pma|segmented` selects the backend used by
standard list commands; segmented lists are the default. Lists separately expose
`--list-max-density` (default `0.97`) and
`--list-resize-growth` (default `2**0.25`) so packing and resize policy do not
silently control one another. See [LISTS.md](LISTS.md).

String values, hash values, compact-hash fields, and both list backends use one
binary-safe encoder. Canonical decimal integers and lowercase raw or dashed
UUIDs use compact binary forms; every other value carries a `0xff` raw tag and
decodes to the exact input bytes. Compact-hash fields use the same exact encoder
but never LZ4. `--use-lz4 <bytes>` enables LZ4 for raw-form values at or above
the given logical size. Specialized integer and UUID forms are never compressed.
A compressed value carries a `0xfe` tag and a three-byte logical length. If LZ4
does not fit but the raw form does, Goblin stores the raw form instead.

`--disable-encoding` disables specialization and compression across strings,
hashes, and lists. Stored bytes are exactly the supplied bytes, with no `0xff`
marker; empty values therefore occupy zero payload bytes and direct values may
use the full 65,535-byte length. This mode is intended for smart clients that
already use compact binary keys and values. Any `--use-lz4` or
`--lz4-compress-level` setting is ignored while encoding is disabled.

`--lz4-compress-level <level>` selects the compressor: `0` (or omission) uses
`LZ4_compress_default`, `3..12` selects the corresponding LZ4HC level, and
`-1..-8` selects `LZ4_compress_fast` with acceleration `1..8`. The compression
level has no effect unless `--use-lz4` is present.

`GOBLIN.SAVE <path>` snapshots every supported key type and its TTL. Lists are
written in canonical order rather than persisting PMA slots or arena addresses.
It `fork()`s a
copy-on-write child that writes the snapshot from a frozen image of the data, so
the command returns immediately — replying `Background saving started` — and the
server keeps serving while the child writes; completion or failure is logged, and
only one background save runs at a time (a second returns an error). The child
writes to a temp file and renames it into place, so a crash mid-save never
corrupts the previous snapshot. `GOBLIN.LOAD <path>` (or `--load <path>` at
startup) replaces the current data with a snapshot, replying with the number of
keys loaded. Snapshots carry portable canonical data for zsets, hashes, strings,
and lists plus version-gated accelerators for the indexed types; a snapshot loads
on any build or machine, rebuilding indexes when an accelerator cannot be
trusted (a different `std::hash`, a changed index format). For all-raw lists, a
versioned two-byte accelerator marker lets the canonical bytes copy directly
into the selected final representation without duplicating them in the file.
Other lists are still restored with one bulk build from ordered snapshot views,
rather than one mutation per element; Redis RDB list imports use the same bulk
construction path.
Snapshots are explicit and client-driven. Without Kafka, a crash loses writes
made since the last successful `GOBLIN.SAVE`, so drive saves from your
operations and `--load` on startup. With Kafka, the snapshot's saved lineage,
logical offset, and acknowledged broker offset provide the restart point for
inclusive replay; see [Firehose replication and Kafka
recovery](docs/replication.md).

By design, Goblin Core does not — and will not — offer an append-only write log
(AOF) or an internal snapshot scheduler, for two different reasons.

An append-only log adds latency to every write and still makes you choose an
`fsync` interval that can lose data on a crash anyway — a real hot-path cost for
a durability guarantee that is only ever partial. If you need durable, replayable
writes, that belongs in a system built for it: configure Kafka as Goblin Core's
durable journal and replay source. This is the same principle as
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

Point Goblin Core at a Redis `dump.rdb` and its sorted sets and lists come across:

```sh
./build-release/goblin-core --port 6379 --load /path/to/dump.rdb
# or, against a running server:
redis-cli -p 6379 GOBLIN.LOAD /path/to/dump.rdb
```

The reader accepts RDB files from **Redis 2.6 through 7.2.x** (RDB versions
6–11). It imports plain and compact sorted sets plus plain lists, ziplist lists,
quicklists, and quicklist2/listpack lists. Strings, sets, and hashes are parsed
and skipped; carry Goblin Core's own versions of those types between restarts
with a native `GOBLIN.SAVE` snapshot. Streams and modules abort the load with a
message. Infinite scores clamp to the largest finite double. A sorted-set member
over 65,535 bytes, or a list value that cannot fit the configured shared value
encoding, aborts the load atomically.

Newer RDB versions (Redis 7.4+ / RDB 12+) are intentionally not read. Re-save the
dump under Redis ≤ 7.2, or migrate over the wire (`SCAN` + `ZRANGE ... WITHSCORES`
into `ZADD`). After a one-time import, `GOBLIN.SAVE` a native snapshot so
subsequent restarts use the faster native `--load`.

## Benchmark

Memory is the headline: after a load-then-`GOBLIN.OPTIMIZE` sequence, Goblin
Core stores a sorted set in about `51` RSS bytes per member versus about `80` for
Redis 8.8, `84` for Valkey 9.1, and `110` for Redis 7.2.4 — **roughly half** of
legacy Redis and ~`35%` under Redis 8.8, consistently from 250K to 4M members.
Dragonfly is closest at ~`55` RSS bytes/member, with Goblin still about `6–7%`
lower. See BENCHMARKS.md. Throughput leads too: on pipelined `redis-benchmark`,
`ZADD` `+67–69%`, `ZRANK` `+35–43%`, `ZSCORE` `+20–24%`, and `ZRANGE` `+19–28%`
over the Redis-family engines.

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
git checkout v0.9.0
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
cmake --install build-release --prefix /usr/local
```

Use the [release checklist](RELEASE.md) before publishing a tag.

## HTML Docs

The build converts the project Markdown docs into static HTML under `html/`.
`README.md` becomes `html/README.html`; the hand-authored `html/index.html`
remains the site landing page. Markdown links between docs are rewritten to
HTML links with human-visible labels that do not end in `.md`.

```sh
python3 scripts/build_html_docs.py --output html
```
