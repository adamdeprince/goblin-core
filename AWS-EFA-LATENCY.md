# Native AWS EFA: the fast follow

The [first libfabric latency paper](EFA-LATENCY.md) qualified Goblin Core's
EFA-shaped `FI_EP_RDM` code on the hardware available locally. It used
`verbs;ofi_rxm` over a direct 100 Gb/s ConnectX-5 link and said plainly that a
native AWS EFA run still had to happen.

This is that run. On two cluster-placed `hpc7g.4xlarge` instances, Goblin Core
completed eight Redis-shaped operations in **23.06 microseconds at the
arithmetic-average median** using SBE over native EFA. RESP2 over the same EFA
path averaged 23.30 microseconds.

These are persistent connections. Connection establishment is outside every
timed region: each client connects once, warms up 20,000 operations, and then
measures 200,000 depth-one request/reply round trips per command. Pub/Sub also
keeps its publisher and subscriber connections open.

*Scope: two AWS `hpc7g.4xlarge` instances, native EFA2, `FI_EP_RDM`, AWS
libfabric `2.4.0amzn5.0`, and Goblin commit `96bcece` on July 27, 2026.*

## Complete ranking

Modes are ranked by the arithmetic average of the eight per-operation medians.
Average p99.99 is likewise an average of per-operation percentiles, not a
merged distribution. Sequential operations per second is the average of each
operation's reciprocal observed mean.

| Rank | Transport/provider | Wire | Send path | Avg p50 (us) | Avg mean (us) | Avg seq. ops/s | Avg p99.99 (us) | Worst max (us) |
|---:|---|---|---|---:|---:|---:|---:|---:|
| 1 | Native EFA | SBE | auto-inject | 23.06 | 23.14 | 43,240 | 33.81 | 60.48 |
| 2 | Native EFA | SBE | `fi_send` | 23.18 | 23.23 | 43,058 | 33.82 | 54.01 |
| 3 | Native EFA | RESP2 | auto-inject | 23.30 | 23.39 | 42,776 | 34.31 | 74.65 |
| 4 | Native EFA | RESP2 | `fi_send` | 23.40 | 23.47 | 42,620 | 34.22 | 55.64 |
| 5 | libfabric `tcp` | SBE | `fi_send` | 29.19 | 29.23 | 34,272 | 38.74 | 213.57 |
| 6 | libfabric `tcp` | SBE | auto-inject | 29.45 | 29.57 | 33,884 | 38.17 | 103.87 |
| 7 | libfabric `tcp` | RESP2 | auto-inject | 29.50 | 29.55 | 33,894 | 38.39 | 96.27 |
| 8 | libfabric `tcp` | RESP2 | `fi_send` | 29.99 | 30.07 | 33,300 | 42.96 | 262.79 |
| 9 | Kernel TCP | RESP2 | kernel send | 33.36 | 33.49 | 29,907 | 46.87 | 430.18 |
| 10 | Kernel TCP | SBE | kernel send | 35.22 | 33.45 | 30,932 | 44.73 | 378.83 |

Against the same-wire kernel controls, native EFA reduced average median
round-trip latency by 34.5% with SBE and 30.2% with RESP2. Against RESP2 over
libfabric's software `tcp` provider, native EFA reduced it by 21.0%.

Automatic injection and forced `fi_send` were effectively tied on native EFA.
The forced path is a full completion-queue result: requests use `fi_send`,
transmit buffers are reclaimed through the TX completion queue, receives use
`fi_recv`, and receive completions come through `fi_cq_readfrom`. Automatic
mode uses `fi_inject` for an eligible short frame and falls back to that same
`fi_send` path.

## Operation latency

Each cell is `p50 / p99.99` in microseconds from the automatic-inject path.
"Automatic" applies to the libfabric columns; kernel TCP is the socket
baseline. Pub/Sub spans `PUBLISH`, its acknowledgement, and validated delivery
to one literal-channel subscriber.

| Operation | EFA / RESP2 | EFA / SBE | libfabric `tcp` / RESP2 | libfabric `tcp` / SBE | Kernel TCP / RESP2 | Kernel TCP / SBE |
|---|---:|---:|---:|---:|---:|---:|
| `PING` | 22.67 / 30.98 | 22.59 / 30.80 | 28.64 / 35.83 | 28.47 / 34.76 | 31.99 / 37.71 | 34.44 / 45.09 |
| `SET` | 23.34 / 50.80 | 23.12 / 51.27 | 29.11 / 54.43 | 29.30 / 53.89 | 32.97 / 57.54 | 36.41 / 59.53 |
| `GET` | 22.97 / 31.17 | 22.73 / 31.19 | 28.85 / 35.88 | 28.71 / 35.18 | 32.21 / 38.03 | 28.93 / 34.85 |
| `HSET` | 23.56 / 32.01 | 23.35 / 31.71 | 29.62 / 36.34 | 29.41 / 36.80 | 33.18 / 38.88 | 29.41 / 35.99 |
| `HGET` | 23.12 / 31.56 | 22.83 / 30.88 | 28.96 / 34.89 | 28.79 / 35.83 | 32.42 / 70.28 | 28.96 / 34.74 |
| `ZADD` | 23.49 / 31.76 | 23.12 / 31.57 | 29.18 / 35.93 | 29.33 / 35.11 | 33.26 / 42.02 | 29.30 / 35.77 |
| `ZSCORE` | 22.99 / 31.67 | 22.68 / 30.73 | 28.94 / 34.99 | 28.63 / 34.49 | 34.43 / 44.33 | 28.78 / 36.01 |
| `PUBSUB` | 24.24 / 34.56 | 24.08 / 32.31 | 32.71 / 38.82 | 32.94 / 39.27 | 36.41 / 46.21 | 65.50 / 75.88 |

