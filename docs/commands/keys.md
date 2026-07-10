# Key commands

These commands operate on a key regardless of the type it holds, across the one
unified keyspace (sorted sets, hashes, and [strings](strings.md)).

| Command | Summary |
|---|---|
| [`DEL`](#del) | Delete one or more keys. |
| [`EXISTS`](#exists) | Count how many of the given keys exist. |
| [`TYPE`](#type) | The type stored at a key. |

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
`hash`, or `none` when the key does not exist. (Because a name maps to one
object, exactly one of these applies.)

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

## See also

- [strings.md](strings.md) — the string value type and its commands.
