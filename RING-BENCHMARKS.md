# Ring round-trip latency

Goblin Core's lowest-latency path is a shared-memory ring: no kernel on the request path,
just a cache-line handoff between two cores. This page measures the round-trip latency of
that path for the [SBE binary wire](docs/sbe-protocol.md) and, for contrast, RESP over the
same ring.

## What is measured

[`benchmarks/sbe_vs_resp_ring.cpp`](benchmarks/sbe_vs_resp_ring.cpp) starts one server that
exposes two rings — RESP and SBE — with the client and server pinned to separate cores
(where the OS allows). For each operation it times a synchronous round trip — send one
command, read its reply — in a tight loop, and reports the **median** over a 3-second window
(millions of samples) plus the p90/p99 tail.

Operations: `PING` (pure framing, nothing to parse), `ZADD` of 1 and 10 members (native
`double` scores on SBE vs ASCII text on RESP), and `HSET` of 1 and 10 fields.

The median is the figure to read. On a scheduler-managed host the *mean* is dominated by
rare preemptions of the busy-poll loop — it can sit several times above the median — so the
median and the tight p90 are the true per-op cost.

## AMD Ryzen Threadripper PRO 5995WX

64 cores / 128 threads, Zen 3, up to 4.575 GHz. Worth stating plainly: this is a many-core
**workstation** part, tuned for aggregate throughput, **not** a single-thread speed
champion — a consumer desktop chip clocks higher on one core. That makes these numbers a
conservative floor: the ring turns in sub-microsecond round trips on a CPU that is not
trying to win single-thread benchmarks.

Median round-trip, client and server pinned to separate cores:

| operation | SBE | RESP |
|---|---:|---:|
| `PING` | 0.13 µs | 0.26 µs |
| `ZADD`, 1 member | 0.21 µs | 0.43 µs |
| `ZADD`, 10 members | 0.93 µs | 1.60 µs |
| `HSET`, 1 field | 0.23 µs | 0.41 µs |
| `HSET`, 10 fields | 0.71 µs | 1.24 µs |

SBE runs ~1.7–2.0× faster than RESP over the same ring — the delta is pure protocol: native
numbers instead of ASCII, and jump-table dispatch on the template id rather than tokenizing.
The single-element ops hold p99 within ~0.1 µs of the median.

## Apple M4 Max

12 performance cores, macOS. No `taskset` (and `THREAD_AFFINITY_POLICY` is a no-op on
Apple Silicon), so client and server run unpinned. The busy-poll path is tuned for Darwin
instead: 128-byte isolation of the four ring index words (Apple L1/L2 lines + 128-byte
prefetch), `USER_INTERACTIVE` QoS on both processes, and `cpu_relax` that does **not**
emit `yield` (a yield-spin is treated as low-value work and parked for ~10–15 µs).

Run the bench under `caffeinate -i` so App Nap / idle throttling does not sit on the
spinners. Numbers below are SBE median round-trip µs, local M4 Max, Release build.

Single-element — clean:

| operation | p50 | p99 | min |
|---|---:|---:|---:|
| `PING` | 0.17 | ~8 | 0.04 |
| `ZADD`, 1 | 0.21 | ~8 | 0.10 |
| `HSET`, 1 | 0.17 | ~1 | 0.10 |

A synchronous unpipelined round trip is reliably sub-250 ns at the median. The p99 still
shows occasional ~8–15 µs scheduler holes (unpinned dual busy-poll has no isolcpus
equivalent); they are rare enough that single-element medians stay honest.

Ten-element — partially open:

| operation | p50 | note |
|---|---:|---|
| `ZADD`, 10 | ~1.0 | usable; p90 still sees the ~15 µs cliff |
| `HSET`, 10 | ~14–15 | median sits on the cliff; `min` ≈ 0.6 µs proves the ring can do it |

Do not publish the HSET-10 median as “ring latency” — it is a scheduler artifact (the min
shows the path is fine when both threads stay hot). Two-sided sub-µs busy-poll without
core isolation remains a Linux strength; on macOS the single-element path is solved, and
batched ops are still under investigation (adaptive spin-then-timeout is in the tree as a
tail bound, not a full cure).

Even the stalled ~15 µs numbers stay at Unix-domain-socket cost (~10 µs in
[BENCHMARKS.md](BENCHMARKS.md)), never worse. Single-element is ~50–75× under a socket.