## Kernel control variability

The first one-pass matrix caught the interrupt-driven kernel path in
higher-latency modes. A dedicated 200,000-sample repeat moved the affected
commands rather than reproducing a command-specific Goblin cost:

| Kernel control | Run | Avg p50 (us) | Avg p99.99 (us) |
|---|---|---:|---:|
| RESP2 | Initial matrix | 37.30 | 49.71 |
| RESP2 | Dedicated repeat | 33.36 | 46.87 |
| SBE | Initial matrix | 48.80 | 64.98 |
| SBE | Dedicated repeat | 35.22 | 44.73 |

Both hosts used the default ENA settings: adaptive RX moderation enabled,
20-microsecond RX coalescing, and 64-microsecond TX coalescing. The moving
20-70 microsecond steps are consistent with that interrupt-driven control
path. Native EFA and libfabric `tcp` remained stable across the full matrix.
The ranking uses the dedicated repeat and the raw artifacts retain both runs.

## Method

- Both endpoints were `hpc7g.4xlarge` instances in the same `us-east-1d`
  cluster placement group.
- Each host exposed 16 physical Arm cores, one socket, one NUMA node, and 32
  MiB of shared L3. Server and client were pinned to CPU 8 and memory node 0.
- The native device reported EFA2, active MTU 4096, and an active port.
- RDM used `FI_EP_RDM`, `FI_MSG | FI_SOURCE`, 16 TX slots, 16 RX slots, a
  128 KiB maximum record, and a 64-message/1 MiB reorder window.
- Every publication distribution used 20,000 warmups and 200,000 measured
  depth-one operations, so p99.99 represents 20 observations.
- `SET`, `HSET`, and `ZADD` updated existing objects without changing their
  sizes. `GET`, `HGET`, and `ZSCORE` were hits. Every reply was validated.
- Pub/Sub used separate persistent publisher and subscriber connections and
  one literal channel.
- Libfabric heartbeats were disabled while the continuously active benchmark
  ran.
- Provider `verbs;ofi_rxm` returned `fi_getinfo: -61 (No data available)` on
  the EFA device, so this hardware matrix marks it unavailable instead of
  substituting a different transport.

Depth-one operations per second measure sequential round trips, not saturated
throughput. EFA is a latency path in this test, not a bandwidth target.

## Data

The [raw AWS matrix on
GitHub](https://github.com/adamdeprince/goblin-core/tree/main/benchmarks/libfabric-efa-hpc7g-2026-07-27)
includes all publication distributions, dedicated kernel repeats, aggregate
CSV, build hashes, provider logs, and sanitized hardware metadata.

## Reproduction

Place both EFA-enabled instances in one cluster placement group and substitute
their private VPC addresses:

```console
SERVER_HOST=<ssh-server> \
CLIENT_HOST=<ssh-client> \
SERVER_ADDRESS=<server-private-ip> \
CLIENT_ADDRESS=<client-private-ip> \
NIC=ens5 \
SERVER_CPU=8 \
CLIENT_CPU=8 \
NUMA_NODE=0 \
PROVIDERS=efa,tcp \
SEND_MODES=auto,send \
WIRES=resp,sbe \
SAMPLES=200000 \
WARMUP=20000 \
  bash benchmarks/libfabric_provider_matrix.sh
```

An automatic-inject native EFA server:

```console
build/goblin-core \
  --libfabric efa <server-private-ip> 17401 \
  --enable-sbe \
  --no-auth-libfabric \
  --efa-heartbeat-timeout-ms 0 \
  --cpu 8 \
  --numa 0
```

Add `--libfabric-force-send` to route every frame through `fi_send` and its TX
completion queue. The benchmark executable selects the same client-side mode
with the final `auto` or `send` argument:

```console
numactl --physcpubind=8 --membind=0 \
  build/goblin_core_libfabric_latency_benchmark \
  fabric-sbe efa <server-private-ip> 17401 <client-private-ip> \
  efa-sbe-send 200000 20000 send
```
