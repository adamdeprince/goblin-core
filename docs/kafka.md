# Kafka write log and recovery

Goblin Core deliberately does not implement an append-only log. If an
application needs a durable write history, Kafka should own that log and Goblin
Core should remain the compact serving layer. `--kafka` makes that arrangement a
startup and runtime feature: Goblin consumes RESP2 mutations from the topic and,
when it is a primary, journals successful writes accepted through either RESP
or SBE back to the same topic. Replicas may consume Kafka for recovery but never
produce a second copy of an upstream write.

Kafka recovery composes with transport-neutral live replication. See [Firehose
replication and Kafka recovery](replication.md) for the snapshot -> Kafka ->
firehose handoff.

## Start a server

`--kafka` accepts either a URI or a semicolon-delimited librdkafka connection
string. Both forms identify one topic:

```bash
goblin-core \
  --kafka 'kafka://kafka-1:9092,kafka-2:9092/goblin-writes' \
  --kafka-ack-mode broker

goblin-core \
  --kafka 'bootstrap.servers=kafka-1:9092,kafka-2:9092;topic=goblin-writes;client.id=prices'
```

The topic must have exactly one partition. Goblin needs one total order for the
replication offset saved in snapshots; spreading the stream across partitions
would require a vector of cursors and would make cross-key operations ambiguous.

URI topic names and query properties use percent encoding. URI query properties
are forwarded to librdkafka, for example `?client.id=prices%20east`. Goblin Core
owns and rejects overrides for brokers, topic, consumer-group and offset
behavior, partition EOF, topic auto-creation, and producer ordering guarantees.

The default vendored build is intentionally dependency-light: it supports
PLAINTEXT brokers plus uncompressed, Snappy, and LZ4 records. Optional TLS/SASL,
curl, gzip, and zstd integrations are disabled rather than varying with whatever
libraries happen to be installed on the build host.

Kafka support is enabled by default. Configure with
`-DGOBLIN_CORE_ENABLE_KAFKA=OFF` to omit the vendored client and reject the
`--kafka` option.

## Acknowledgement policy

`--kafka-ack-mode queued|broker` selects what a successful write reply means.
The default, `queued`, preserves the lowest-latency behavior: Goblin applies the
atomic command, librdkafka accepts its records into the local producer queue,
and Goblin releases the client reply and live firehose batch. Delivery remains
asynchronous. A process or host failure in the interval before broker delivery
can therefore lose a write whose client already saw success.

`broker` makes the external log part of the acknowledgement boundary:

```bash
goblin-core \
  --kafka 'kafka://kafka-1:9092,kafka-2:9092/goblin-writes' \
  --kafka-ack-mode broker \
  --kafka-pending-bytes 16mb
```

Goblin retains each reply and corresponding firehose batch until delivery
callbacks confirm every Kafka record produced by that atomic command. `EXEC`
and scripts may produce several consecutive records; their one reply is
released only after the last record is acknowledged. Other clients continue to
make progress while acknowledgements are pending, and pipelined replies remain
in request order.

Production uses an idempotent producer, `acks=all`, and one in-flight request
per broker connection. The exact storage guarantee behind `acks=all` remains a
Kafka/Redpanda cluster policy: replication factor, in-sync replica requirements,
and disk flush policy belong to the broker. Broker mode means the configured
broker policy accepted the record; it does not silently strengthen that policy
to a physical `fsync` per message.

`--kafka-pending-bytes` bounds retained, unacknowledged replication payloads and
defaults to 16 MiB. At the watermark Goblin pauses command input while it keeps
polling Kafka and draining output. The command already executing remains atomic
and may take the retained total across the watermark. Input resumes as delivery
callbacks release batches.

`INFO` reports `kafka_ack_mode`, acknowledged logical offset, pending record and
byte counts, oldest pending age, retained batch bytes, and whether Kafka input
backpressure is active. A broker acknowledgement can still be followed by a
lost client connection before the reply arrives; applications that retry
non-idempotent operations need their usual idempotency key.

The measured latency and batching tradeoffs are in the
[Kafka durability benchmark](../KAFKA-DURABILITY-BENCHMARK.md).

## Record contract

One Kafka record value contains exactly one complete canonical RESP2 mutation.
This record carries `SET key value`:

```text
*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
```

Inline commands, RESP3 values, empty/tombstone records, partial frames, and
multiple commands in one record are rejected. The strict one-record/one-command
boundary makes each Kafka record independently replayable. One atomic client
command, transaction, or script may yield several consecutive records; its live
firehose batch retains the enclosing atomic boundary.

Records produced by Goblin carry `goblin-replication-version`,
`goblin-replication-id`, and `goblin-replication-offset` headers. The replication
ID names one server lineage; the logical offset orders writes inside it. Records
from an older lineage are discarded before their remaining headers or payload
are interpreted during snapshot recovery, and records at or behind the
snapshot's logical offset are harmless duplicates.

