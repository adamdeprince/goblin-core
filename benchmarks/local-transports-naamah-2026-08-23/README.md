# Local transport latency on naamah, 2026-08-23

This directory records two complete runs of the RESP and SBE local-transport
latency matrix added in commit
`d122399a3c78be3a4941f7d4767387b7c070bff2`.

Each case used one depth-one client, 10,000 warm-up requests, and 100,000
measured requests for each of `PING`, `SET`, and `GET`. `SET` and `GET` used
the key `goblin:local-transport-latency` and a 16-byte value. Times are
end-to-end client round trips, including protocol encoding and reply decoding.

## Two-run mean

Median round-trip latency, in microseconds:

| Protocol / transport | PING | SET | GET |
|---|---:|---:|---:|
| RESP / ring | 0.316 | 0.721 | 0.401 |
| SBE / ring | 0.195 | 0.461 | 0.256 |
| RESP / Aeron IPC | 0.496 | 0.907 | 0.626 |
| SBE / Aeron IPC | 0.341 | 0.646 | 0.411 |
| RESP / Aeron UDP loopback | 13.967 | 15.485 | 14.713 |
| SBE / Aeron UDP loopback | 13.926 | 14.112 | 13.977 |
| RESP / UDS | 8.236 | 9.979 | 9.613 |
| SBE / UDS | 7.194 | 7.589 | 7.329 |

p99 round-trip latency, in microseconds:

| Protocol / transport | PING | SET | GET |
|---|---:|---:|---:|
| RESP / ring | 0.366 | 0.812 | 0.456 |
| SBE / ring | 0.251 | 0.556 | 0.311 |
| RESP / Aeron IPC | 1.623 | 2.229 | 1.788 |
| SBE / Aeron IPC | 1.533 | 2.009 | 1.603 |
| RESP / Aeron UDP loopback | 20.384 | 157.655 | 22.658 |
| SBE / Aeron UDP loopback | 19.727 | 154.909 | 20.359 |
| RESP / UDS | 11.983 | 13.080 | 12.449 |
| SBE / UDS | 8.261 | 9.328 | 8.436 |

The median ordering was ring, Aeron IPC, UDS, then Aeron UDP loopback. Aeron
IPC added roughly 0.15--0.23 microseconds over the corresponding ring median.
SBE had a lower median than RESP in every transport/operation pair.

Both runs reproduced an Aeron UDP `SET` p99 around 153--158 microseconds for
both protocols. The benchmark establishes that this tail is repeatable on this
configuration. Subsequent ordered-trace experiments found that the 16-byte SET
value periodically grows and compacts its spilled string tail in the store
arena. The Media Driver's default exponential-backoff agents amplified those
brief pauses into contiguous UDP latency clusters.

## Aeron polling policy

These archived runs used `DEDICATED` Media Driver threads but did not override
Aeron 1.51's default `backoff` idle strategy. `DEDICATED` assigned conductor,
sender, and receiver agents to separate pinned threads; it did not make those
agents poll continuously. The recorded 153--158 microsecond SET p99 therefore
describes the default-backoff policy.

The current benchmark launcher instead defaults to Aeron's low-latency C Media
Driver profile: conductor `spin`, sender and receiver `noop`, and two messages
per network send. Goblin's C++ Aeron client conductor also uses `spin`. In the
controlled follow-up on this host, that policy reduced the same 16-byte Aeron
UDP SET p99 to 18.4 microseconds for RESP and 19.2 microseconds for SBE; it
continuously occupies the pinned C++ client-conductor and Media Driver agent
cores. Checkout the recorded source commit above to reproduce these archived
backoff results exactly.

## Files

- `run-1.csv` and `run-2.csv` are the complete per-run statistics emitted by
  the benchmark.
- `aggregate.csv` places each run's p50 and p99 beside their arithmetic mean.
- `metadata.txt` records source, dependency, binary, host, and affinity
  provenance.

## Isolation and topology

Every protocol/transport pair ran against a freshly started server. The server
main loop was pinned to CPU 2 and the measuring client thread to CPU 3. Aeron
client conductors used CPUs 10 and 11. Driver conductor, sender, and receiver
agents used CPUs 4--6 for the server driver and CPUs 7--9 for the client
driver. These are distinct physical cores on the single NUMA node; SMT siblings
start at CPU 64.

Aeron IPC used one local Media Driver. Aeron UDP used separate server and
client Media Drivers connected through `127.0.0.1`, so its data path crossed
the kernel loopback UDP stack. UDS used a Unix-domain stream socket. The ring
and Goblin Aeron subscription paths were busy-polled, while the Media Driver
agents used the historical backoff policy described above. UDS used the
server's socket event loop.

The machine had no configured isolated CPUs. Its scaling governor reported
`performance`, while frequency boost remained enabled. These are latency
microbenchmarks, not throughput results.

## Reproduction

The two runs used the committed launcher with only build/result locations and
sample counts overridden:

```console
env AERON_PREFIX=/tmp/packrat-aeron-d122399.JoXfbh/aeron-prefix \
    BUILD_DIR=/tmp/packrat-aeron-d122399.JoXfbh/build \
    OUT_DIR=/tmp/packrat-aeron-d122399.JoXfbh/results-full \
    SKIP_BUILD=1 SAMPLES=100000 WARMUP=10000 \
    benchmarks/local_transport_latency.sh
```

For the second run, `OUT_DIR` was changed to `results-full-repeat`.
