# Handoff → Grok: ring-latency benchmark writeups

You're taking a crack at the writeups for the shared-memory ring latency numbers. This
brief has everything: the data, the honest story, the framing rules, and the deliverables.
I (the other agent) measured all of this; do **not** trust me blindly — you can re-run
(see the end) — but the numbers below are current as of this handoff.

## The deliverables

1. **Finish `RING-BENCHMARKS.md`** — it has the naamah (Linux) section; fill in the "Apple
   M4 Max" section honestly (single-element table + the 10-element problem as open
   research). This is the factual doc.
2. **Blog post** in `blogs/` (new file, e.g. `blogs/ring-latency.md`). Match the voice of
   `blogs/lichess-leaderboard.md`.
3. **A LinkedIn post** and **a Facebook post** (short, punchy).

## The honest story (this is the whole point)

- **Linux is the hero.** On a pinned Linux box the ring does clean, reliable sub-microsecond
  round trips, median AND tail. That's the headline.
- **macOS sorta works, kinda, maybe.** Single-element ops are genuinely great on an M4 (a
  synchronous, *unpipelined* round trip is reliably sub-250 ns — that matters, see below).
  But 10-element (batched) ops hit a macOS scheduler stall we have **not** cracked. Say so
  plainly. Frame it as ongoing research, not a solved win. Do not oversell the Mac.
- **Even the imperfect Mac numbers beat a socket.** A Unix-domain-socket round trip is
  ~10 µs (see `BENCHMARKS.md`). The ring's single-element ~0.13 µs is ~75× better; the
  10-element ~1 µs (when it behaves) is ~10× better; and even the scheduler-stalled ~13 µs
  is only *socket-level*, never worse. There is no case where the ring loses to a socket —
  that's the honest "still freaking good" angle for the rough Mac numbers.
- **Why it matters (the pitch):** low latency lets you do "dumb" things like *unpipelined*,
  synchronous request/reply and still be fast. You don't have to batch/pipeline to get
  throughput. The single-element numbers are exactly that story, and they hold on a laptop.

## The data

Benchmark: `benchmarks/sbe_vs_resp_ring.cpp` — one server, two rings (RESP and the SBE
binary wire), client and server on separate cores, per-op round trip timed with the CPU
cycle counter (rdtscp on x86). Median over a 3 s window, millions of samples. The **mean**
is noise (rare busy-poll preemptions); quote the **median**, with p99 for the tail.

### naamah — AMD Ryzen Threadripper PRO 5995WX (64C/128T, Linux) — THE HERO

Client and server pinned to separate cores with `taskset`. Median µs; p99 sits within
~0.1 µs of the median. SBE runs ~1.7–2.0× faster than RESP over the same ring.

| operation | SBE | RESP |
|---|---:|---:|
| `PING` | 0.13 µs | 0.26 µs |
| `ZADD`, 1 | 0.22 µs | 0.43 µs |
| `ZADD`, 10 | 0.93 µs | 1.60 µs |
| `HSET`, 1 | 0.21 µs | 0.41 µs |
| `HSET`, 10 | 0.71 µs | 1.24 µs |

Note this is a many-core *workstation* CPU, not a single-thread speed champion — the
sub-microsecond round trips are a conservative floor, not a hero-CPU cherry-pick.

### Apple M4 Max (12 P-cores, macOS) — "local," sorta works

macOS/Apple Silicon has **no core pinning** (no `taskset`; `THREAD_AFFINITY_POLICY` is a
no-op). Best config found: 128-byte cache lines (M-series L1/L2 line size) + QoS
`USER_INTERACTIVE`, unpinned. SBE, median µs.

Single-element — clean and reliable:

| operation | p50 | p99 | min |
|---|---:|---:|---:|
| `PING` | 0.13 µs | 0.21 µs | 0.017 µs |
| `ZADD`, 1 | 0.17 µs | 0.25 µs | 0.058 µs |
| `HSET`, 1 | 0.17 µs | 0.25 µs | 0.058 µs |

(A 17 ns PING minimum through shared memory, on a laptop. p99 ~0.25 µs — before this config
it was ~14 µs; the fix was QoS + 128-byte lines.)

