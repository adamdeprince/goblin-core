# Operational compatibility commands

Goblin Core exposes a deliberately small operational surface for clients and
health checks. These replies describe capabilities that actually exist; they do
not synthesize replication, persistence, or database settings that the server
does not implement.

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

Goblin Core is currently a single-node primary. `ROLE` therefore returns the
Redis-compatible master shape with replication offset zero and no replicas:

```text
1) "master"
2) (integer) 0
3) (empty array)
```

This is intentionally not a claim that replication exists. The reply remains
`master` until Goblin Core gains a real replication role model.

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
| `maxmemory` | `0` | No configured process memory ceiling. |
| `maxmemory-policy` | `noeviction` | Writes are not resolved by key eviction. |

`CONFIG SET` and every other `CONFIG` subcommand are rejected. Snapshot and
Kafka durability are configured through Goblin Core's own command-line and
`GOBLIN.*` surfaces rather than being misrepresented as Redis configuration.
