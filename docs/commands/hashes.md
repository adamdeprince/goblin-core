# Hash commands

Hashes map binary-safe field names to binary-safe values. Small hashes use one
packed listpack; larger efficient hashes promote to arena storage plus a Swiss
index, while the real-time implementation uses incrementally split linear-hash
buckets. See the [implementation guide](../real-time-hashes.md) for those layout
and latency tradeoffs.

| Command | Purpose |
|---|---|
| `HSET`, `HMSET`, `HSETNX` | Set one or more fields. |
| `HGET`, `HMGET`, `HGETALL` | Read fields and values. |
| `HDEL`, `HEXISTS`, `HLEN`, `HSTRLEN` | Delete or inspect fields. |
| `HKEYS`, `HVALS` | Read all fields or all values. |
| `HINCRBY`, `HINCRBYFLOAT` | Atomically increment numeric field values. |
| `HRANDFIELD` | Sample fields, optionally with values. |
| `HSCAN` | Incrementally visit fields with bounded work. |

Missing keys behave as empty hashes. A key holding another type returns
`WRONGTYPE`.

## HMSET

```text
HMSET key field value [field value ...]
```

`HMSET` is the legacy alias for multi-field `HSET`. It performs the same atomic
write but returns `OK`, preserving the reply expected by older clients.

## HINCRBYFLOAT

```text
HINCRBYFLOAT key field increment
```

Treat a missing field as zero, add a finite floating-point increment, store the
shortest round-trippable decimal result, and return that text as a bulk string.
Invalid existing text, a non-finite increment, or a non-finite result is an
error and leaves the field unchanged.

## HRANDFIELD

```text
HRANDFIELD key [count [WITHVALUES]]
```

Without `count`, return one field or nil for an empty hash. A positive count
returns up to that many distinct fields. A negative count returns exactly its
absolute value and may repeat fields. `WITHVALUES` returns a flat sequence of
field/value pairs and is valid only when `count` is present. Sampling addresses
the hash's dense field table directly rather than first materializing every
value.

## Implementations and scans

The hash family is also addressable through `GOBLIN.RT.*` and the historical
`GOBLIN.EFFICENT.*` spelling. A qualified mutating command selects the layout
when it creates a hash; subsequent commands operate on that stored layout.
Standard names use `--hash-implementation`, or the real-time layout when
`--real-time` is enabled.

For `HSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]`, including its
cursor and mutation contract, see the [iteration reference](iteration.md#hscan).
