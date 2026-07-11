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

_Pending — measured with the machine in high-power mode. (macOS has no `taskset`, so the
client and server run unpinned; the busy-poll needs high-power mode to keep the scheduler
from throttling it.)_
