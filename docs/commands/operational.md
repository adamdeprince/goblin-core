# Operational compatibility commands

Goblin Core exposes a deliberately small operational surface for clients and
health checks. These replies describe capabilities that actually exist rather
than synthesizing unsupported database settings.

## TIME

```text
TIME
```

Return Unix time as two bulk strings: whole seconds and the microsecond portion
of the current second.

## ROLE

```text
ROLE
```

A primary returns the Redis-compatible master shape with its current logical
replication offset:

```text
1) "master"
2) (integer) <offset>
3) (empty array)
```

The downstream array is currently empty even when internal firehose subscribers
are connected; Goblin does not describe those transport-neutral connections as
Redis replica descriptors.

A server configured with one `--replica-*` source returns:

```text
1) "slave"
2) "<upstream transport description>"
3) (integer) 0
4) "connected" | "connecting" | "down"
5) (integer) <offset>
```

The third element is zero because a ring, UDS, RDMA, or TLS source does not
necessarily have one meaningful Redis TCP port. The transport and endpoint are
included in the second element. See [Firehose replication and Kafka
recovery](../replication.md) for setup and recovery semantics. `connected`
means the replica is ready; `connecting` covers connection and Kafka/firehose
handoff states; `down` means a lineage or offset safety check left it degraded.
`INFO` exposes the more detailed state and last error.

## GOBLIN.FIREHOSE

```text
GOBLIN.FIREHOSE
```

Turn the live connection into a one-way replication stream. The initial reply
identifies the replication protocol version, lineage ID, and current logical
offset; subsequent internal frames contain canonical RESP2 mutations. This is a
same-version Goblin server protocol, not a general client API. Authentication,
transport setup, buffering, and failure behavior are documented in [Firehose
replication and Kafka recovery](../replication.md).

## CONFIG GET

```text
CONFIG GET pattern [pattern ...]
```

Patterns use Redis glob syntax. RESP2 returns alternating name/value elements;
RESP3 returns a map. Only the following truthful compatibility values are
published:

| Name | Value | Meaning |
|---|---|---|
| `databases` | `1` | Only database zero exists. |
| `appendonly` | `no` | Goblin Core does not own an append-only log. |
| `save` | empty | No Redis-style periodic save rules are configured. |
| `maxmemory` | configured bytes, or `0` | Persistent-store ceiling selected by `--maxmemory`; `0` is unlimited. |
| `maxmemory-policy` | `noeviction` | Capacity-growing writes are rejected rather than resolved by key eviction. |

`CONFIG SET` and every other `CONFIG` subcommand are rejected. Snapshot and
Kafka durability are configured through Goblin Core's own command-line and
`GOBLIN.*` surfaces rather than being misrepresented as Redis configuration.
See the [hard memory ceiling guide](../maxmemory.md) for the accounting boundary
and OOM behavior.
