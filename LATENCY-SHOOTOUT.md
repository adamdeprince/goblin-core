# Latency shootout: SBE over the ring vs the socket tax

Goblin Core supports two transports — sockets and shared-memory rings — and two protocols,
RESP and SBE. Either protocol works over either transport. This shootout compares the most
compatible combination, RESP over a Unix-domain socket, with the lowest-latency combination,
SBE over a shared-memory ring: no kernel on the request path, binary numeric fields, and a
cache-line handoff between two cores. It measures the latency of a single, unpipelined
request/reply.

It puts Goblin's SBE/ring next to Goblin's own RESP-over-Unix-socket and to every
Redis-family incumbent on their sockets — Redis 7.2.4, Redis 8.8, Valkey 9.1, and
Dragonfly — on the same host, with the same client and the same timer. The result: on
these operations the ring is 38–50x faster at the median than the fastest socket, and its
tail is tighter by two orders of magnitude.

## What is measured

[`benchmarks/latency_shootout.cpp`](benchmarks/latency_shootout.cpp) is one C++ client with
one timing method (the CPU cycle counter, `rdtscp`) and two backends: SBE over the ring, and
RESP over a Unix-domain socket. The RESP backend is engine-agnostic, so the *only* variable
between rows is the transport, the protocol, and the server — never the client language.

Six operations, a write and a point-read for each data type:

| type | write | read |
|---|---|---|
| string | `SET s v` | `GET s` |
| hash | `HSET h f5 v5` | `HGET h f5` |
| zset | `ZADD z 5.5 m5` | `ZSCORE z m5` |

Each op is one synchronous round trip — send the command, block for its reply — in a tight
loop. We report the median over a 2-second window (millions of samples for the ring, ~120k
for the sockets) plus p90/p99/min. The writes update an element that already exists, so the
collections never grow.

Cardinality is deliberately low: the zset holds 10 members and the hash 10 fields. At that
size every engine is on its compact small-collection encoding — listpack in the Redis family,
`CompactListpack`/`CompactHashListpack` in Goblin — so there is almost no data-structure work
per op. What remains is the transport, which is the point.

## The host

AMD Ryzen Threadripper PRO 5995WX — 64 cores / 128 threads, Zen 3, 2021. Worth stating: this
is a many-core workstation part tuned for aggregate throughput, not a single-thread speed
champion; a consumer desktop chip clocks higher on one core. That makes these numbers a
conservative floor. The socket server and the client are pinned to neighbouring cores (shared
L3); the ring's server and client likewise. No other tuning.

## Median round-trip latency (microseconds)

Read the median. On a scheduler-managed host the mean is pulled up by rare preemptions of the
busy-poll loop; the median and the tight p90 are the true per-op cost.

| engine / transport | SET | GET | HSET | HGET | ZADD | ZSCORE |
|---|---:|---:|---:|---:|---:|---:|
| Goblin — SBE / ring | 0.20 | 0.17 | 0.20 | 0.21 | 0.22 | 0.19 |
| Goblin — RESP / UDS | 8.34 | 8.54 | 8.58 | 8.52 | 8.44 | 8.57 |
| Redis 7.2.4 — RESP / UDS | 9.11 | 8.75 | 8.95 | 8.88 | 9.19 | 9.03 |
| Redis 8.8 — RESP / UDS | 9.33 | 9.13 | 10.32 | 9.56 | 10.39 | 9.55 |
| Valkey 9.1 — RESP / UDS | 9.01 | 8.85 | 9.08 | 9.04 | 10.10 | 9.07 |
| Dragonfly — RESP / UDS | 8.99 | 8.79 | 9.76 | 9.70 | 9.82 | 9.81 |

## What it shows, in three layers

### 1. Goblin's socket path is competitive

Goblin-RESP/UDS (8.3–8.6 µs) is the fastest of the socket engines, a hair ahead of
Redis/Valkey/Dragonfly (8.7–10.4 µs). So the ring's lead is not a slow-server artifact —
Goblin holds its own on the wire everyone shares.

### 2. SBE over the ring is 38–50x faster at the median

