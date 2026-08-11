# BlueField DPU-side Pub/Sub benchmark

Generated on August 11, 2026 from the current v0.10.4 release tree.

## Current result

Goblin Core processed RESP Pub/Sub on a BlueField-2 DPU connected directly at
100 Gb/s to a ConnectX-5 client host. The DPU-resident edge used the native XLIO
Ultra API in both runs; only the client transport changed.

| Client transport | DPU-side operation | p50 | p90 | p99 | p99.9 | Mean | Sequential ops/s |
|---|---|---:|---:|---:|---:|---:|---:|
| Native XLIO Ultra | Local Pub/Sub | 12.54 us | 14.36 us | 19.31 us | 24.39 us | 12.93 us | 77,338 |
| Kernel TCP | Local Pub/Sub | 107.77 us | 108.67 us | 116.47 us | 153.67 us | 108.07 us | 9,253 |
| Native XLIO Ultra | PING control | 10.61 us | 10.80 us | 11.28 us | 18.56 us | 9.84 us | 101,657 |
| Kernel TCP | PING control | 27.58 us | 28.29 us | 32.65 us | 39.75 us | 27.79 us | 35,982 |

Against the same DPU-resident edge, the native XLIO client reduced median
Pub/Sub latency by 8.59x and raised sequential throughput by 8.36x. On the
all-XLIO path, DPU-side subscription lookup, RESP encoding, second-connection
delivery, and fanout added 1.93 microseconds to the PING median.

## What was measured

- The DPU edge ran on BlueField Arm CPU 2 with XLIO 3.61.2 and DPCP 1.1.61. It
  parsed `PUBLISH`, matched DPU-local subscriptions, and delivered the RESP
  message on-card before forwarding the publication to the authoritative
  host-side server. `SUBSCRIBE` was likewise registered on the DPU before its
  aggregate subscription was sent upstream.
- The client ran on CPU 5, local to its ConnectX-5 NUMA node. The XLIO mode used
  the same native userspace client as the earlier
  ConnectX-5-to-ConnectX-5 qualification.
- Pub/Sub used 5,000 measured requests after 500 warmups. PING used 100,000
  measured requests after 500 warmups. Both used one outstanding request.
- Pub/Sub timing began before `PUBLISH` and ended after the subscriber received
  and validated its RESP message. The upstream publication reply was validated
  after the timestamp and before the next sample, so forwarding remained part
  of the required behavior without putting host-side server processing in the
  DPU-local fanout measurement.
- Exactly one native Ultra edge process ran for each row. The direct link
  negotiated 100G_4X with RS-FEC, and post-run counters showed no CRC errors,
  corrected bits, or RX/TX discards.

The DPU used anonymous XLIO allocation because the lab image had no reserved
HugeTLB pages. These numbers describe this exact BlueField-2/ConnectX-5 stack;
they are not estimates for newer DPU cores or a tuned huge-page deployment.

## Artifacts

- [Pub/Sub latency CSV](benchmarks/bluefield_pubsub_2026-08-11.csv)
- [PING latency CSV](benchmarks/bluefield_ping_2026-08-11.csv)
- [Deployment metadata](benchmarks/bluefield_pubsub_2026-08-11-metadata.txt)
- [BlueField edge architecture and deployment](docs/bluefield.md)
