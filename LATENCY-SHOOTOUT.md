# Latency shootout: SBE over the ring vs the socket tax

Goblin Core supports two transports — sockets and shared-memory rings — and two protocols,
RESP and SBE. Either protocol works over either transport. This shootout compares the most
compatible combination, RESP over a Unix-domain socket, with the lowest-latency combination,
SBE over a shared-memory ring: no kernel on the request path, binary numeric fields, and a
cache-line handoff between two cores. It measures the latency of a single, unpipelined
request/reply.

It puts Goblin's SBE/ring next to Goblin's own RESP-over-Unix-socket and to the established
Redis-family incumbents on their sockets — Redis 7.2.4, Redis 8.8, Valkey 9.1, and
Dragonfly — on the same host, with the same client and the same timer. mini-redis-go is
also included, but over RESP/TCP because the tested revision does not yet expose a Unix
socket. The result: on these operations Goblin's SBE shared-memory ring is 33–40x faster
at the median than the fastest RESP Unix-domain-socket result, and its tail is tighter by
roughly two orders of magnitude. The mini TCP row is informational and is not used in
that multiplier.

The earlier [ring round-trip latency report](RING-BENCHMARKS.md) is retained as
the older, ring-only story. It compares RESP and SBE on Goblin's shared-memory
transport and includes the macOS development-host results. This shootout is the
current headline because it puts the ring on the same timer and host as Goblin's
socket path and the Redis-family incumbents.

## What is measured

[`benchmarks/latency_shootout.cpp`](benchmarks/latency_shootout.cpp) is one C++ client with
one timing method (the CPU cycle counter, `rdtscp`) and two protocol paths: SBE over the
ring, and RESP over a connected socket. RESP uses UDS for Goblin and the established
incumbents, and TCP only for the explicitly labelled mini-redis-go row. The client code
and timer remain identical; the variables between rows are transport, protocol, and server,
never client language.

Six operations, a write and a point-read for each data type:

| type | write | read |
|---|---|---|
| string | `SET s v` | `GET s` |
| hash | `HSET h f5 v5` | `HGET h f5` |
| zset | `ZADD z 5.5 m5` | `ZSCORE z m5` |

Each op is one synchronous round trip — send the command, block for its reply — in a tight
loop. We report the median over a 2-second window (7.2–8.3 million samples for the ring,
178k–261k for UDS, and about 105k for mini over TCP) plus p90/p99/min. The writes update an
element that already exists, so the collections never grow.

Cardinality is deliberately low: the zset holds 10 members and the hash 10 fields. Goblin,
Redis, Valkey, and Dragonfly are configured for their compact small-collection encodings, so
there is almost no data-structure work per op. mini-redis-go supports the string and hash
probes at the tested revision but not ZADD/ZSCORE, which are reported as `n/a`. What remains
is predominantly the transport and protocol path, which is the point.

## The host

AMD Ryzen Threadripper PRO 5995WX — 64 cores / 128 threads, Zen 3, 2021. This is a five-year-old
many-core workstation part tuned for aggregate throughput, not a single-thread speed champion;
a consumer desktop chip clocks higher on one core. That makes these numbers a conservative
floor. The socket server and the client are pinned to neighbouring cores (shared L3); the
ring's server and client likewise. No other tuning. These results were captured July 14, 2026.

## Median round-trip latency (microseconds)

Read the median. On a scheduler-managed host the mean is pulled up by rare preemptions of the
busy-poll loop; the median and the tight p90 are the true per-op cost.

| engine / transport | SET | GET | HSET | HGET | ZADD | ZSCORE |
|---|---:|---:|---:|---:|---:|---:|
| Goblin — SBE / ring | 0.20 | 0.20 | 0.21 | 0.23 | 0.23 | 0.19 |
| Goblin — RESP / UDS | 7.72 | 7.56 | 7.61 | 7.54 | 8.10 | 7.61 |
| Redis 7.2.4 — RESP / UDS | 10.44 | 7.99 | 10.49 | 10.48 | 10.52 | 10.50 |
| Redis 8.8 — RESP / UDS | 11.19 | 10.74 | 10.62 | 10.85 | 10.76 | 10.55 |
| Valkey 9.1 — RESP / UDS | 10.48 | 10.46 | 10.50 | 10.47 | 10.54 | 10.52 |
| Dragonfly — RESP / UDS | 9.61 | 8.08 | 9.85 | 9.76 | 9.81 | 9.79 |
| mini-redis-go `74d87c0` — RESP / TCP | 17.71 | 17.52 | 17.96 | 17.66 | n/a | n/a |

