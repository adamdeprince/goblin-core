# Kafka write log and recovery

Goblin Core deliberately does not implement an append-only log. If an
application needs a durable write history, Kafka should own that log and Goblin
Core should remain the compact serving layer. `--kafka` makes that arrangement a
startup and runtime feature: Goblin consumes RESP2 writes from the topic and
journals writes accepted through its RESP command path back to the same topic.

## Start a server

`--kafka` accepts either a URI or a semicolon-delimited librdkafka connection
string. Both forms identify one topic:

```bash
goblin-core \
  --kafka 'kafka://kafka-1:9092,kafka-2:9092/goblin-writes'

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

## Record contract

One Kafka record value contains exactly one complete RESP2 array. This record
carries `SET key value`:

```text
*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
```

Inline commands, RESP3 values, empty/tombstone records, partial frames, and
multiple commands in one record are rejected. The strict one-record/one-command
boundary makes a Kafka offset identify one atomic Goblin mutation.

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

## Runtime behavior

librdkafka fetches on its own I/O threads, but those threads never touch the
store. Its consumer queue wakes Goblin's existing `poll()` loop through a
nonblocking pipe; Goblin parses and executes records on the single server thread
in bounded batches. Client commands and Kafka writes therefore share the same
atomic command execution model with no store lock.

Goblin does not commit a consumer-group offset. The restart cursor belongs to the
database snapshot, so copying a snapshot also copies its precise replay point.
Without a snapshot Goblin starts at the earliest retained record.

Primary-side production is asynchronous and uses Kafka's idempotent producer,
`acks=all`, and one in-flight request per connection. Delivery callbacks run on
the Goblin server thread. A successful callback advances the broker offset that
the next snapshot captures; a permanent delivery failure stops the server rather
than allowing the serving state and durable log to diverge silently.

Transient broker and leader changes are left to librdkafka's reconnect logic.
Permanent consumer errors and invalid write records stop serving with a nonzero
exit status.

## Library and license

Goblin Core vendors librdkafka 2.15.0 under its BSD-2-Clause license. The
upstream license set is retained under `third_party/librdkafka/`, recorded in
`NOTICE`, and is compatible with Goblin Core's Apache-2.0 license. The local
integration test is exercised against Apache Kafka 4.3.1.
