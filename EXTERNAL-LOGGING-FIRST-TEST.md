# External Logging: The First Recovery Test

Goblin Core deliberately rejects the idea that every Redis-shaped server should
also invent and maintain its own append-only log. Logging is a separate systems
problem, and open-source projects such as [Redpanda](https://redpanda.com/) and
[Apache Kafka](https://kafka.apache.org/) live and breathe it. They already own
durable ordered storage, retention, inspection, compaction, and operational
tooling. Goblin Core stays focused on serving hot state efficiently and depends
on one of those systems when an application requires a durable write history.

The resulting persistence model is simple: Goblin Core provides native
`GOBLIN.SAVE`/`--load` snapshots, while Kafka or Redpanda owns everything written
after the snapshot.

## TL;DR: a recoverable instance

The complete, tested procedure is in [INSTALL.md](INSTALL.md). The short recipe
is:

1. Run Redpanda in production mode on durable local storage.
2. Create a Kafka topic with exactly one partition so mutations have one total
   order.
3. Start Goblin Core with `--kafka kafka://BROKER/TOPIC`. On the first start,
   omit `--load` and let it consume the retained topic from the beginning.
4. Create a native snapshot with `GOBLIN.SAVE /path/to/state.snapshot ACCEL`.
5. On subsequent starts, supply that snapshot and the same Kafka topic:

```ini
ExecStart=/usr/local/bin/goblin-core \
  --port 6379 \
  --load /path/to/state.snapshot \
  --kafka kafka://BROKER/TOPIC
Restart=on-failure
```

Keep that invocation in a service manager such as systemd. Recovery then uses
the exact same arguments every time: load the snapshot, replay Kafka to its high
watermark, and only then open client listeners.

## Goblin Core command lines

For a new database with no snapshot, start with the external log alone. Goblin
Core consumes the retained topic from its beginning before it listens on port
6379, then becomes the primary producer for new writes:

```sh
/usr/local/bin/goblin-core \
  --port 6379 \
  --numa eno1 \
  --kafka kafka://192.168.1.49:9092/goblin-core-replication
```

After creating the first snapshot, add `--load` and keep every other argument
unchanged:

```sh
/usr/local/bin/goblin-core \
  --port 6379 \
  --numa eno1 \
  --load /mnt/local/goblin-core/state/goblin.snapshot \
  --kafka kafka://192.168.1.49:9092/goblin-core-replication
```

Create or refresh the snapshot through the normal RESP endpoint:

```sh
redis-cli -h 127.0.0.1 -p 6379 \
  GOBLIN.SAVE /mnt/local/goblin-core/state/goblin.snapshot ACCEL
```

Current native snapshots carry an exact Kafka cursor. `--kafka-time-buffer N`
is only a timestamp-based fallback for a load source without that cursor; it
asks Goblin Core to begin Kafka consumption `N` seconds before the snapshot's
creation time. It is unnecessary in the steady-state command above.

## How external logging works

After a successful write, Goblin Core converts the result into one or more
canonical RESP2 mutations. Each mutation is the smallest independently
overwritten unit. An `HSET` record is keyed by its hash and field; a `ZADD`
record is keyed by its sorted set and member. Those keys make future Kafka log
compaction useful without changing the RESP2 value carried in the record.

Every record also carries a protocol version, a Goblin replication ID, and a
monotonically increasing logical replication offset. The producer is
idempotent, requests `acks=all`, and permits only one in-flight request per
connection. A successful broker delivery advances Goblin Core's acknowledged
Kafka offset.

A native snapshot contains the keyspace plus three pieces of recovery state:
the replication ID, the logical replication offset represented by the
keyspace, and the last broker offset known to be durable. On restart, Goblin
Core loads the native structures, seeks to that broker offset inclusively, and
discards records from another lineage or logical offsets already represented by
the snapshot. It applies the remaining write-only RESP2 records in order.
Listeners stay closed until this startup replay reaches Kafka's high watermark.

The command reply path waits for a mutation to enter the Kafka producer queue;
it does not turn every database command into a synchronous disk round trip. The
delivery callback records what Redpanda has durably acknowledged. That
distinction matters: the throughput below measures asynchronous external
journaling with an `acks=all` producer, followed by a hard-crash recovery test.

## Test machine

The first end-to-end test ran on `thunder`, an older Dell PowerEdge R820:

| Component | Configuration |
|---|---|
| CPU | 4x Intel Xeon E5-4657L v2 at 2.40 GHz |
| Topology | 48 physical cores, 96 threads, 4 NUMA nodes |
| Memory | 1.5 TiB |
| Local storage | 960 GB Intel SSDPED1D960GAY NVMe, XFS |
| OS | Ubuntu 22.04.5 LTS |
| Redpanda | 26.1.13, production mode, 4 cores and 8 GiB |
| Redpanda durability | Write caching disabled; data on local XFS |

Goblin Core was placed on NUMA node 0 with `--numa eno1`. Redpanda and the test
client ran on the same physical server, so this was a storage and recovery test,
not a network benchmark.

Both data paths used TCP. The native C++ client sent pipelined RESP2 commands to
`127.0.0.1:6379`; it did not use UDS or a shared-memory ring. Goblin Core used
Kafka over TCP to Redpanda at `192.168.1.49:9092`. Because both processes were on
`thunder`, that second connection traversed the local TCP/IP stack rather than a
physical network link.

## The crash test

The client deleted hash `foo`, then issued one million commands of the form:

```text
HSET foo 0 0
HSET foo 1 1
...
HSET foo 999999 999999
```

Commands were pipelined 512 deep. Immediately before field `500000`, the client
requested an accelerated native snapshot. Snapshotting forked a stable view of
the first 500,000 fields while the live server continued accepting the second
half of the workload.

The million logged updates completed in **5.09 seconds**, or **196,508 HSET/s**.
The first half took 2.68 seconds and the second took 2.41 seconds, including the
period in which the snapshot was being written. This is pipelined throughput,
not individual-command latency.

After the final write, the test waited one second and sent `SIGKILL` to the
systemd unit's main Goblin Core process. There was no graceful shutdown and no
opportunity for an in-process log flush. The snapshot file had already been
atomically replaced, so the on-disk image contained the first half of `foo`.

Systemd recreated Goblin Core one second later from the unchanged `ExecStart`.
The test byte-compared the full unit definition and the NUL-delimited process
argument vector across the crash; both were identical. The replacement process
loaded 500,000 fields from the snapshot and replayed the remaining Kafka suffix
before becoming ready.

After five minutes, the verifier checked the whole result rather than sampling:

| Check | Result |
|---|---:|
| `HLEN foo` | 1,000,000 |
| Correct field/value pairs | 1,000,000 |
| Missing fields | 0 |
| Incorrect values | 0 |
| Full verification time | 1.84 seconds |

Goblin Core's logical replication offset and Kafka's high watermark both ended
at `2000004`; the last acknowledged Kafka record was offset `2000003`, because a
Kafka high watermark names the next record position. Goblin Core came back with
the exact intended state: no lost suffix, no duplicate logical effect, and no
special restart script reconstructing its configuration.

This first test establishes correctness for one snapshot-plus-log crash on one
host. It is not yet a measurement of failover availability, a remote-broker
network test, or proof against simultaneous database and broker loss. It does
show that the external-logging design works end to end under a real hard kill.

The follow-up [all-types recovery test](EXTERNAL-LOGGING-ALL-TYPES-TEST.md)
repeats the exercise for strings, hashes, sets, sorted sets, lists, and arrays.
The [staggered primary/replica test](EXTERNAL-LOGGING-PRIMARY-REPLICA-TEST.md)
then kills both Goblin processes, one at a time, and verifies the surviving and
rebuilt copies after each death.
