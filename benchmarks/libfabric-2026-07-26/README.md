# Libfabric 100 Gb baseline, 2026-07-26

This directory freezes the butterfly-to-rain baseline used by
`EFA-LATENCY.md`. It preserves both the original stalled results and the
production results after the bootstrap-listener fix, ready for comparison with
an AWS EFA run.

## Result files

| File | Contents |
|---|---|
| `tcp-resp.csv` | Kernel TCP with RESP2 |
| `tcp-sbe.csv` | Kernel TCP with SBE |
| `rdm-resp-pre-fix.csv` | `verbs;ofi_rxm` with RESP2 before the listener fix |
| `rdm-sbe-pre-fix.csv` | `verbs;ofi_rxm` with SBE before the listener fix |
| `rdm-sbe-no-heartbeat-pre-fix.csv` | Control run proving heartbeats were not the tail source |
| `rdm-resp-post-fix.csv` | Final non-instrumented RESP2/RDM result |
| `rdm-sbe-post-fix.csv` | Final non-instrumented SBE/RDM result |
| `ordering-client.log` | Full client reply-order counters |
| `ordering-server.log` | Server request-order counters from the investigation |
| `client-tail-events-pre-fix.csv.gz` | Timestamped pre-fix events over 100 us |
| `server-softirq-pre-fix.trace.gz` | Pre-fix server scheduler and softirq trace |
| `server-rcu-functions-pre-fix.trace.gz` | Pre-fix `rcu_core` function-graph trace |
| `server-softirq-post-fix.trace.gz` | Post-fix server scheduler and softirq trace |

Every latency CSV uses 20,000 warmups and 200,000 measured depth-one
operations per command. Pub/Sub measures `PUBLISH` through receipt by one
literal-channel subscriber.

## Comparison anchor

| Mode | State | Average p50 (us) | Average p99.99 (us) |
|---|---|---:|---:|
| RDM / RESP2 | pre-fix | 9.44 | 756.88 |
| RDM / RESP2 | post-fix | 7.76 | 32.34 |
| RDM / SBE | pre-fix | 8.33 | 857.47 |
| RDM / SBE | post-fix | 6.53 | 29.47 |

The ordering counters recorded no gaps, sequestered messages, duplicates, or
reorder-window overflows. The tail came from calling nonblocking `accept()` on
an empty bootstrap listener on every spin-loop pass. Linux retired the
temporary socket, file, and inode objects through long RCU softirq batches.
The fixed server polls listener readiness before entering `accept()`.

The pre-fix server trace contained 4,119 RCU softirq runs over 100 us. In the
two-second post-fix trace, all 59 softirq runs were below 100 us and the maximum
was 20.6 us.

## Platform

```text
captured_utc=20260727T004925Z
base_git_commit=774117c954557150a862033f7b3447f079cc1a9a
working_tree=dirty-libfabric-feature
server_host=butterfly
server_address=10.100.0.1
client_host=rain
client_address=10.100.0.2
interface=enp68s0np0
numa_node=1
server_cpu=5
client_cpu=5
cpu=Intel Xeon E5-4657L v2 at 2.40 GHz
kernel=Linux 5.15.0-186-generic
nic=Mellanox ConnectX-5 MT27800
nic_firmware=16.35.8008
link=100000 Mb/s full duplex, MTU 1500
provider=verbs;ofi_rxm
libfabric=2.4.0amzn5.0
compiler=GCC 16.1
build=Release
```

This is a libfabric RDM qualification of Goblin's EFA-oriented code path over
ConnectX-5 Ethernet. It is not an AWS EFA hardware result.

## Exact build hashes

```text
e881b6bfc13044fde0c6614231a234b520f49cbacb9a92e0fd5ae297feec7a5a  src/libfabric_transport.cpp
e5a220e45b6ecb774650f5d90ba433856591174f83caf1593b9fa59cbecbe7f0  include/goblin/core/libfabric_transport.hpp
224315477ed21e9b9265a85707c04c83c50a2c34f715332f3336c8d02d4c4e9f  include/goblin/core/libfabric_wire.hpp
f34c043c4c2e7f07009b87be18ae8bab398ab74a49fa5d162f4327b31f5ee51f  benchmarks/libfabric_latency_benchmark.cpp
2654c30c038c4756d511e0e8fd891d9f231a0f5693418249c4be12cf8097eccd  goblin-core
fd7664e70e01d58783a35abe22019cc8b2e03777a1bdf0033c2c7eed746b41d1  goblin_core_libfabric_latency_benchmark
```

## Reproduction

The fixed RDM server was started with:

```console
goblin-core \
  --libfabric 'verbs;ofi_rxm' 10.100.0.1 7401 \
  --enable-sbe \
  --no-auth-libfabric \
  --efa-heartbeat-timeout-ms 0 \
  --cpu 5 \
  --numa enp68s0np0
```

The final clients were:

```console
taskset -c 5 goblin_core_libfabric_latency_benchmark \
  fabric-resp 'verbs;ofi_rxm' 10.100.0.1 7401 10.100.0.2 \
  fabric-resp-final 200000 20000

taskset -c 5 goblin_core_libfabric_latency_benchmark \
  fabric-sbe 'verbs;ofi_rxm' 10.100.0.1 7401 10.100.0.2 \
  fabric-sbe-final 200000 20000
```

Use `gzip -dc FILE.trace.gz` to inspect a saved trace without modifying it.