Kafka keys identify the smallest independently overwritten unit. A sorted-set
member uses `(ZADD, key, member)`, a hash field uses `(HSET, key, field)`, and a
set member uses `(SADD, key, member)`. The key is binary and length-framed, so
arbitrary Redis key/member bytes remain unambiguous. Ordered operations that
cannot be compacted independently are written without a Kafka key.

Only commands on the logical keyspace-mutation allowlist run. That includes the
supported writes for strings, counters, TTLs, hashes, sets, sorted sets, lists,
sparse arrays, and Goblin's native conditional-write helpers. Read-only legacy
records are parsed and counted but otherwise ignored. A Goblin-authored record
that claims a logical offset but contains a read is rejected.

An unknown or malformed command is not treated as a read. A recognized write
that returns an error is also fatal: startup aborts, or a running server closes
after reporting the topic, partition, offset, and error. Silently skipping a bad
write would make the serving state disagree with the durable log.

## Startup and snapshots

With no snapshot, Goblin starts at the earliest retained record:

```bash
goblin-core --kafka 'kafka://127.0.0.1:9092/goblin-writes'
```

Native snapshots save three replication values with the keyspace: the
replication ID, the highest logical replication offset represented by the
snapshot, and the last broker offset Kafka acknowledged. With `--load`, Goblin
seeks to that broker offset **inclusively**. It intentionally rereads the
boundary: old-lineage records are discarded, matching records already in the
snapshot are discarded by logical offset, and the first later matching record
is applied.

Inclusive replay makes the snapshot/Kafka acknowledgement race conservative.
Reprocessing a short overlap is safe; skipping one write is not. Kafka log
compaction can create valid offset holes, so recovery requires monotonic offsets
but does not require them to be adjacent.

Snapshots made before exact Kafka cursors were added fall back to the first
record whose Kafka timestamp is at or after the snapshot file's creation time:

```bash
goblin-core \
  --load /srv/goblin/prices.gcsn \
  --kafka-time-buffer 5 \
  --kafka 'kafka://127.0.0.1:9092/goblin-writes'
```

`--kafka-time-buffer N` subtracts `N` whole seconds from that fallback file
timestamp. Linux uses filesystem birth time through `statx`; macOS and BSD use
their native birth-time field. A filesystem without creation time falls back to
modification time and prints a warning.

Goblin captures the topic's high-water mark, replays through it, and only then
creates TCP, UDS, shared-memory-ring, ExaSock, and RDMA listeners. Clients cannot
observe the snapshot before its initial Kafka catch-up completes. Records that
arrive after the captured mark remain queued for the normal server loop.

When an upstream `--replica-*` source is also configured, Goblin connects its
firehose before Kafka replay and buffers that live suffix in a fixed page-backed
mapping. Kafka catches up to at least the logical offset reported by the
firehose hello; overlapping live frames are skipped by logical offset, the
Kafka consumer closes, and the firehose becomes the sole upstream before client
listeners open.

The same handoff runs after a live firehose disconnect. The replica becomes
not-ready, reconnects upstream, and creates a fresh inclusive Kafka consumer
only when the new hello is ahead of local state. Kafka closes again after the
target and buffered suffix are applied. An exact-offset reconnect does not need
Kafka; a positive gap cannot be bridged without it.

## Runtime behavior

librdkafka fetches on its own I/O threads, but those threads never touch the
store. Its consumer queue wakes Goblin's existing `poll()` loop through a
nonblocking pipe; Goblin parses and executes records on the single server thread
in bounded batches. Client commands and Kafka writes therefore share the same
atomic command execution model with no store lock.

Goblin does not commit a consumer-group offset. The restart cursor belongs to the
database snapshot, so copying a snapshot also copies its precise replay point.
Without a snapshot Goblin starts at the earliest retained record.

Primary-side production always uses Kafka's idempotent producer, `acks=all`, and
one in-flight request per connection. Delivery callbacks run on the Goblin
server thread. A successful callback advances the broker offset that the next
snapshot captures. It also releases replies and firehose batches in `broker`
mode. A permanent delivery failure stops the server rather than allowing the
serving state and durable log to diverge silently.

A server with an upstream firehose does not create a Kafka producer. This is
true for replicas of replicas as well: only the origin primary owns the durable
journal.

Transient broker and leader changes are left to librdkafka's reconnect logic.
On a primary, permanent Kafka errors and invalid write records stop the process
with a nonzero exit status rather than allow unjournaled serving state. During
replica recovery, they instead leave the read-only replica degraded and
not-ready while a fresh firehose/Kafka handoff is retried.

## Library and license

Goblin Core vendors librdkafka 2.15.0 under its BSD-2-Clause license. The
upstream license set is retained under `third_party/librdkafka/`, recorded in
`NOTICE`, and is compatible with Goblin Core's Apache-2.0 license. The local
integration test is exercised against Apache Kafka 4.3.1.
