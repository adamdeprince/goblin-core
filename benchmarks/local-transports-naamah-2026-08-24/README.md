# Local transport latency on naamah, 2026-08-24

This directory records two complete runs of Goblin Core's depth-one local
transport matrix at commit
`9ddb3ed3ea29dfbf458a388ca3e5143a3ffcdd76`. Both runs used Aeron 1.51.0 at
commit `9773cba37e4b88b2b7eb9460c4e0050267b71d28`.

Every RESP/transport and SBE/transport case received a fresh server. Each
`PING`, `SET`, and `GET` statistic contains 100,000 measured round trips after
10,000 warm-up requests. `SET` and `GET` used the key
`goblin:local-transport-latency` and a 16-byte value. There was one outstanding
request at a time, so the figures include an entire client/server round trip,
protocol encoding, and reply decoding.

## Two-run means

Median round-trip latency, in microseconds:

| Protocol / transport | PING | SET | GET |
|---|---:|---:|---:|
| RESP / ring | 0.316 | 0.736 | 0.406 |
| SBE / ring | 0.200 | 0.471 | 0.261 |
| RESP / Aeron IPC | 0.691 | 1.062 | 0.741 |
| SBE / Aeron IPC | 0.481 | 0.832 | 0.601 |
| RESP / Aeron UDP loopback | 12.744 | 13.225 | 12.779 |
| SBE / Aeron UDP loopback | 12.489 | 12.864 | 12.799 |
| RESP / UDS | 8.271 | 10.420 | 10.395 |
| SBE / UDS | 7.204 | 7.634 | 7.349 |

p99 round-trip latency, in microseconds:

| Protocol / transport | PING | SET | GET |
|---|---:|---:|---:|
| RESP / ring | 0.376 | 0.812 | 0.466 |
| SBE / ring | 0.251 | 0.556 | 0.311 |
| RESP / Aeron IPC | 1.904 | 2.390 | 1.994 |
| SBE / Aeron IPC | 1.738 | 2.179 | 1.808 |
| RESP / Aeron UDP loopback | 18.084 | 18.475 | 19.427 |
| SBE / Aeron UDP loopback | 17.198 | 19.302 | 17.929 |
| RESP / UDS | 12.343 | 13.395 | 12.599 |
| SBE / UDS | 8.451 | 9.388 | 8.701 |

p99.9 exposes the remaining store-maintenance tail:

| Protocol / transport | PING | SET | GET |
|---|---:|---:|---:|
| RESP / ring | 0.551 | 15.955 | 0.666 |
| SBE / ring | 0.356 | 15.419 | 0.411 |
| RESP / Aeron IPC | 4.443 | 17.638 | 4.564 |
| SBE / Aeron IPC | 3.046 | 17.193 | 4.153 |
| RESP / Aeron UDP loopback | 23.274 | 32.321 | 23.129 |
| SBE / Aeron UDP loopback | 19.998 | 31.154 | 20.854 |
| RESP / UDS | 15.309 | 29.751 | 15.279 |
| SBE / UDS | 11.522 | 25.664 | 11.827 |

The median ordering is ring, Aeron IPC, UDS, then two-driver Aeron UDP over
Linux loopback. The 16-byte overwrite still periodically compacts an 11-byte
spilled value tail, so `SET` retains a roughly 15--32 microsecond p99.9 across
the transports. It is now a rare tail rather than the 153--158 microsecond UDP
p99 seen with the historical default-backoff driver policy.

## Polling and topology

These runs use the benchmark's continuous latency profile. The C++ Aeron
client conductor and Media Driver conductor use `spin`; driver sender and
receiver agents use `noop`, which immediately begins the next duty cycle. The
driver sends at most two messages per network send. This profile consumes its
pinned cores even when no work is available.

`naamah` is an AMD Ryzen Threadripper PRO 5995WX with 64 physical cores, 128
logical CPUs, and one NUMA node. The server and measuring client used CPUs 2
and 3. Server/client Aeron conductors used CPUs 10 and 11. Driver conductor,
receiver, and sender agents occupied CPUs 4--6 and 7--9. All are distinct
physical cores; SMT siblings begin at CPU 64. The host reported the
`performance` governor and enabled boost, but no CPUs were isolated from the
kernel.

Aeron IPC shared one local Media Driver. Aeron UDP used separate server and
client drivers connected through `127.0.0.1`, so it crossed Linux's UDP
loopback path. The ring and Goblin Aeron subscription paths were busy-polled;
UDS used the server's socket event loop.

## Provenance and files

The source worktree used for the run was clean. Binary SHA-256 hashes were:

| Artifact | SHA-256 |
|---|---|
| `goblin-core` | `24a7988c763e577a9f8d673db5f21bb7a6c7e1c7773812bfd684d66c5e140b9d` |
| latency probe | `9b6edc3f4b4b88d697a703bd6e69cfbad2e668ba936e13b6a92253fa1490acc7` |
| `aeronmd_s` | `87d5df0f4eb50e1d930f19748c96bed21031823557fd24881ecc3d54adb10cd8` |

- `run-1.csv` and `run-2.csv` are the complete statistics emitted by the
  benchmark.
- `aggregate.csv` preserves both runs and their arithmetic p50, p99, and
  p99.9 means.
- `metadata-run-1.txt` and `metadata-run-2.txt` record source, dependency,
  host, polling, and affinity provenance.
- `SHA256SUMS` covers every other file in this directory.

## Reproduction

The two runs used the same clean build and changed only `OUT_DIR`:

```console
env AERON_PREFIX=/tmp/packrat-aeron-d122399.JoXfbh/aeron-prefix \
    BUILD_DIR=/tmp/goblin-aeron-current.3ikFVO/build \
    OUT_DIR=/tmp/goblin-aeron-current.3ikFVO/results-run-1 \
    SKIP_BUILD=1 SAMPLES=100000 WARMUP=10000 \
    /tmp/goblin-aeron-current.3ikFVO/source/benchmarks/local_transport_latency.sh
```

For the repeat, `results-run-1` became `results-run-2`.