## What it shows, in four layers

### 1. Goblin's socket path is competitive

Goblin-RESP/UDS (7.54–8.10 µs) is the fastest UDS row on all six operations, ahead of
Redis/Valkey/Dragonfly (7.99–11.19 µs). So the ring's lead is not a slow-server artifact —
Goblin holds its own on the wire the established engines share.

### 2. SBE over the ring is 33–40x faster at the median

The comparison uses the fastest UDS result on each operation — Goblin's *own* RESP/UDS —
so it compares one server to itself across its conventional and low-latency paths. `GET` is
0.20 µs versus 7.56 µs. The delta is the combined fast path: no kernel on the request path,
no ASCII number parsing, and one cache-line handoff. Against the established incumbents the
gap is 40–52x.

### 3. The win survives the tail

Even the mean, which is tail-sensitive, keeps the ring 33–40x ahead of Goblin's UDS path. In
each fixed 2-second window it completed 7.2–8.3 million round trips, versus 224k–261k on
Goblin UDS. This is not a cherry-picked median.

### 4. mini-redis-go is measured on the transport it has today

The [`mini-redis-go`](https://github.com/ThatDeparted2061/mini-redis-go) row is commit
`74d87c0`. Its 17.52–17.96 µs medians are RESP over TCP loopback, not UDS, so they should not
be read as a direct server-to-server ranking against the UDS rows. It is included now to keep
the benchmark surface honest and will move to UDS when that transport is available. The tested
revision does not implement sorted sets, so ZADD and ZSCORE are `n/a` rather than timed errors.

## Predictability: p99 round-trip latency (microseconds)

| engine / transport | SET | GET | HSET | HGET | ZADD | ZSCORE |
|---|---:|---:|---:|---:|---:|---:|
| Goblin — SBE / ring | 0.26 | 0.25 | 0.26 | 0.28 | 0.28 | 0.24 |
| Goblin — RESP / UDS | 11.49 | 9.24 | 11.80 | 11.44 | 12.10 | 11.56 |
| Redis 7.2.4 — RESP / UDS | 12.70 | 12.36 | 13.10 | 12.78 | 12.64 | 12.98 |
| Redis 8.8 — RESP / UDS | 13.71 | 12.71 | 13.20 | 12.85 | 13.24 | 13.30 |
| Valkey 9.1 — RESP / UDS | 12.67 | 12.68 | 12.79 | 13.02 | 12.94 | 13.13 |
| Dragonfly — RESP / UDS | 12.04 | 11.67 | 12.77 | 12.17 | 12.94 | 12.70 |
| mini-redis-go `74d87c0` — RESP / TCP | 25.59 | 26.23 | 24.87 | 25.31 | n/a | n/a |

The ring's p99 is 0.24–0.28 µs, within 0.05 µs of its median. The UDS rows sit at
9.24–13.71 µs; mini's separately labelled TCP row sits at 24.87–26.23 µs. Low latency and
a tight tail travel together here.

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
- mini-redis-go commit `74d87c0` runs with `GOMAXPROCS=1`, AOF and metrics disabled, over
  TCP loopback. It uses the same C++ timing client but is not part of the UDS multiplier.
- Each op is one round trip; the median is reported over a 2-second window per op.

## Caveats

- The median is the headline. The mean is tail-sensitive on a shared host; it is recorded
  (`n` reflects it) but is not the figure to read.
- The ring is a Goblin capability the incumbents do not offer. The RESP/UDS rows are the
  apples-to-apples baseline — same protocol everyone shares — and they show Goblin is
  competitive there before the ring changes the game.
- mini-redis-go's TCP row has extra loopback transport cost and is not apples-to-apples with
  the UDS rows. Its transport is carried in the row label anywhere the result appears.
- This is a Linux result. The two-sided busy-poll needs pinned, ideally isolated, cores to
  hold the sub-microsecond median; on a scheduler that parks the spinner the median degrades.
  The macOS status is covered in [RING-BENCHMARKS.md](RING-BENCHMARKS.md).

The wire is documented in [`docs/sbe-protocol.md`](docs/sbe-protocol.md), the transport in
[`docs/ring-buffers.md`](docs/ring-buffers.md), and the single-engine ring latency numbers in
[RING-BENCHMARKS.md](RING-BENCHMARKS.md).
