# XLIO Ultra Command Latency

## Summary

This benchmark compares seven small Goblin Core operations over four transport
paths: a local shared-memory RESP ring, ordinary kernel TCP across a direct
100 Gb/s Ethernet link, a native XLIO Ultra server with an ordinary kernel TCP
client, and native XLIO Ultra on both ends. Every path uses the same C++ RESP2
probe, pipeline depth one, fixtures, operation order, warmup, and sample count.

With native XLIO on both endpoints, median cross-host round trips were
4.98-8.37 microseconds. That is a conservative 5.3-9.1x improvement over
ordinary kernel TCP on this hardware, while the asymmetric run demonstrates
that an ordinary TCP client can still talk to the accelerated server. The local
shared-memory RESP ring remains the fastest path at 0.87-2.77 microseconds.

## Test paths

| Label | Server | Client | Route |
|---|---|---|---|
| `ring-resp` | RESP shared-memory ring | RESP shared-memory ring | Local to `butterfly` |
| `kernel-kernel` | Linux kernel TCP | Linux kernel TCP | `butterfly` to `rain` |
| `xlio-kernel` | Native XLIO Ultra | Linux kernel TCP | `butterfly` to `rain` |
| `xlio-xlio` | Native XLIO Ultra | Native XLIO Ultra | `butterfly` to `rain` |

The benchmark seeds one string, a ten-field hash, and a ten-member sorted set.
`SET`, `HSET`, and `ZADD` then overwrite an existing value with the same bytes;
`GET`, `HGET`, and `ZSCORE` read those entries. `PING` measures the smallest
complete command path.

Each operation has 100,000 warmup round trips followed by 1,000,000 measured
round trips. There is exactly one request in flight, so the numbers are
synchronous command latency rather than pipelined throughput.

## NUMA and hardware control

Both systems are Dell PowerEdge R820 servers with four Intel Xeon E5-4657L v2
sockets and four NUMA nodes. Each uses an NVIDIA/Mellanox ConnectX-5
MCX515A-CCAT at 100 Gb/s full duplex over a direct DAC:

| Endpoint | Address | NIC | NIC NUMA node | Exact CPU |
|---|---|---|---:|---:|
| Server (`butterfly`) | `10.100.0.1` | `enp68s0np0` | 1 | 5 |
| Network client (`rain`) | `10.100.0.2` | `enp68s0np0` | 1 | 5 |
| Local ring client (`butterfly`) | Shared memory | same host | 1 | 9 |

The harness reads `/sys/class/net/enp68s0np0/device/numa_node`, verifies that
each selected CPU belongs to that node, and aborts on any mismatch. Server and
client commands are wrapped with both
`numactl --cpunodebind=1 --membind=1` and an exact `taskset`. Goblin Core also
receives `--cpu 5 --numa 1`. The benchmark therefore cannot silently migrate a
process or its allocations to a remote socket.

Both adapters used firmware `16.35.8008` and the inbox `mlx5_core` driver on
Linux 5.15. Adaptive RX/TX coalescing remained enabled at the host defaults;
the CPU governor was `schedutil`.

## Median latency

All values are microseconds. Lower is better.

| Operation | Local RESP ring | Kernel / kernel | XLIO server / kernel client | XLIO / XLIO | Conservative XLIO gain |
|---|---:|---:|---:|---:|---:|
| `PING` | 0.865 | 45.240 | 30.759 | 4.976 | 9.09x |
| `SET` | 1.895 | 91.860 (full run) | 41.668 | 6.974 | 5.98x (repeat) |
| `GET` | 1.063 | 39.096 | 23.399 | 5.501 | 7.11x |
| `HSET` | 2.456 | 42.321 | 32.834 | 7.482 | 5.66x |
| `HGET` | 1.126 | 39.766 | 32.259 | 5.715 | 6.96x |
| `ZADD` | 2.769 | 44.438 | 41.331 | 8.373 | 5.31x |
| `ZSCORE` | 1.161 | 39.413 | 37.155 | 5.499 | 7.17x |

**Full-suite SET note.** The full-suite kernel `SET` interval was anomalous: its median was 91.860
microseconds while the neighboring kernel operations were near 40-45
microseconds. A separate one-million-sample `SET` repeat, under the same hard
NUMA binding, measured 41.692 microseconds at p50, 60.655 at p75, and 69.362 at
p99. The full-suite row remains in the raw results rather than being silently
replaced.

**SET gain note.** The `SET` gain uses the 41.692-microsecond repeat as the conservative kernel
baseline. Dividing by the anomalous full-suite interval would report 13.17x.

## Native XLIO tail

All values are microseconds from the `xlio-xlio` run.

| Operation | p50 | p99 | p99.9 | p99.99 | Maximum |
|---|---:|---:|---:|---:|---:|
| `PING` | 4.976 | 5.743 | 17.116 | 25.261 | 6,053.518 |
| `SET` | 6.974 | 7.717 | 19.944 | 29.478 | 6,062.927 |
| `GET` | 5.501 | 6.210 | 17.678 | 26.332 | 6,002.567 |
| `HSET` | 7.482 | 8.247 | 20.837 | 29.794 | 6,063.457 |
| `HGET` | 5.715 | 6.317 | 17.851 | 27.692 | 6,097.222 |
| `ZADD` | 8.373 | 9.172 | 21.229 | 31.514 | 6,000.464 |
| `ZSCORE` | 5.499 | 6.074 | 17.588 | 26.727 | 290.208 |

The common distribution is tight through p99. The roughly 6 ms maxima seen in
six rows are isolated host scheduling events on these non-isolated,
`schedutil` systems, not representative wire latency. The p99.99 values show
the high tail without allowing a single event to define the transport.

## Buffers and ownership

| Path | Recorded buffer configuration |
|---|---|
| Shared-memory ring | 2 MiB request ring and 2 MiB completion ring |
| Kernel TCP | Linux `SO_SNDBUF=87040`, `SO_RCVBUF=131072` |
| XLIO server / kernel client | Same kernel-client socket buffers |
| XLIO / XLIO | 65,536-byte client work buffer, inline-copy TX, XLIO-owned zero-copy RX |

Each `--xlio` listener owns a separate Ultra polling group. Mixed ring, XLIO,
RDMA, and ExaSock targets retain literal command-line order as strict priority;
a busy earlier target can starve a later one. Kernel sockets receive a sparse
pass only when the polled targets are idle.

## Reproduction and raw data

The orchestration script builds the native transport, verifies NIC and CPU NUMA
membership on both hosts, runs the four matched cases, and records its complete
environment:

```bash
bash benchmarks/xlio_latency.sh
```

Published artifacts:

- [All latency samples summarized as percentiles](https://github.com/adamdeprince/goblin-core/blob/main/benchmarks/xlio_latency_2026-07-22.csv)
- [Transport and buffer configuration](https://github.com/adamdeprince/goblin-core/blob/main/benchmarks/xlio_latency_2026-07-22-config.csv)
- [Hosts, firmware, NUMA binding, link, and coalescing metadata](https://github.com/adamdeprince/goblin-core/blob/main/benchmarks/xlio_latency_2026-07-22-metadata.txt)
- [Isolated kernel TCP SET repeat](https://github.com/adamdeprince/goblin-core/blob/main/benchmarks/xlio_kernel_set_repeat_2026-07-22.csv)

The measured tree was commit
`810df152395bbe2d7d296a25f4b4e25534675cca`.
