# Aeron UDP between rain and butterfly, 2026-08-24

This directory records depth-one Goblin Core latency across two physical
network paths between `rain` and `butterfly`:

| Path | Server / client | Network stack |
|---|---|---|
| 10 GbE | `butterfly` `192.168.1.46` / `rain` `192.168.1.134` | Aeron UDP through Linux kernel UDP on `eno1` |
| 100 GbE | `butterfly` `10.100.0.1` / `rain` `10.100.0.2` | Aeron UDP Media Drivers preloaded with XLIO 3.61.2 on `enp68s0np0` |

Each RESP/path and SBE/path case received fresh server and client Media
Drivers plus a fresh Goblin server. `PING`, `SET`, and `GET` each contain
100,000 measured round trips after 10,000 warm-up requests, with one request
outstanding at a time. The complete matrix ran twice. `SET` and `GET` used the
same key and 16-byte value as the local `naamah` matrix.

## Two-run means

Median round-trip latency, in microseconds:

| Path / protocol | PING | SET | GET |
|---|---:|---:|---:|
| 10 GbE kernel / RESP | 49.085 | 50.734 | 49.494 |
| 10 GbE kernel / SBE | 48.099 | 49.725 | 48.759 |
| 100 GbE XLIO / RESP | 7.378 | 8.764 | 7.718 |
| 100 GbE XLIO / SBE | 7.172 | 8.282 | 7.427 |

p99 round-trip latency, in microseconds:

| Path / protocol | PING | SET | GET |
|---|---:|---:|---:|
| 10 GbE kernel / RESP | 62.713 | 66.385 | 64.168 |
| 10 GbE kernel / SBE | 61.019 | 64.340 | 62.313 |
| 100 GbE XLIO / RESP | 16.021 | 19.451 | 17.753 |
| 100 GbE XLIO / SBE | 14.895 | 18.955 | 16.932 |

For these tiny depth-one messages, the 100 GbE userspace configuration cut
PING p50 by 6.7x and p99 by roughly 4x. This is a comparison of two complete
lab paths, not a claim that link rate alone caused the difference: the 10 GbE
path uses Broadcom `bnx2x`, kernel UDP, and 24/48 microsecond RX/TX coalescing;
the 100 GbE path uses ConnectX-5, XLIO userspace UDP, and a different NUMA
node. The full p99.9 values are retained in `aggregate.csv` and
`latency.csv`.

## Hosts, affinity, and polling

Both machines are Dell PowerEdge R820 servers with four 12-core Intel Xeon
E5-4657L v2 sockets, 96 logical CPUs, and four NUMA nodes. They reported the
`schedutil` governor. The 10 GbE NICs and CPUs are on node 0; the direct
100 GbE ConnectX-5 NICs and CPUs are on node 1.

On each endpoint, the measuring or serving thread, Aeron client conductor, and
Media Driver conductor/receiver/sender occupied distinct physical cores:

| Path | Application | Client conductor | Driver conductor / receiver / sender | XLIO helper |
|---|---:|---:|---:|---:|
| 10 GbE, node 0 | 0 | 16 | 4 / 8 / 12 | n/a |
| 100 GbE, node 1 | 5 | 21 | 9 / 13 / 17 | 25 |

The C++ Aeron conductors and Media Driver conductors used `spin`; driver sender
and receiver agents used `noop`, meaning that they immediately started their
next duty cycle. Network publications sent at most two messages per send. This
is a continuously polled latency profile and consumes the assigned cores when
idle.

`rain` and `butterfly` share `/home` over NFS. Goblin and Aeron were built once
on `rain`, staged under a shared read-only prefix, hashed from both hosts, and
reused byte-for-byte. Aeron directories, process logs, NIC counters, and other
runtime state lived under each host's local `/tmp`. The build was read and
hashed before the measured cases, warming its executable pages.

## What “100 GbE userspace” means here

The 100 GbE cases preload `libxlio.so` into the two external Aeron C Media
Drivers. Goblin and the benchmark probe still communicate with their local
drivers through Aeron's shared-memory client protocol. XLIO intercepts the
drivers' UDP sockets, so this is Aeron UDP over XLIO's userspace path—not
Goblin's separate native XLIO Ultra TCP transport.

The XLIO profile used `XLIO_SPEC=latency`, anonymous memory, infinite initial
unicast polling, no kernel-FD poll ratio, and no progress-engine drain thread.
`XLIO_EXCEPTION_HANDLING=2` returned errors instead of silently undoing an
unsupported offload operation. XLIO's helper thread was pinned to CPU 25.

Attaching `xlio_stats` is itself an uncontrolled concurrent process, so the
reported latency runs did not include it. The separate `qualification/` run
used the identical binaries, routes, driver settings, and CPU layout with
`XLIO_STATS_DURING_RUN=1`. Statistics from both live Media Drivers recorded
nonzero `Tx Offload` and `Rx Offload` packet counts. Its latency rows are not
included in either reported run or the aggregate.

## Aeron channels

Both directions use Aeron response channels. The request endpoint and response
control endpoint are addresses on `butterfly`. The response publication binds
the control address on the server; `rain` uses the same response-control URI as
its remote control target. Aeron's response correlation learns the client's
return destination. The per-case `channels.txt` files preserve the exact URIs
and stream IDs.

## Provenance and files

The benchmark used Goblin commit
`9ddb3ed3ea29dfbf458a388ca3e5143a3ffcdd76`, Aeron 1.51.0 commit
`9773cba37e4b88b2b7eb9460c4e0050267b71d28`, and XLIO 3.61.2. Canonical
artifact hashes were:

| Artifact | SHA-256 |
|---|---|
| `goblin-core` | `0121d96c2349cfdd68d75a90901c54cb1ee0a3d5f1ceede1c4ca6188d0cd4712` |
| latency probe | `7cae49c136345a993917f9c5d32bdafd323802af7d68c9910efab55e434fb52f` |
| `aeronmd_s` | `43c4c98c4b4d4a23164be94389f14492f38cb753cfd1137272fa9312f4defac5` |
| `libxlio.so` | `0472beaec85b6299aad9fb47377fc6fc09ee4bd8eb22a2a0d0025559f6625e2f` |

- `latency.csv` contains all 24 reported rows; `aggregate.csv` contains their
  two-run p50, p99, and p99.9 means.
- `run-1/` and `run-2/` preserve raw per-case CSVs, human-readable probe
  output, server/driver logs, thread affinities, channel URIs, and NIC counters.
- `metadata.txt` records the original dual-path controller run and both NIC
  topologies. `metadata-100gbe-clean.txt` records the clean 100 GbE rerun with
  `xlio_stats_during_run=0`.
- `qualification/` contains the independent live XLIO statistics check.
- `SHA256SUMS` covers every other file in this directory.

## Reproduction

The publication-quality controller command is:

```console
env RUNS=2 SAMPLES=100000 WARMUP=10000 \
    XLIO_STATS_DURING_RUN=0 \
    OUT_DIR=/tmp/aeron-network-results \
    bash benchmarks/aeron_network_latency.sh
```

The archived clean 100 GbE rerun added `PATHS=100gbe`. To independently prove
offload without treating that run as latency data:

```console
env RUNS=1 PATHS=100gbe PROTOCOLS=RESP \
    SAMPLES=100000 WARMUP=10000 XLIO_STATS_DURING_RUN=1 \
    OUT_DIR=/tmp/aeron-network-xlio-qualification \
    bash benchmarks/aeron_network_latency.sh
```
