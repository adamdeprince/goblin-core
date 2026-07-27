# Libfabric provider matrix over 100 Gb/s

Goblin Core's fastest locally available network path completed Redis-shaped
operations in 6.39 microseconds at the arithmetic-average median. That was SBE
over libfabric `FI_EP_RDM` using `verbs;ofi_rxm` and the automatic inject path.
The same eight operations averaged 90.33 microseconds over ordinary kernel TCP
with RESP2: 14.1x higher latency and 14.0x fewer sequential round trips per
second.

Libfabric's software `tcp` provider also mattered. RESP2 over that provider
averaged 39.59 microseconds at p50, 2.28x lower than RESP2 over the kernel
socket path. This is a provider and software-path result, not a bandwidth
result; every workload deliberately used pipeline depth one.

> **Scope:** this qualifies the Goblin code path intended for AWS EFA, but it
> is not an AWS EFA hardware result. Native provider `efa` was unavailable on
> the local machines. The measured providers were `tcp` and `verbs;ofi_rxm`
> on two decade-old, four-socket Intel Xeon E5-4657L v2 systems over a direct
> 100 Gb/s Mellanox ConnectX-5 Ethernet link. An EFA instance still needs its
> own run.

## Complete ranking

Modes are ranked by the arithmetic average of the eight per-operation medians.
The average p99.99 is likewise an average of reported per-operation
percentiles, not a merged distribution. Sequential operations per second is
the reciprocal of each operation's observed mean, averaged across operations.

| Rank | Transport/provider | Wire | Send path | Avg p50 (us) | Avg mean (us) | Avg seq. ops/s | Avg p99.99 (us) | Worst max (us) |
|---:|---|---|---|---:|---:|---:|---:|---:|
| 1 | `verbs;ofi_rxm` | SBE | auto-inject | 6.39 | 6.51 | 156,498 | 30.83 | 952.81 |
| 2 | `verbs;ofi_rxm` | SBE | `fi_send` | 6.67 | 6.69 | 152,576 | 30.64 | 995.31 |
| 3 | `verbs;ofi_rxm` | RESP2 | auto-inject | 7.38 | 7.56 | 134,388 | 32.31 | 973.51 |
| 4 | `verbs;ofi_rxm` | RESP2 | `fi_send` | 7.69 | 7.80 | 130,176 | 32.63 | 983.07 |
| 5 | libfabric `tcp` | SBE | `fi_send` | 38.12 | 38.48 | 26,268 | 76.80 | 200.13 |
| 6 | libfabric `tcp` | SBE | auto-inject | 39.09 | 39.48 | 26,014 | 77.05 | 218.95 |
| 7 | libfabric `tcp` | RESP2 | auto-inject | 39.59 | 40.17 | 25,084 | 73.73 | 217.58 |
| 8 | libfabric `tcp` | RESP2 | `fi_send` | 39.82 | 40.19 | 25,075 | 75.47 | 204.38 |
| 9 | Kernel TCP | SBE | kernel send | 61.71 | 62.62 | 16,079 | 116.17 | 1,127.29 |
| 10 | Kernel TCP | RESP2 | kernel send | 90.33 | 91.34 | 11,170 | 151.82 | 2,358.18 |

SBE over `verbs;ofi_rxm` led the aggregate result with either send path.
Automatic injection was 4.2-4.4% faster at the average median than forced
`fi_send` on that provider, while their average p99.99 results differed by
less than one percent. On the software `tcp` provider, the two send policies
were effectively tied.

The forced-send result is important beyond its ranking: every frame used
`fi_send`, transmit buffers were reclaimed through the TX completion queue,
receives used `fi_recv`, and receive completions came through
`fi_cq_readfrom`. The faster automatic mode uses `fi_inject` for an eligible
small frame and falls back to that same `fi_send` path for a larger frame.

## Operation latency

Each cell below is `p50 / p99.99` in microseconds. Pub/Sub measures the full
path from issuing `PUBLISH`, through its acknowledgement, to validated receipt
by one literal-channel subscriber.

### Automatic inject path

Kernel TCP is included as the socket baseline; "automatic" applies only to the
libfabric columns.

