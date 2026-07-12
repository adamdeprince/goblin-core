# Sub-microsecond round trips: Goblin Core's shared-memory ring

Goblin Core has two transports and two wire protocols. Clients can connect through an
ordinary TCP or Unix-domain socket, or through a shared-memory ring. Independently, either
transport can carry RESP or SBE. The first bytes sent by an endpoint select the protocol;
the transport does not dictate it.

The shared-memory ring is the transport a normal Redis does not have. There is no kernel on
its request path. The client writes a request into a memory-mapped ring buffer, the server
busy-polls it out, runs it, and writes the reply back the same way — one cache-line handoff
between two cores, no syscalls. SBE is the binary protocol borrowed from FIX/HFT tooling:
scores and counts travel as native `double`s and `int64`s, so neither side parses or
re-stringifies numbers, and the server dispatches with a jump table on the message type.
The measurements below run both SBE and RESP over the same ring, separating the protocol
cost from the transport cost.

The result is HFT-adjacent latency for a key-value store — a full request/reply round trip
in well under a microsecond.

## The numbers

Host: a 64-core AMD Ryzen Threadripper PRO 5995WX, Linux, client and server pinned to
separate cores. Median round trip over a 3-second window, timed with the CPU cycle counter
(`rdtscp`). SBE and RESP over the same ring:

| operation | SBE | RESP |
|---|---:|---:|
| `PING` (pure framing) | 0.15 µs | 0.25 µs |
| `HSET`, 1 field | 0.22 µs | 0.41 µs |
| `ZADD`, 1 member | 0.22 µs | 0.42 µs |
| `HSET`, 10 fields | 0.71 µs | 1.25 µs |
| `ZADD`, 10 members | 0.92 µs | 1.59 µs |

A single-field `HSET` or single-member `ZADD` round trip is about 220 nanoseconds. A
ten-element write stays under a microsecond. The p99 sits within ~0.1 µs of the median, so
this is the steady state, not a cherry-picked best case. SBE runs roughly 1.7–2x faster than
RESP over the identical ring — pure protocol, no ASCII number parsing.

For scale: a Unix-domain-socket round trip is around 10 µs. This is 20–70x under that. You
are not paying the socket tax.

## A five-year-old workstation, not a single-thread screamer

This changes how you should read the numbers. The 5995WX is a 2021 part — five years old —
and it is a 64-core workstation CPU tuned for aggregate throughput, not single-thread speed.
A modern desktop chip clocks a good deal higher on one core. So these sub-microsecond round
trips are a conservative floor, not a hero-CPU cherry-pick. The latency comes from the
design — no kernel, no parsing, one cache-line handoff — not from an exotic processor. On
newer or single-thread-focused silicon there is headroom, not a ceiling. (In fact, see the
macOS section: an M4 laptop's single-op latency already beats this box.)

## Unpipelined, and still fast

Here is why the number matters. Most systems only look fast because they pipeline: batch a
few hundred requests, amortize the round trip, report throughput. That is real, but it means
every naive, synchronous, one-at-a-time call pays the full latency. When that latency is
~10 µs, unpipelined code is slow, and you are forced to batch to get performance.

At ~220 ns, you are not. You can write the dumb, obvious thing — send a request, wait for
the reply, do the next one — and still be fast. Low latency buys you the freedom to not
pipeline. That is the pitch: it lets you do the simple thing.

## macOS: fast, until it isn't

People develop on Macs, so it matters that it runs there. The honest status:

On an Apple M4 Max, single-element round trips are excellent — `PING` 0.13 µs, a single
`ZADD`/`HSET` around 0.13–0.17 µs, p99 ~0.25 µs. The laptop's fast single core actually
beats the workstation on these (0.13 vs 0.15 µs). For interactive development it is great.

But it is not reliable for the low-latency workload yet. Apple Silicon has no core pinning —
there is no `taskset`, and the thread-affinity API is a no-op — so two 100%-CPU busy-poll
threads cannot be given private cores the way Linux does with `isolcpus` + `taskset`. The
macOS scheduler parks the spin loop, and on ten-element operations that surfaces as a stall:
a 10-field `HSET` sits at ~13 µs median on the Mac, even though the same operation's best
case is 0.43 µs. We tried the obvious levers — real-time thread policies, high fixed
priority, QoS, and the 128-byte cache lines the M-series needs. QoS plus 128-byte lines
cleaned up the single-element tail nicely (p99 went from ~14 µs to ~0.25 µs), but the
ten-element scheduler stall is still open research.

So the recommendation is simple: for low-latency work, run it on Linux, where you can pin
the poll threads to isolated cores and get deterministic sub-microsecond behavior. macOS is
fine to develop against — single ops are fast — but the reliable low-latency story lives on
a pinned Linux box today. We are still chipping at the macOS scheduler problem.

---

The SBE wire is documented in [`docs/sbe-protocol.md`](../docs/sbe-protocol.md), the ring
transport in [`docs/ring-buffers.md`](../docs/ring-buffers.md), and the full latency table in
[`RING-BENCHMARKS.md`](../RING-BENCHMARKS.md).
