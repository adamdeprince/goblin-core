# Kafka Durability Benchmark

## Summary

Goblin Core's broker acknowledgement mode buys a precise success boundary: a
write reply is not released until Kafka has accepted every mutation in that
atomic operation under the broker's configured durability policy. On the
single-node Redpanda installation described below, an individually confirmed
`SET` took **966 us at p50**, **1.355 ms at p99**, and **1.532 ms at p99.9**, for
**1,079 writes/s**. The rare tail was real: p99.99 was **16.7 ms** across 100,000
measurements per round.

That cost amortizes well. RESP pipelines reached **51.0K broker-confirmed
writes/s at depth 128** and **61.5K/s at depth 512**. A 128-write `MULTI`/`EXEC`
block reached **44.1K writes/s**, with its one atomic result withheld until all
128 Kafka records were acknowledged. Every broker-mode workload finished with
zero pending records and an acknowledged logical offset equal to the applied
offset.

These are medians of three complete rounds, not the best round.

## What was tested

One native C++ client talked RESP2 over a Unix-domain socket to one Goblin Core
process. The server ran in three modes:

| Mode | Successful write reply means |
|---|---|
| No journal | The write was applied to Goblin's in-memory state |
| Kafka `queued` | The write was applied and librdkafka accepted its copied record into the local producer queue |
| Kafka `broker` | The write was applied and Redpanda acknowledged every Kafka record in the atomic operation |

The Kafka producer used idempotence, `acks=all`, one in-flight request per broker
connection, and an explicit `linger.ms=0`. Every Kafka run used a new topic with
one partition and replication factor 1. The runner deleted only its own topics
after preserving their final high watermarks.

`SET` used fixed keys and alternating 16-byte values, so each operation was a
real overwrite without growing the Goblin keyspace. Each mode and round issued
exactly **1,101,520 writes**:

- 2,000 warm-up writes.
- 100,000 sequential SETs, timed one by one. This provides 100,000 observations
  per round for the p99.99 result.
- About 200,000 SETs at each pipeline depth: 8, 32, 128, and 512.
- About 50,000 SETs inside `MULTI`/`EXEC` blocks containing 1, 8, 32, or 128
  writes. The server transaction buffer was explicitly set to 64 KiB.

Mode order rotated across rounds to reduce ordering bias. Tables report the
median of each run-level result. Pipeline and transaction latency is whole-batch
latency; the observation count therefore shrinks as batch size grows. Their tail
percentiles should not be confused with the 100,000-observation sequential
distribution.

## Sequential latency

One SET was sent and its reply consumed before the next SET. All latency columns
are microseconds.

| Reply boundary | writes/s | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| No journal | 53,616 | 18.31 | 18.86 | 32.79 | 45.44 | 66.10 | 86.25 |
| Kafka queued | 13,887 | 24.68 | 166.25 | 519.80 | 775.64 | 1,468.87 | 18,399.16 |
| Kafka broker | 1,079 | 966.18 | 1,159.59 | 1,354.51 | 1,532.44 | 16,675.26 | 28,044.70 |

Queued mode keeps the common path close to the UDS baseline, but asynchronous
producer work still appears in its tail because delivery callbacks run on the
single Goblin server thread. Broker mode makes the approximately one-millisecond
Redpanda acknowledgement the common path. The source of the roughly 16-28 ms
rare pauses was not profiled in this run, so this document does not assign them
to Goblin, Redpanda, the NVMe device, or the scheduler.

## RESP pipeline throughput

Each pipeline is one client write followed by consumption of every individual
SET reply. Throughput is SET operations per second. `settled` adds the time
needed after the last client reply for Kafka's acknowledged logical offset to
catch the applied offset.

| Pipeline | batches/round | no journal | queued replies | queued settled | broker confirmed | broker p50 batch |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 25,000 | 184,516/s | 63,475/s | 63,472/s | 5,805/s | 1.329 ms |
| 32 | 6,250 | 254,143/s | 96,134/s | 96,071/s | 12,326/s | 2.593 ms |
| 128 | 1,562 | 110,679/s | 102,336/s | 102,267/s | 51,010/s | 2.376 ms |
| 512 | 390 | 112,740/s | 101,244/s | 101,138/s | 61,473/s | 8.215 ms |