| Operation | Kernel TCP / RESP2 | Kernel TCP / SBE | libfabric `tcp` / RESP2 | libfabric `tcp` / SBE | `verbs;ofi_rxm` / RESP2 | `verbs;ofi_rxm` / SBE |
|---|---:|---:|---:|---:|---:|---:|
| `PING` | 63.74 / 99.58 | 55.06 / 99.83 | 36.36 / 64.66 | 34.57 / 67.76 | 5.65 / 26.34 | 4.73 / 25.33 |
| `SET` | 94.07 / 215.54 | 62.08 / 183.27 | 39.77 / 94.03 | 36.86 / 89.76 | 7.63 / 54.00 | 6.34 / 52.31 |
| `GET` | 86.84 / 130.38 | 57.58 / 102.74 | 37.69 / 64.65 | 35.09 / 68.34 | 6.51 / 28.93 | 6.22 / 26.24 |
| `HSET` | 96.89 / 165.96 | 66.41 / 108.31 | 39.97 / 73.38 | 38.97 / 68.70 | 8.33 / 30.80 | 7.60 / 29.34 |
| `HGET` | 89.14 / 157.82 | 58.52 / 102.41 | 37.79 / 67.64 | 35.08 / 66.38 | 7.12 / 28.87 | 5.59 / 26.38 |
| `ZADD` | 98.95 / 147.30 | 66.70 / 113.62 | 40.03 / 70.25 | 38.78 / 71.87 | 8.32 / 31.36 | 7.09 / 29.94 |
| `ZSCORE` | 88.40 / 138.30 | 56.89 / 99.68 | 36.67 / 64.72 | 34.90 / 69.42 | 7.03 / 26.31 | 6.23 / 26.97 |
| `PUBSUB` | 104.60 / 159.69 | 70.47 / 119.51 | 48.41 / 90.50 | 58.43 / 114.18 | 8.44 / 31.86 | 7.30 / 30.09 |

The `tcp`/SBE automatic Pub/Sub cell was slower than its RESP2 neighbor and
than the forced-SBE result. It is retained as measured rather than replaced by
an inferred value.

### Forced `fi_send` path

| Operation | libfabric `tcp` / RESP2 | libfabric `tcp` / SBE | `verbs;ofi_rxm` / RESP2 | `verbs;ofi_rxm` / SBE |
|---|---:|---:|---:|---:|
| `PING` | 35.78 / 73.07 | 34.45 / 76.37 | 6.36 / 27.17 | 5.46 / 24.92 |
| `SET` | 39.81 / 92.30 | 36.90 / 90.60 | 7.84 / 54.27 | 6.76 / 53.02 |
| `GET` | 37.69 / 73.31 | 36.56 / 70.21 | 7.10 / 28.92 | 6.09 / 25.45 |
| `HSET` | 39.95 / 73.13 | 38.46 / 80.59 | 8.44 / 31.00 | 7.92 / 28.98 |
| `HGET` | 37.83 / 67.17 | 36.10 / 68.21 | 7.33 / 28.67 | 6.12 / 27.38 |
| `ZADD` | 41.28 / 73.05 | 38.85 / 69.53 | 9.00 / 31.08 | 7.89 / 28.48 |
| `ZSCORE` | 37.72 / 68.96 | 34.81 / 65.96 | 6.70 / 27.99 | 5.58 / 26.46 |
| `PUBSUB` | 48.48 / 82.74 | 48.82 / 92.94 | 8.72 / 31.92 | 7.53 / 30.47 |

