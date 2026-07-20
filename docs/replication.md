# Firehose replication and Kafka recovery

Goblin Core separates live replication from durable history. `GOBLIN.FIREHOSE`
streams writes directly from one Goblin server to another over any supported
transport. Kafka owns the durable log, and native snapshots provide compact
restart points. A replica can combine all three: load a snapshot, replay Kafka,
and hand off to a firehose without opening its client listeners in between.

## Configure an upstream

A server becomes a read-only replica when exactly one `--replica-*` source is
configured:

```sh
# Unix-domain socket
goblin-core --replica-uds /run/goblin/primary.sock

# Shared-memory ring
goblin-core --replica-ring /run/goblin/primary.ring

# Polled one-sided RDMA ring
goblin-core --replica-rdma 192.0.2.10 18515 2097152

# TCP; non-loopback sources use TLS automatically
goblin-core \
  --replica-tcp 192.0.2.10 6379 \
  --replica-tls-ca-file /etc/goblin/replication-ca.pem \
  --replica-tls-server-name primary.example.net
```

The firehose negotiation and records use RESP2 over every transport, including
ring and RDMA. Their ordering and recovery semantics therefore do not change
with the channel.

A replica rejects ordinary client writes. It can still serve reads and accept
downstream `GOBLIN.FIREHOSE` subscribers, which permits primary -> replica ->
replica chains. Goblin does not detect replication cycles; do not configure
one.

## Authentication and TLS

When the upstream uses `--auth-file`, give the follower a username and a file
containing one password line:

```sh
goblin-core \
  --replica-uds /run/goblin/primary.sock \
  --replica-auth-user replicator \
  --replica-auth-password-file /etc/goblin/replicator.password
```

The password is read at startup rather than exposed in the process argument
list. The follower sends `AUTH` before requesting its firehose. An upstream can
instead trust every RESP connection on its ring or RDMA listeners with
`--no-auth-ring` or `--no-auth-rdma`.

Non-loopback `--replica-tcp` connections use TLS even when `--replica-tls` is
omitted. The follower verifies the certificate against the system trust store,
or against `--replica-tls-ca-file` when supplied, and verifies the source
address as the certificate identity. Use `--replica-tls-server-name` to verify
a DNS identity instead. Loopback TCP is plaintext unless `--replica-tls` is
specified.

## Wire contract

`GOBLIN.FIREHOSE` is an internal same-version protocol, not an application
command. After authentication, a subscriber sends:

```text
GOBLIN.FIREHOSE
```

The primary replies with a hello frame containing the protocol version, its
128-bit replication ID, and its current logical offset. The connection then
becomes one-way. Each subsequent frame contains:

- the replication protocol version;
- the same replication ID;
- the first logical offset in the batch; and
- one or more canonical RESP2 mutations.

One successful client command is emitted as one firehose batch. `EXEC`, a
script, or another atomic helper can produce several canonical mutations in
that batch, preserving the command's atomic boundary at the receiving server.
Each canonical mutation has its own consecutive logical offset and can be
replayed independently from Kafka.

RESP and SBE writes enter the same capture path. Commands with nondeterministic
results are converted to exact post-state mutations before replication. For
example, a random `SPOP` replicates the members that were actually removed,
not another random-pop request.

Firehose frames are a private Goblin protocol. Both servers must run exactly
the same Goblin Core version. Use ordinary RESP for compatibility with other
servers and clients.

## Replication identity and offsets

The replication ID identifies one history. The logical replication offset
starts at zero and advances once per canonical mutation. A native snapshot
saves:

1. the replication ID;
2. the highest logical offset represented by the keyspace; and
3. the last Kafka broker offset acknowledged by the producer.

The Kafka broker offset and the Goblin logical offset are different values.
The broker offset says where to seek in one Kafka partition. The logical offset
says which mutations from the matching Goblin lineage are already in the
snapshot.

## Snapshot, Kafka, and live handoff

For a durable replica restart, configure all three inputs:

```sh
goblin-core \
  --load /srv/goblin/state.gcsn \
  --kafka 'kafka://kafka-1:9092/goblin-writes' \
  --replica-tcp primary.example.net 6379 \
  --replica-tls-ca-file /etc/goblin/replication-ca.pem \
  --replica-auth-user replicator \
  --replica-auth-password-file /etc/goblin/replicator.password
```

Startup proceeds in this order:

1. Load the snapshot and its replication metadata.
2. Connect the firehose before replay begins. Its hello captures a live target
   offset while later messages accumulate in a fixed buffer.
3. Seek Kafka to the saved broker offset **inclusively**.
4. Discard the short prefix whose replication ID predates the snapshot lineage.
5. For the matching lineage, discard logical offsets at or below the snapshot
   offset and apply later records.
6. Continue until Kafka reaches at least the offset observed in the firehose
   hello.
7. Apply the buffered firehose suffix. Any overlap already applied from Kafka
   is skipped by logical offset.
8. Close the Kafka consumer, make the firehose the sole live input, and only
   then open client listeners.

The inclusive Kafka seek is intentional. A snapshot may capture a broker
acknowledgement immediately before or after the corresponding state becomes
visible. Re-reading a small overlap is safe because the ID and logical offset
remove duplicates; starting after the saved broker offset could skip a write.
Kafka compaction may leave holes in broker offsets, so recovery requires
monotonic ordering, not adjacency.

Without Kafka, the loaded state must be at exactly the offset reported by the
upstream hello. A fresh empty replica can therefore join an upstream only while
that upstream is still at offset zero. Goblin does not currently transfer a
full snapshot over the firehose; load a current snapshot or configure Kafka to
bootstrap an established server.

## Kafka ownership

Only a primary produces new Kafka records. A replica may consume Kafka during
recovery, but it never journals the writes it receives from its upstream. This
prevents replica chains from duplicating one logical mutation in the durable
log.

Kafka record keys identify the smallest independently overwritten unit, which
makes log compaction useful without merging unrelated state. For example,
`ZADD leaderboard 1 alice` and a later score update for `alice` use a key built
from `ZADD`, `leaderboard`, and `alice`. Hash fields and set members follow the
same rule. Ordered mutations that cannot be compacted independently have no
Kafka key. See [Kafka write log and recovery](kafka.md) for the complete record
contract.

## Buffers and failure behavior

`--replication-buffer-bytes N` controls two fixed, anonymous `mmap` buffers:
the startup handoff buffer on a replica and the output queue for each downstream
firehose client. The default is 1 MiB. The requested size is always rounded up
to a whole OS page and prefaulted.

Goblin fails closed rather than silently diverging:

- a lineage change or a gap in a live firehose stops the replica;
- an invalid or failed replicated write stops the replica;
- exhausting the startup handoff buffer aborts startup;
- a downstream that fills its fixed output queue is disconnected;
- losing the upstream after startup stops the replica process.

Automatic reconnect and full-snapshot transfer are not implemented yet.
`GOBLIN.LOAD` is rejected on a running replicated server; supply `--load` at
startup so the lineage and offsets can be validated before listeners open.

## Observe the role

`ROLE` reports `master` for a primary and `slave` plus the upstream transport
description and current logical offset for a replica. `INFO` exposes `role`,
`master_replid`, `master_repl_offset`, and `kafka_acknowledged_offset`. The last
field is `-1` until a Kafka broker acknowledgement is available.