The queued producer did not hide a large unpaid backlog in this test. Across all
queued workloads and rounds, at most 384 records remained after the final client
reply and the longest final drain was 3.237 ms. Broker mode had zero pending
records at every measured client boundary by construction.

## Atomic transaction throughput

The client sent `MULTI`, N SET commands, and `EXEC` as one RESP batch, then read
the `OK`, every `QUEUED`, and the final EXEC array. Throughput counts the SET
mutations, not transaction envelopes.

| Writes/EXEC | transactions/round | no journal | queued replies | queued settled | broker confirmed | broker p50 EXEC batch |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 50,000 | 18,400/s | 12,157/s | 12,157/s | 951/s | 1.051 ms |
| 8 | 6,250 | 70,241/s | 61,324/s | 61,232/s | 5,838/s | 1.324 ms |
| 32 | 1,562 | 93,610/s | 93,800/s | 93,388/s | 12,866/s | 2.512 ms |
| 128 | 390 | 91,156/s | 111,820/s | 111,430/s | 44,099/s | 2.840 ms |

The one-write transaction row includes MULTI/EXEC protocol and transaction
bookkeeping, so it should not be compared to raw SET as though they were the same
operation. The useful result is the scaling: the broker round trip is amortized
over the mutation block without weakening atomicity or releasing EXEC early.

## Test system

| Component | Configuration |
|---|---|
| Host | Dell PowerEdge R820 (`thunder`) |
| CPU | 4x Intel Xeon E5-4657L v2 at 2.40 GHz; 48 cores, 96 threads |
| Memory topology | 4 NUMA nodes; about 1.5 TiB total |
| Goblin/client placement | CPU 4 and CPU 24, separate physical cores on NUMA node 0 |
| Local storage | 960 GB Intel SSDPED1D960GAY NVMe, XFS, NUMA node 0 |
| OS | Linux 5.15.0-186-generic |
| Redpanda | 26.1.13, one node, 4 shards, 8 GiB, `write_caching_default=false` |
| Redpanda placement | CPU affinity on NUMA node 2 during the run |
| Goblin Core | commit `ffc8d78`, GCC 16.1, Release, native architecture |

Goblin and the benchmark client were deliberately pinned. Redpanda retained its
deployed affinity rather than being retuned for the benchmark. On this four-
socket machine, node 0 to node 2 has the largest reported NUMA distance. The
result is therefore representative of this installation, not a claim about the
best latency a broker-local placement or current hardware can achieve.

Goblin connected to Redpanda at the host's LAN address, but both processes ran on
the same physical server; Linux routed the TCP traffic locally. This benchmark
does not include a switch or remote Kafka node.

## Interpretation and limits

Broker mode does not invent a durability guarantee stronger than the broker's
configuration. This Redpanda topic had one replica, so `acks=all` acknowledged
one broker. It validates the Goblin-to-broker success boundary and local durable
configuration, but not availability through a broker-machine failure or the
latency of a multi-node quorum.

Goblin applies a write locally before the asynchronous delivery callback releases
its reply. Other clients continue making progress while that acknowledgement is
pending. The mode guarantees what a successful write reply and downstream
firehose release mean; it is not an isolation barrier hiding the tentative local
value from concurrent readers.

Crash recovery and snapshot replay are tested separately in
[External Logging: The First Recovery Test](EXTERNAL-LOGGING-FIRST-TEST.md) and
[External Logging: All Persistent Types](EXTERNAL-LOGGING-ALL-TYPES-TEST.md).

## Reproduce it

Build the server and native harness, then run against an existing Redpanda or
Kafka broker:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ENABLE_KAFKA=ON \
  -DGOBLIN_CORE_BUILD_BENCHMARKS=ON
cmake --build build \
  --target goblin_core_server goblin_core_kafka_ack_benchmark -j

BROKERS=127.0.0.1:9092 \
GOBLIN_BINARY="$PWD/build/goblin-core" \
BENCHMARK_BINARY="$PWD/build/goblin_core_kafka_ack_benchmark" \
OUTPUT_DIR=/path/on/local/storage/kafka-ack-results \
benchmarks/kafka_ack_benchmark.sh
```

The runner records raw CSV, machine and broker metadata, per-process logs, and
topic high watermarks. For this run the raw CSV SHA-256 was
`32ebb9d6c31e041377eed1f40bccaa1ac5972f459b3097d3d256d2a3774ee2d8`.
