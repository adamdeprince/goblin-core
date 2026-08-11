# BlueField Pub/Sub benchmark

Generated on August 11, 2026 from the current v0.10.3 release tree.

## Current result

Goblin Core's RESP-speaking Pub/Sub edge ran on a BlueField-2 DPU connected
directly at 100 Gb/s to the ConnectX-5 in `rain`. The DPU server used the native
XLIO Ultra API in both runs; only the client transport changed.

| Client on `rain` | Operation | p50 | p90 | p99 | p99.9 | Mean | Sequential ops/s |
|---|---|---:|---:|---:|---:|---:|---:|
| Native XLIO Ultra | Local Pub/Sub | 12.54 us | 14.36 us | 19.31 us | 24.39 us | 12.93 us | 77,338 |
| Kernel TCP | Local Pub/Sub | 107.77 us | 108.67 us | 116.47 us | 153.67 us | 108.07 us | 9,253 |
| Native XLIO Ultra | PING control | 10.61 us | 10.80 us | 11.28 us | 18.56 us | 9.84 us | 101,657 |
| Kernel TCP | PING control | 27.58 us | 28.29 us | 32.65 us | 39.75 us | 27.79 us | 35,982 |

Against the same DPU server, the native XLIO client reduced median Pub/Sub
latency by 8.59x and raised sequential throughput by 8.36x. On the all-XLIO
path, local subscription lookup, RESP encoding, second-connection delivery,
and fanout added 1.93 microseconds to the PING median.

## What was measured

- The edge ran on BlueField Arm CPU 2 with XLIO 3.61.2 and DPCP 1.1.61. It
  accepted ordinary RESP2 over TCP, delivered the publication locally first,
  then forwarded it to the authoritative host server.
- The client ran on `rain` CPU 5, local to its ConnectX-5 NUMA node. The XLIO
  mode used the same native userspace client as the earlier
  ConnectX-5-to-ConnectX-5 qualification.
- Pub/Sub used 5,000 measured requests after 500 warmups. PING used 100,000
  measured requests after 500 warmups. Both used one outstanding request.
- Pub/Sub timing began before `PUBLISH` and ended after the subscriber received
  and validated its RESP message. The upstream publication reply was validated
  after the timestamp and before the next sample, so forwarding remained part
  of the required behavior without inflating the local-fanout measurement.
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