The comparison uses the fastest socket result on each operation — Goblin's *own* RESP/UDS —
so it compares one server to itself across its conventional and low-latency paths. `GET` is
0.17 µs versus 8.5 µs. The delta is the combined fast path: no kernel on the request path,
no ASCII number parsing, and one cache-line handoff. Against the incumbents the gap is the
same 40–55x.

### 3. The win survives the tail

Even the mean, which is tail-sensitive, keeps the ring roughly 20x ahead: in the fixed
2-second window it completed ~2.7M round trips per op against ~130k on the sockets. This is
not a cherry-picked median.

## Predictability: p99 round-trip latency (microseconds)

| engine / transport | SET | GET | HSET | HGET | ZADD | ZSCORE |
|---|---:|---:|---:|---:|---:|---:|
| Goblin — SBE / ring | 0.25 | 0.22 | 0.25 | 0.26 | 0.28 | 0.23 |
| Goblin — RESP / UDS | 12.21 | 12.29 | 12.48 | 13.79 | 14.02 | 15.32 |
| Redis 7.2.4 — RESP / UDS | 12.93 | 12.24 | 35.42 | 45.02 | 60.41 | 50.73 |
| Redis 8.8 — RESP / UDS | 14.08 | 13.51 | 14.10 | 13.78 | 14.26 | 14.15 |
| Valkey 9.1 — RESP / UDS | 13.54 | 12.85 | 13.83 | 13.38 | 14.05 | 13.69 |
| Dragonfly — RESP / UDS | 12.09 | 11.29 | 12.60 | 12.48 | 12.85 | 12.48 |

The ring's p99 is ~0.25 µs — within 0.06 µs of its median. The socket engines sit at
12–15 µs, and Redis 7.2.4 spikes to 35–60 µs on the hash and zset operations. Low latency
and a tight tail travel together here.

## Why low cardinality is the honest choice

At 10 members or fields, per-op CPU is minimal and dominated by framing and transport rather
than structure traversal. That isolates the transport, which is what this benchmark is about.
Larger collections add data-structure cost to every engine roughly equally and *dilute* the
transport delta — they measure something else. If you want the large-collection story, that is
[BENCHMARKS.md](BENCHMARKS.md); this page is deliberately the transport story.

## Reproducing

Run it on the benchmark host with the engine binaries in place:

```sh
# build goblin-core (Release) and compile the header-only probe
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel --target goblin_core_server -j
g++ -std=c++23 -O3 -march=native -DNDEBUG -Iinclude -Isbe/generated \
    benchmarks/latency_shootout.cpp -o latency_shootout

# drive every engine (paths are env-overridable; see the script header)
GOBLIN=build-rel/goblin-core bash benchmarks/latency_shootout.sh
```

Configuration, matching the other Goblin benchmark reports:

- Redis and Valkey are launched with [`benchmarks/redis-parity.conf`](benchmarks/redis-parity.conf):
  persistence off, `io-threads 1`, `activedefrag no` (so no background defragmenter adds jitter),
  listpack thresholds pinned to the shared defaults, then `--unixsocket <s> --port 0` to serve
  UDS only.
- Dragonfly runs with `--proactor_threads=1` (a single shard — the fair single-core comparison;
  it manages its own CPU affinity and is not externally pinned) over `--unixsocket=<s>`.
- Goblin serves RESP on `--unixsocket <s>` and SBE on a `--ring`. The ring's server busy-polls
  on one core, the client on a neighbour.
- Each op is one round trip; the median is reported over a 2-second window per op.

## Caveats

- The median is the headline. The mean is tail-sensitive on a shared host; it is recorded
  (`n` reflects it) but is not the figure to read.
- The ring is a Goblin capability the incumbents do not offer. The RESP/UDS rows are the
  apples-to-apples baseline — same protocol everyone shares — and they show Goblin is
  competitive there before the ring changes the game.
- This is a Linux result. The two-sided busy-poll needs pinned, ideally isolated, cores to
  hold the sub-microsecond median; on a scheduler that parks the spinner the median degrades.
  The macOS status is covered in [RING-BENCHMARKS.md](RING-BENCHMARKS.md).

The wire is documented in [`docs/sbe-protocol.md`](docs/sbe-protocol.md), the transport in
[`docs/ring-buffers.md`](docs/ring-buffers.md), and the single-engine ring latency numbers in
[RING-BENCHMARKS.md](RING-BENCHMARKS.md).
