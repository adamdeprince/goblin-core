# Libfabric provider matrix, 2026-07-26

This directory freezes the full butterfly-to-rain transport matrix reported in
`EFA-LATENCY.md`. It qualifies Goblin Core's EFA-oriented `FI_EP_RDM` path on
hardware available locally. It is not an AWS EFA hardware result.

## Matrix

- Providers: libfabric `tcp` and `verbs;ofi_rxm`
- Endpoint: `FI_EP_RDM`
- Capabilities: `FI_MSG | FI_SOURCE`
- Data APIs: `fi_send`/`fi_recv`
- Completion API: `fi_cq_read`/`fi_cq_readfrom`
- Wire formats: RESP2 and SBE
- Send paths: automatic `fi_inject` with `fi_send` fallback, and forced
  `fi_send`
- Reference: ordinary kernel TCP with RESP2 and SBE
- Operations: `PING`, `SET`, `GET`, `HSET`, `HGET`, `ZADD`, `ZSCORE`, and
  one-subscriber Pub/Sub

Every cell uses 20,000 warmups followed by 200,000 measured depth-one
round trips per operation. The complete matrix therefore contains 16 million
measured operations.

## Files

| File | Contents |
|---|---|
| `latency.csv` | All 80 latency distributions in one machine-readable table |
| `config.csv` | Transport, wire, buffer, and sample configuration for each cell |
| `metadata.txt` | Hosts, CPU, NIC, NUMA, build, and binary hashes |
| `*.out` | Original benchmark output for each configuration |
| `*.err` | Original benchmark standard error |
| `*.server.log` | Server startup record and selected provider/send path |
| `SHA256SUMS` | SHA-256 digest for every other artifact in this directory |

The earlier listener-tail investigation, including pre-fix traces, remains in
`../libfabric-2026-07-26/`.

## Platform

The server was `butterfly` at `10.100.0.1`; the client was `rain` at
`10.100.0.2`. Both are four-socket Intel Xeon E5-4657L v2 systems. Each host
used CPU 5 and memory node 1, which is local to its Mellanox ConnectX-5
interface. The direct link negotiated at 100,000 Mb/s full duplex.

The build used AWS libfabric `2.4.0amzn5.0`. Native `efa` was unavailable on
these hosts, so only providers that successfully negotiated the required
contract were measured.

## Exact source and binary hashes

```text
cfecab56f1184c1900022f6ab5a650e73a8388834c82fdaf690d18f922fe1ff2  src/libfabric_transport.cpp
e46325764ffc10fc55079c3e19e330c017dc41d07245dd74a2553d362d14be1d  include/goblin/core/libfabric_transport.hpp
224315477ed21e9b9265a85707c04c83c50a2c34f715332f3336c8d02d4c4e9f  include/goblin/core/libfabric_wire.hpp
851bd25a855ece0c1f495e2789c89e4c518c7f9408460a561eef8366a9897dc3  benchmarks/libfabric_latency_benchmark.cpp
03918369150223f1312315e3da22445385341202d17ff1cdb82c6fefb42107a5  benchmarks/libfabric_provider_matrix.sh
fccfe23eb6fd6c0535e75fca8e907432834195337db0906061fea260047ff821  goblin-core
751a773489a6069779228ac57bdb4a56e0e124fccc12ff01872f2902e3cbca48  goblin_core_libfabric_latency_benchmark
```

The source tree was based on commit
`774117c954557150a862033f7b3447f079cc1a9a`; the libfabric feature was still
uncommitted, so the source and binary hashes above are the authoritative build
identity.

## Reproduction

The controller script verifies NIC/CPU NUMA placement, builds when requested,
starts each server configuration separately, validates provider startup,
captures the raw output, and assembles the CSV files:

```console
SAMPLES=200000 WARMUP=20000 \
  bash benchmarks/libfabric_provider_matrix.sh
```

Set `PROVIDERS`, `SEND_MODES`, or `WIRES` to comma-separated subsets. Set
`SKIP_KERNEL_TCP=1` to omit the ordinary socket reference.
