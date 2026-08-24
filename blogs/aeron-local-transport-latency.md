# Aeron IPC below a microsecond, and Aeron UDP across 100 GbE

With continuously polled clients and Media Drivers, Goblin Core's SBE `PING`
measured 0.481 microseconds at p50 over Aeron IPC. The same operation measured
12.489 microseconds through two Aeron drivers over Linux UDP loopback. Between
two physical servers, it measured 48.099 microseconds over a 10 GbE kernel-UDP
path and 7.172 microseconds when the two Aeron drivers used XLIO over direct
100 GbE.

Those numbers answer different questions. The local matrix compares IPC,
loopback networking, a shared-memory ring, and a Unix-domain socket on one
Threadripper host. The network matrix compares two complete paths between a
pair of older PowerEdge servers. It changes the NIC and network stack as well
as the link rate, so it is not a 10-versus-100-Gb bandwidth experiment.

Every figure below is the arithmetic mean of two complete runs. Each run used
10,000 warm-up requests followed by 100,000 measured, synchronous, depth-one
round trips for each operation. There was no pipeline hiding the return path.

## Local transport results

The local matrix ran on `naamah`, an AMD Ryzen Threadripper PRO 5995WX with 64
physical cores and one NUMA node. Each protocol/transport pair received a fresh
Goblin server. The server and measuring client occupied distinct physical
cores. In the Aeron cases, their two client conductors and all Media Driver
agents also had separate physical cores.

All latency values in the tables are microseconds.

| Protocol / transport | PING p50 | PING p99 | SET p50 | SET p99 | GET p50 | GET p99 |
|---|---:|---:|---:|---:|---:|---:|
| RESP / ring | 0.316 | 0.376 | 0.736 | 0.812 | 0.406 | 0.466 |
| SBE / ring | 0.200 | 0.251 | 0.471 | 0.556 | 0.261 | 0.311 |
| RESP / Aeron IPC | 0.691 | 1.904 | 1.062 | 2.390 | 0.741 | 1.994 |
| SBE / Aeron IPC | 0.481 | 1.738 | 0.832 | 2.179 | 0.601 | 1.808 |
| RESP / Aeron UDP loopback | 12.744 | 18.084 | 13.225 | 18.475 | 12.779 | 19.427 |
| SBE / Aeron UDP loopback | 12.489 | 17.198 | 12.864 | 19.302 | 12.799 | 17.929 |
| RESP / UDS | 8.271 | 12.343 | 10.420 | 13.395 | 10.395 | 12.599 |
| SBE / UDS | 7.204 | 8.451 | 7.634 | 9.388 | 7.349 | 8.701 |

The steady-state order is straightforward: Goblin's native ring is fastest,
Aeron IPC is next, then UDS, then two-driver Aeron UDP over loopback. Aeron IPC
adds roughly 0.28--0.38 microseconds to the corresponding ring median while
retaining a p99 below 2.4 microseconds in every row.

Loopback UDP is deliberately not an IPC shortcut. The client and server have
separate Media Drivers, and their datagrams cross Linux's UDP loopback path.
That makes the result representative of Aeron UDP's local network machinery,
not a differently named shared-memory channel.

## Across 10 and 100 GbE

`butterfly` served the network cases and `rain` ran the client. Both are Dell
PowerEdge R820 systems with four Intel Xeon E5-4657L v2 sockets and four NUMA
nodes. The shared NFS home meant Goblin and Aeron were built once on `rain` and
the byte-identical artifact was reused on both hosts; Aeron directories, logs,
and other runtime state remained on each machine's local `/tmp`.

| Path / protocol | PING p50 | PING p99 | SET p50 | SET p99 | GET p50 | GET p99 |
|---|---:|---:|---:|---:|---:|---:|
| 10 GbE kernel / RESP | 49.085 | 62.713 | 50.734 | 66.385 | 49.494 | 64.168 |
| 10 GbE kernel / SBE | 48.099 | 61.019 | 49.725 | 64.340 | 48.759 | 62.313 |
| 100 GbE XLIO / RESP | 7.378 | 16.021 | 8.764 | 19.451 | 7.718 | 17.753 |
| 100 GbE XLIO / SBE | 7.172 | 14.895 | 8.282 | 18.955 | 7.427 | 16.932 |

The 10 GbE path used the kernel UDP stack on node-local Broadcom `bnx2x` NICs.
The adapters reported 24-microsecond RX and 48-microsecond TX coalescing. The
100 GbE path used a direct ConnectX-5 link on NUMA node 1. XLIO 3.61.2 was
preloaded into the external Aeron C Media Driver on each endpoint, with every
Aeron agent and XLIO's helper thread pinned to a separate node-local physical
core.

For these tiny depth-one messages, the complete 100 GbE userspace
configuration reduced PING p50 by about 6.7x and p99 by roughly 4x. That result
belongs to the whole configuration. Larger link capacity alone does not explain
it: XLIO bypasses the kernel UDP path, the ConnectX-5 has different coalescing,
and both configurations use different NICs on different NUMA nodes.

