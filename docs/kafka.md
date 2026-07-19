# Kafka write-log ingestion

Goblin Core deliberately does not implement an append-only log. If an
application needs a durable write history, Kafka should own that log and Goblin
Core should remain the compact serving layer. `--kafka` makes that arrangement a
startup and runtime feature rather than an application-specific replay step.

## Start a consumer

`--kafka` accepts either a URI or a semicolon-delimited librdkafka connection
string. Both forms identify one topic:

```bash
goblin-core \
  --kafka 'kafka://kafka-1:9092,kafka-2:9092/goblin-writes'

goblin-core \
  --kafka 'bootstrap.servers=kafka-1:9092,kafka-2:9092;topic=goblin-writes;client.id=prices'
```

URI topic names and query properties use percent encoding. URI query properties
are forwarded to librdkafka, for example
`?client.id=prices%20east`. Goblin Core owns and rejects overrides for
`bootstrap.servers`, `topic`, consumer-group and offset behavior, partition EOF,
and topic auto-creation.

The default vendored build is intentionally dependency-light: it supports
PLAINTEXT brokers plus uncompressed, Snappy, and LZ4 records. Optional TLS/SASL,
curl, gzip, and zstd integrations are disabled rather than varying with whatever
libraries happen to be installed on the build host.

Kafka support is enabled by default. Configure with
`-DGOBLIN_CORE_ENABLE_KAFKA=OFF` to omit the vendored client and reject the
`--kafka` option.

## Record contract

One Kafka record value contains exactly one complete RESP2 array. Kafka keys and
headers are not interpreted. This record carries `SET key value`:

```text
*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n
```

Inline commands, RESP3 values, empty/tombstone records, partial frames, and
multiple commands in one record are rejected. The strict one-record/one-command
boundary makes a Kafka offset identify one atomic Goblin operation.

Only commands on the logical keyspace-mutation allowlist run. That includes the
supported writes for strings, counters, TTLs, hashes, sets, sorted sets, lists,
sparse arrays, and Goblin's native conditional-write helpers. Read-only
commands are parsed and counted but otherwise ignored. Connection state,
Pub/Sub, scripting, and administrative commands such as `GOBLIN.SAVE`,
`GOBLIN.LOAD`, and `GOBLIN.OPTIMIZE` are also filtered.

An unknown or malformed command is not treated as a read. A recognized write
that returns an error is also fatal: startup aborts, or a running server closes
after reporting the topic, partition, offset, and error. Silently skipping a
bad write would make the serving state disagree with the durable log.

## Startup and snapshots

With no snapshot, Goblin assigns every current partition and starts at its
earliest retained offset:

```bash
goblin-core --kafka 'kafka://127.0.0.1:9092/goblin-writes'
```

With `--load`, replay starts at the first record whose Kafka timestamp is at or
after the snapshot file's creation time:

```bash
goblin-core \
  --load /srv/goblin/prices.gcsn \
  --kafka-time-buffer 5 \
  --kafka 'kafka://127.0.0.1:9092/goblin-writes'
```

`--kafka-time-buffer N` subtracts `N` whole seconds from the file timestamp. It
defaults to zero and is valid only with both `--load` and `--kafka`. Linux uses
filesystem birth time through `statx`; macOS and BSD use their native birth-time
field. A filesystem without creation time falls back to modification time and
prints a warning.

Goblin captures each partition's high-water mark, replays through those marks,
and only then creates TCP, UDS, shared-memory-ring, ExaSock, and RDMA listeners.
Clients therefore cannot observe the snapshot before its initial Kafka catch-up
has completed. Records arriving after the captured marks remain queued and are
consumed by the normal server loop.

The time buffer deliberately permits overlap. Replaying non-idempotent commands
such as `INCR` from before the snapshot changes their result, so use a buffer
only when the snapshot publication workflow needs that safety margin.

## Runtime behavior

librdkafka fetches on its own I/O threads, but those threads never touch the
store. Its consumer queue wakes Goblin's existing `poll()` loop through a
nonblocking pipe; Goblin parses and executes records on the single server thread
in bounded batches. Socket clients and Kafka writes therefore share the same
atomic command execution model with no store lock.

Goblin does not commit consumer offsets. A restart derives position again from
the loaded snapshot, or from the earliest retained record when no snapshot is
provided. Every Goblin instance assigns every partition, so multiple serving
instances each reconstruct a complete copy instead of dividing the topic like a
consumer group.

Kafka ordering is per partition. Producers should use a stable Kafka key for all
commands that touch the same Goblin key; there is no defined total order across
partitions. Partitions added after Goblin starts are picked up on the next
restart.

Transient broker and leader changes are left to librdkafka's reconnect logic.
Permanent consumer errors and invalid write records stop serving with a nonzero
exit status.

## Library and license

Goblin Core vendors librdkafka 2.15.0 under its BSD-2-Clause license. The
upstream license set is retained under `third_party/librdkafka/`, recorded in
`NOTICE`, and is compatible with Goblin Core's Apache-2.0 license. The local
integration test is exercised against Apache Kafka 4.3.1.
