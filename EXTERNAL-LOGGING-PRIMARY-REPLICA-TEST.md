# External Logging: Staggered Primary and Replica Crashes

The first two external-logging tests killed one Goblin Core process and proved
that its native snapshot plus Redpanda history reconstructed the intended
state. This test adds a live replica and kills **both** Goblin processes, one at
a time. While either process is down, the surviving peer must retain every
expected object. Each killed process must then restart from its unchanged
systemd unit and recover before the other process is allowed to die.

This is a process-recovery test, not a host-failover claim. The primary,
replica, and Redpanda broker all ran on `thunder`. The Goblin processes had
independent listeners, snapshots, systemd units, and lifecycles; their live
replication connection used loopback TCP. Keeping the broker alive isolates the
snapshot, Kafka replay, and firehose handoff behavior under test.

## Topology

The primary used the same production-style invocation as the earlier tests:

```sh
/usr/local/bin/goblin-core \
  --port 6379 \
  --numa eno1 \
  --load /mnt/local/goblin-core/state/goblin.snapshot \
  --kafka kafka://192.168.1.49:9092/goblin-core-replication
```

The replica loaded its own copy of the snapshot, consumed the same durable
history, and followed the primary's firehose over loopback TCP:

```sh
/usr/local/bin/goblin-core \
  --port 6380 \
  --numa eno1 \
  --load /mnt/local/goblin-core/state/goblin-replica.snapshot \
  --kafka kafka://192.168.1.49:9092/goblin-core-replication \
  --replica-tcp 127.0.0.1 6379
```

Only the primary produced Kafka records. The replica used Kafka to bridge a
gap during recovery, but did not write a second copy of replicated mutations.
Both servers used the same Goblin Core binary, as required by the private
firehose protocol.

## Object set

The baseline came from the exhaustive all-types workload in
[External Logging: All Persistent Types](EXTERNAL-LOGGING-ALL-TYPES-TEST.md).
It contained one million values in each persistent type:

| Type | Baseline shape | Check |
|---|---|---|
| String | 1,000,000 individual keys | `GET` every key |
| Hash | 1 hash with 1,000,000 fields | `HLEN`, then `HGET` every field |
| Set | 1 set with 1,000,000 members | `SCARD`, then `SISMEMBER` every member |
| Sorted set | 1 zset with 1,000,000 members | `ZCARD`, then `ZSCORE` every member |
| List | 1 list with 1,000,000 elements | `LLEN`, then `LINDEX` every element |
| Array | 1 array with 1,000,000 elements | `ARLEN`, then `ARGET` every element |

The test first verified all six million values on the primary. It refreshed the
native snapshot, copied the completed 171.6 MiB file to the replica's snapshot
path, started the replica, waited for `replica_state:live`, and independently
verified the same six million values there.

## Failure sequence

The test used `SIGKILL`, not a graceful service restart:

1. Send `SIGKILL` to the primary's recorded `MainPID` and hold its systemd unit
   down.
2. Confirm the replica reports `goblin_ready:0` and `replica_state:reconnecting`,
   then exhaustively read all six million baseline values from it.
3. Start the primary only through its unchanged systemd unit. Wait for recovery,
   wait for the replica to return to `live`, and verify the rebuilt primary.
4. Append 1,000 values to each type through the primary. Wait until the replica
   reaches the primary's new logical offset.
5. Send `SIGKILL` to the replica's recorded `MainPID` and hold its unit down.
6. Exhaustively read all 6,006,000 final values from the primary.
7. Start the replica only through its unchanged unit. It loads the baseline
   snapshot, observes the newer upstream offset, replays the 6,000-mutation
   Kafka suffix, and hands back to the live firehose.
8. Exhaustively verify all 6,006,000 values on each server again.

Holding each unit down is deliberate. A `Restart=on-failure` policy would
otherwise recreate it before the surviving peer could be checked in isolation.
Redpanda remained active throughout.

## Results

Systemd recorded a hard death for each process:

```text
goblin-core.service: Main process exited, code=killed, status=9/KILL
goblin-core-replica-test.service: Main process exited, code=killed, status=9/KILL
```

Every exhaustive checkpoint passed:

| Checkpoint | Values checked | Missing | Wrong |
|---|---:|---:|---:|
| Primary baseline | 6,000,000 | 0 | 0 |
| Replica baseline | 6,000,000 | 0 | 0 |
| Replica while primary was dead | 6,000,000 | 0 | 0 |
| Primary after its rebuild | 6,000,000 | 0 | 0 |
| Primary while replica was dead | 6,006,000 | 0 | 0 |
| Replica after its rebuild | 6,006,000 | 0 | 0 |
| Primary final pass | 6,006,000 | 0 | 0 |
| Replica final pass | 6,006,000 | 0 | 0 |

The post-recovery suffix advanced the logical replication offset from
`9000009` to `9006009`. At the end, both processes reported the same replication
ID and offset. The replica reported `master_link_status:up`,
`replica_state:live`, `replica_lag:0`, and `goblin_ready:1`. Redpanda's high
watermark was also `9006009`; its last acknowledged record was offset `9006008`
because the high watermark identifies the next record position.

The primary and replica each reported 207,690,670 bytes of logical used memory.
Their RSS values were 304,553,984 and 272,322,560 bytes respectively. The
different RSS values do not represent different logical contents: all counts,
values, replication metadata, and reported allocator usage matched.

The test byte-compared both systemd unit definitions and both NUL-delimited
process argument vectors across their deaths. The primary command-line hash was
`86199041dabad28a6388075bd9be9b7fe61d0408f53e574a55ee341c38bd9ee3`; the
replica hash was
`2ddc1150ddc5aedf925b91c7d71af28a1190bef2f242590989eb3f6b683b0aea`.
Both were identical before and after restart.

## Reproduce it

Build the native C++ worker and first establish the one-million-per-type
baseline with the all-types recovery test:

```sh
cmake --build build --target goblin_core_external_logging_recovery -j
benchmarks/external_logging_recovery.sh \
  --data-type all --snapshot-at 500000 --count 1000000 --pipeline 512
```

Install an adapted copy of
`benchmarks/goblin-core-replica-test.service`, then run:

```sh
benchmarks/primary_replica_recovery.sh \
  --data-type all \
  --baseline-count 1000000 \
  --delta-count 1000 \
  --pipeline 512
```

The runner records both unit files, both process argument vectors, INFO output,
Kafka partition metadata, systemd journal evidence, and every verifier result
under its timestamped result directory. It exits successfully only after both
processes have died separately, restarted unchanged, converged to one offset,
and passed the final exhaustive checks.

## Finding

The replica preserved a complete readable copy while the primary was dead, and
the primary did the same while the replica was dead. More importantly, both
processes also recovered their own complete state after `SIGKILL`: the primary
from snapshot plus Kafka, and the replica from snapshot plus Kafka plus live
firehose handoff. Sequential loss of both database processes produced no
missing value, wrong value, duplicate logical effect, or configuration drift.