This is also distinct from Goblin's native XLIO Ultra TCP transport. Goblin and
the benchmark client still talk to their local Aeron drivers through shared
memory. XLIO intercepts the drivers' ordinary UDP socket calls and moves that
network path into userspace.

## Proving that XLIO really offloaded UDP

The 100 GbE drivers used XLIO's latency profile, anonymous memory, infinite
initial polling for unicast UDP, no kernel-FD poll ratio, and no progress-engine
drain thread. `XLIO_EXCEPTION_HANDLING=2` returned errors instead of silently
undoing an unsupported socket operation.

Attaching `xlio_stats` while measuring would add another runnable process to a
latency experiment. The reported rows therefore use no concurrent statistics
collector. A separate qualification run used the same binaries, routes, CPU
layout, channel URIs, and driver settings, then attached `xlio_stats` to the
actual Media Driver PIDs. Both endpoints recorded nonzero transmitted and
received offloaded packet counts. That qualification is archived beside the
reported data but is not included in either mean.

## Continuous polling is part of the result

`DEDICATED` Aeron threading assigns conductor, sender, and receiver agents to
separate threads; it does not by itself make them continuously poll. The
latency profile in these runs is explicit:

| Agent | Idle strategy |
|---|---|
| Goblin and benchmark C++ Aeron client conductors | `spin` |
| Media Driver conductor | `spin` |
| Media Driver sender and receiver | `noop` |
| Network publication | at most two messages per send |

In Aeron's C driver, `noop` means immediately begin the next duty cycle. It
does not disable the agent. Goblin's synchronous client also continuously polls
its response subscription on the calling thread. The policy minimizes wake-up
latency by occupying the assigned cores, including when traffic is quiet.

The cost differs by topology. IPC has one shared local Media Driver. UDP has a
driver on each endpoint, so both machines reserve conductor, sender, and
receiver cores in addition to their application and Aeron client-conductor
cores. The 100 GbE XLIO path reserves one more core per host for XLIO's helper.

## Why the old Aeron SET p99 was 157 microseconds

The previous local archive used dedicated Media Driver threads with Aeron's
default exponential-backoff idle policy. `PING` and `GET` had ordinary UDP
tails, but repeated 16-byte `SET` operations paused the store often enough to
let idle driver agents advance into backoff. Work then waited for those agents
to wake.

| Local UDP policy | RESP SET p99 | SBE SET p99 |
|---|---:|---:|
| Historical default backoff | 157.655 | 154.909 |
| Current continuous profile | 18.475 | 19.302 |

The application pause itself comes from a compact value layout. A 16-byte raw
value becomes 17 encoded bytes after its tag. Six bytes remain inline and an
11-byte tail goes into the keyspace arena. Replacing the same key retires one
11-byte tail, so the 64 KiB dead-byte floor is reached every

```text
ceil(65,536 / 11) = 5,958 overwrites
```

Goblin then rebuilds the arena and updates live tail locations. That work is
still visible: in the current local runs, `SET` p99.9 ranged from 15.419
microseconds on the SBE ring to 32.321 microseconds on RESP UDP. Continuous
polling removes the transport's backoff amplification; it does not remove the
store maintenance. Reusing compatible tails or reclaiming incrementally would
be a separate store-side optimization.

## One response-channel detail that matters across hosts

Aeron response channels use an asymmetric control URI. The response publication
binds the control address on the server. The remote client subscription uses
that same server control URI as its target; Aeron's response correlation learns
the client's return destination. Supplying the client's IP as Goblin's response
control address makes the server Media Driver try to bind an address it does not
own.

The benchmark harness records the exact channels for every case. Bare server
endpoints are sufficient when routing is unambiguous:

```text
request:  aeron:udp?endpoint=SERVER_IP:REQUEST_PORT
response: aeron:udp?control=SERVER_IP:RESPONSE_PORT
```

## Reproduction and raw data

All reported runs use Goblin commit
[`9ddb3ed`](https://github.com/adamdeprince/goblin-core/commit/9ddb3ed3ea29dfbf458a388ca3e5143a3ffcdd76)
and Aeron 1.51.0 commit
[`9773cba`](https://github.com/aeron-io/aeron/commit/9773cba37e4b88b2b7eb9460c4e0050267b71d28).

- The [current local archive](../benchmarks/local-transports-naamah-2026-08-24/README.md)
  contains both complete `naamah` runs, metadata, hashes, and the reproduction
  command.
- The [10/100 GbE network archive](../benchmarks/aeron-network-rain-butterfly-2026-08-24/README.md)
  contains raw case CSVs, affinities, channel URIs, NIC counters, Media Driver
  logs, XLIO logs, and the independent offload qualification.
- The [historical backoff archive](../benchmarks/local-transports-naamah-2026-08-23/README.md)
  preserves the original 153--158 microsecond UDP `SET` p99 rather than mixing
  it into the current tables.
- The [Aeron transport guide](../docs/aeron.md) documents the server, C++, and
  Python configuration and the continuous polling policy.

The launchers are `benchmarks/local_transport_latency.sh` for one host and
`benchmarks/aeron_network_latency.sh` for the cross-host matrix.