Ten-element — macOS scheduler-limited, open problem:

| operation | p50 | note |
|---|---:|---|
| `ZADD`, 10 | ~0.8–1.4 µs (variable) | p90 tail ~15 µs |
| `HSET`, 10 | ~13.5 µs (stuck) | but `min` = 0.46 µs — the ring *can* do it; the scheduler parks the busy-poll |

Do not publish the 13.5 µs as a "latency" — it's a scheduler artifact (the `min` proves the
ring itself is fine). Present it as: single-element is solved, 10-element is under
investigation.

## The macOS scheduler problem — what we tried (for the "ongoing research" section)

Two 100%-CPU busy-poll threads (client + server) with no way to pin them to dedicated
cores. What we tried, in order:
- **`THREAD_AFFINITY_POLICY`** (core pinning): no-op on Apple Silicon.
- **`THREAD_TIME_CONSTRAINT_POLICY`** (real-time budget): made it *worse* — its "bounded
  work then sleep" model parks a continuous spin loop; every op collapsed to ~13 µs.
- **`SCHED_FIFO`** max priority: helped the single-element p99, not the 10-element.
- **QoS `USER_INTERACTIVE` + 128-byte cache lines** (current): fixed single-element p99
  (~14 µs → ~0.25 µs) and dropped the PING min to 17 ns; 10-element improved a lot
  (ZADD-10 median 13.5 → ~1 µs) but HSET-10 still stalls — and it's *consistently* the last
  op in a ~30 s dual-busy-poll run, so thermal/sustained-load throttling is a live suspect.
- The idiomatic macOS answer (from real-time-audio practice, e.g. sbooth's CXXRingBuffer,
  which is `[[clang::nonblocking]]` and OS-wakeup-driven, not a spinner) is **adaptive
  spin-then-block** (`os_sync_wait_on_address` / `__ulock` with `ULF_WAIT_ADAPTIVE_SPIN`),
  which would bound the tail to single-digit-µs. But **two-sided sub-µs busy-poll is a
  Linux-only trick** (isolcpus + taskset); Apple Silicon has no equivalent. That's the
  honest ceiling to state.

## Framing rules (follow these)

- **Hero = Linux.** Lead with naamah's clean numbers.
- **Be honest about macOS.** "Sorta works, kinda, maybe" — single-element great, 10-element
  is open research. No spin, no asterisk-hiding.
- **The UDS point.** Even the rough Mac numbers beat a socket (~10 µs UDS). Use it.
- **No bold.** Do not use `**bold**` for emphasis — it reads as an AI tic. Prose and headers only.
- **Social posts: do NOT name the host `naamah`.** Call it "a 64-core AMD Threadripper PRO
  workstation" (the CPU model is fine; the hostname is not). The M4 can be called an M4 /
  MacBook. In the repo docs (`RING-BENCHMARKS.md`), `naamah` is fine.
- Cross-link: the SBE wire is documented in `docs/sbe-protocol.md`; the ring transport in
  `docs/ring-buffers.md`.

## Reference / how to re-run

- Bench source: `benchmarks/sbe_vs_resp_ring.cpp` (covers PING / ZADD-1 / ZADD-10 / HSET-1 /
  HSET-10, RESP and SBE).
- Linux (naamah, over ssh): `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DGOBLIN_CORE_ARCH=avx2 -S .`,
  build `goblin_core_server`, compile the bench with `g++ -std=c++23 -O3 -march=native -DNDEBUG -Iinclude -Isbe/generated`,
  run `<bench> build-rel/goblin-core` (it forks the server pinned to core 2, pins the client to core 3).
- macOS (M4, local): same, `-DCMAKE_BUILD_TYPE=Release`, compile the bench with `/usr/bin/c++ -std=c++23 -O3 -Iinclude -Isbe/generated`.
- Code state (uncommitted, in the working tree as of this handoff): the 128-byte-line + QoS
  changes (`include/goblin/core/ring_buffer.hpp`, macOS-only, no-op on Linux), the rdtscp
  timing in the bench, and your in-flight HSET work. The numbers above already reflect all
  of it. Nothing here is committed yet.
