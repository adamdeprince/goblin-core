# Key commands

These commands operate on a key regardless of the type it holds, across the one
unified keyspace (sorted sets, hashes, and [strings](strings.md)).

| Command | Summary |
|---|---|
| [`DEL`](#del) | Delete one or more keys. |
| [`EXISTS`](#exists) | Count how many of the given keys exist. |
| [`TYPE`](#type) | The type stored at a key. |
| [`DBSIZE`](#dbsize) | Count live keys in database zero. |
| [`RENAME` / `RENAMENX`](#rename--renamenx) | Move a value to a new key name. |
| [`COPY`](#copy) | Deep-copy a value, including its expiry. |
| [`RANDOMKEY`](#randomkey) | Return one live key. |
| [`TOUCH`](#touch) | Count existing keys (no LRU metadata is maintained). |
| [`SCAN`](iteration.md#scan) | Incrementally visit keys with optional glob and type filters. |

## DEL

```
DEL key [key ...]
```

Delete each key that exists (of any type) and reply with the number removed.

```
> SET a 1
OK
> ZADD b 1 m
(integer) 1
> DEL a b missing
(integer) 2
```

## EXISTS

```
EXISTS key [key ...]
```

Reply with how many of the given keys exist. A key repeated in the argument list
is counted each time it is present.

```
> SET a 1
OK
> EXISTS a a missing
(integer) 2
```

## TYPE

```
TYPE key
```

Reply with the type stored at `key` as a simple string: `string`, `zset`,
`hash`, `list`, `set`, `array`, or `none` when the key does not exist. (Because
a name maps to one object, exactly one of these applies.)

```
> SET a 1
OK
> TYPE a
string
> ZADD b 1 m
(integer) 1
> TYPE b
zset
> TYPE missing
none
```

## DBSIZE

```
DBSIZE
```

Return the exact number of live keys in database zero. Due expirations are
drained before the count is produced.

## RENAME / RENAMENX

```
RENAME source destination
RENAMENX source destination
```

Move the source object to `destination`, preserving its type, representation,
and TTL. `RENAME` replaces an existing destination and replies `OK`;
`RENAMENX` leaves both keys unchanged and returns `0` when the destination
exists, otherwise it returns `1`. A missing source is an error.

## COPY

```
COPY source destination [DB 0] [REPLACE]
```

Deep-copy any supported value type. The source and destination do not share
mutable state, and the source TTL is copied to the destination. An existing
destination makes the command return `0` unless `REPLACE` is present. Goblin
Core has one database, so only `DB 0` is accepted.

## RANDOMKEY

```
RANDOMKEY
```

Return one live key as a bulk string, or nil when the keyspace is empty. Due
expirations are drained before selection.

## TOUCH

```
TOUCH key [key ...]
```

Return how many arguments name live keys; repeated keys are counted repeatedly.
Goblin Core does not maintain an LRU clock, so `TOUCH` intentionally performs
the compatibility-visible existence operation without allocating per-key access
metadata.

## See also

- [strings.md](strings.md) — the string value type and its commands.
- [iteration.md](iteration.md) — bounded keyspace and collection traversal.