The [raw matrix on
GitHub](https://github.com/adamdeprince/goblin-core/blob/main/benchmarks/libfabric-provider-matrix-2026-07-26/latency.csv)
contains min, p50, p75, p90, p95, p99, p99.9, p99.99, maximum, mean, and
sequential operations per second for all 80 distributions. Its neighboring
[README](benchmarks/libfabric-provider-matrix-2026-07-26/README.md) records
the build identity and artifact layout.

## Tail behavior

On `verbs;ofi_rxm`, operation-level p99.99 ranged from 24.92 to 54.27
microseconds across both wire formats and send paths. Isolated maxima between
0.74 and 1.00 milliseconds remained beyond p99.99. Kernel TCP produced the
largest single observation, 2.36 milliseconds during `GET`.

The original RDM qualification had a much larger and repeatable tail step. It
was not caused by out-of-order delivery. Instrumented depth-one runs observed
1,760,003 replies on the publisher connection and 220,001 replies on the
subscriber connection without one sequestered frame, duplicate, reorder-window
overflow, sequence gap, or pending reordered message. Server-side request
counters were likewise clean.

The stall was local to Goblin's bootstrap listener. The polled server called
nonblocking `accept()` on every spin-loop pass. On Linux, an unsuccessful
`accept()` can allocate and then retire socket, file, and inode objects before
returning `EAGAIN`. Function tracing showed repeated RCU softirq batches that
reclaimed those objects, with individual batches lasting roughly 0.4-1.1
milliseconds.

Goblin now checks listener readiness first and only drains `accept()` when a
bootstrap connection is waiting. Before the fix, one trace contained 4,119 RCU
softirq runs over 100 microseconds. During a two-second active post-fix trace,
all 59 softirq runs were below 100 microseconds and the maximum was 20.6
microseconds. The immediate control run cut average SBE/RDM p99.99 from 857.47
to 29.47 microseconds; this independent full matrix reproduced it at 30.83
microseconds.

The preserved pre-fix results and traces are in
[`benchmarks/libfabric-2026-07-26`](benchmarks/libfabric-2026-07-26/README.md).

## Method

- Both hosts are four-socket Intel Xeon E5-4657L v2 systems running Linux
  5.15.0-186.
- Both endpoints use a Mellanox ConnectX-5 (firmware 16.35.8008) negotiated at
  100,000 Mb/s full duplex over a direct-attach copper link.
- The NIC is on NUMA node 1 on both hosts. Both Goblin and the benchmark client
  were pinned to CPU 5 and memory node 1.
- The Release build used AWS libfabric `2.4.0amzn5.0`. RDM used
  `FI_EP_RDM`, `FI_MSG | FI_SOURCE`, `fi_send`/`fi_recv`, and completion
  queues through providers `tcp` and `verbs;ofi_rxm`.
- Each provider/send combination used a separate server run. This also kept
  Goblin's strict-priority polled libfabric target out of the kernel TCP
  reference.
- Every workload used pipeline depth 1, a 128 KiB client work buffer, 20,000
  warmups, and 200,000 measured operations. Thus p99.99 represents 20
  observations per distribution.
- Libfabric heartbeats were disabled during measurement, avoiding idle
  heartbeat traffic while both clients continuously generated work.
- The libfabric client used 16 transmit slots, 16 receive slots, a 128 KiB
  maximum record, and a bounded 64-message/1 MiB reorder window.
- `SET` alternated two same-length 16-byte values on an existing key. `GET` was
  a hit. `HSET` updated one existing field with the same alternating values and
  `HGET` was a hit. `ZADD` alternated scores 1 and 2 for an existing member and
  `ZSCORE` was a hit.
- Pub/Sub used separate publisher and subscriber connections and one literal
  channel. Timing began before `PUBLISH` and ended only after the subscriber
  received and validated the payload.
- RESP2 and SBE replies were validated on every warmup and measured operation.

Depth-one operations per second measure sequential round trips, not saturated
throughput. The 100 Gb/s link is a latency path in this test, not a bandwidth
target.

## Reproduction

The complete cross-host matrix is automated:

```console
SAMPLES=200000 WARMUP=20000 \
  bash benchmarks/libfabric_provider_matrix.sh
```

An automatic-inject server for the fastest local provider:

```console
build/goblin-core \
  --libfabric 'verbs;ofi_rxm' 10.100.0.1 17401 \
  --enable-sbe \
  --no-auth-libfabric \
  --efa-heartbeat-timeout-ms 0 \
  --cpu 5 \
  --numa 1
```

Add `--libfabric-force-send` to route every frame through `fi_send` and its TX
completion queue. The benchmark executable selects the same client-side mode
with the final `auto` or `send` argument:

```console
numactl --physcpubind=5 --membind=1 \
  build/goblin_core_libfabric_latency_benchmark \
  fabric-sbe 'verbs;ofi_rxm' 10.100.0.1 17401 10.100.0.2 \
  verbs-sbe-send 200000 20000 send
```
