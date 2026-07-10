# String commands

The **string** is Goblin Core's third value type, alongside the sorted set and
hash. A key holds a single binary-safe byte string (keys and values are both
binary-safe). Strings share the one **unified keyspace** with zsets and hashes:
a name resolves to **at most one object**, so `SET` over an existing sorted set
replaces it, and a string command against a non-string key is a
[`WRONGTYPE`](#type-errors) error.

| Command | Summary |
|---|---|
| [`SET`](#set) | Set a key to a value (optionally only if absent). |
| [`SETNX`](#setnx) | Set only if the key does not exist. |
| [`GET`](#get) | Get a key's value. |
| [`GETSET`](#getset) | Set a new value, return the old one. |
| [`GETDEL`](#getdel) | Return a key's value and delete it. |
| [`STRLEN`](#strlen) | Length of the value in bytes. |
| [`APPEND`](#append) | Append to a value (creating it if absent). |
| [`INCR` / `DECR` / `INCRBY` / `DECRBY`](#incr--decr--incrby--decrby) | Integer arithmetic on the value. |
| [`INCRBYFLOAT`](#incrbyfloat) | Floating-point arithmetic on the value. |
| [`GETRANGE`](#getrange) | A substring of the value. |
| [`SETRANGE`](#setrange) | Overwrite part of the value at an offset. |
| [`MSET`](#mset) | Set several keys at once. |
| [`MGET`](#mget) | Get several keys at once. |

For `DEL`, `EXISTS`, and `TYPE` (which work on any type), see
[keys.md](keys.md).

## Memory model — why the value is cheap

Memory efficiency is the project's hero feature, and strings are built for it:

- **Small-value inlining.** A value of **≤ 14 bytes lives entirely inside the
  key's object slot** — counters, flags, short ids, most integers cost **zero**
  arena bytes. A larger value keeps a 6-byte prefix inline and spills the rest
  into the shared key arena.
- **One arena, no total cap.** Keys and value tails share a single arena
  addressed by a 32/32 `{block, offset}` pair, so the total size of all keys and
  values is not capped the way a single 32-bit offset would (4 GiB) cap it.
- **64 KiB per value.** A single value is capped at 64 KiB by design. A larger
  write is rejected:

  ```
  > SET big <70 KB blob>
  (error) ERR value is larger than the 64 KiB limit; use https://goblin-store.dev
  ```

  Large blobs belong in the object store, not the keyspace.

## Type errors

Because a name maps to one object, the type-specific commands below reject a key
that already holds a different type:

```
> ZADD board 1 alice
(integer) 1
> GET board
(error) WRONGTYPE Operation against a key holding the wrong kind of value
```

The exceptions are the commands that **do not require** an existing string:
`SET`, `SETNX`, and `MSET` create or replace regardless of the current type, and
`MGET` / `DEL` / `EXISTS` / `TYPE` are type-agnostic. So `SET` **clobbers**:

```
> SET board "now a string"     # replaces the sorted set
OK
> TYPE board
string
```

> **TTL is not yet supported.** `SET`'s `EX` / `PX` / `EXAT` / `PXAT` / `XX` /
> `GET` / `KEEPTTL` options are rejected as a syntax error for now; only `NX` is
> accepted. Expiry is on the roadmap.

---

## SET

```
SET key value [NX]
```

Set `key` to `value`, replacing any existing value **of any type**. Replies
`+OK`. With `NX`, set only if the key is absent — replying `+OK` when stored, or
a nil bulk string when the key already exists.

```
> SET greeting "hello"
OK
> SET greeting "hi" NX
(nil)
> GET greeting
"hello"
```

## SETNX

```
SETNX key value
```

Set `key` to `value` only if it does not already exist (of any type). Replies
`1` if set, `0` otherwise. Equivalent to `SET key value NX` but with an integer
reply.

```
> SETNX lock "1"
(integer) 1
> SETNX lock "2"
(integer) 0
```

## GET

```
GET key
```

The value at `key`, or a nil bulk string if the key is absent. `WRONGTYPE` if
the key holds a non-string.

```
> SET n 42
OK
> GET n
"42"
> GET missing
(nil)
```

## GETSET

```
GETSET key value
```

Set `key` to `value` and return its **previous** value (nil if it did not
exist). `WRONGTYPE` if the key holds a non-string.

```
> SET counter 10
OK
> GETSET counter 0
"10"
> GET counter
"0"
```

## GETDEL

```
GETDEL key
```

Return the value at `key` and delete the key, or nil if it was absent.

```
> SET token "abc"
OK
> GETDEL token
"abc"
> EXISTS token
(integer) 0
```

## STRLEN

```
STRLEN key
```

The length of the value in bytes, or `0` if the key is absent.

```
> SET name "goblin"
OK
> STRLEN name
(integer) 6
```

## APPEND

```
APPEND key value
```

Append `value` to the existing value (creating the key with `value` if it did
not exist), and reply with the **new length**. Rejected if the result would
exceed 64 KiB.

```
> APPEND log "line1\n"
(integer) 6
> APPEND log "line2\n"
(integer) 12
```

## INCR / DECR / INCRBY / DECRBY

```
INCR key
DECR key
INCRBY key increment
DECRBY key decrement
```

Parse the value as a base-10 signed 64-bit integer, add (or subtract) the delta,
store the result as text, and reply with the new value. A missing key is treated
as `0`. An error is returned if the value is not an integer, or if the result
overflows a 64-bit integer.

```
> SET hits 100
OK
> INCR hits
(integer) 101
> INCRBY hits 9
(integer) 110
> DECRBY hits 10
(integer) 100
> SET hits "abc"
OK
> INCR hits
(error) ERR value is not an integer or out of range
```

## INCRBYFLOAT

```
INCRBYFLOAT key increment
```

Add a floating-point `increment` to the value (missing key treated as `0`) and
reply with the new value in its shortest round-trippable text form (so `3.0`
prints as `3`). An error is returned if the value or the result is not a finite
number.

```
> SET price 10.5
OK
> INCRBYFLOAT price 0.1
"10.6"
> INCRBYFLOAT price 1.4
"12"
```

## GETRANGE

```
GETRANGE key start end
```

The inclusive substring between offsets `start` and `end`. Negative offsets
count from the end (`-1` is the last byte); out-of-range offsets are clamped.
Returns an empty string if `start > end` after normalization, or if the key is
absent or empty.

```
> SET s "Hello World"
OK
> GETRANGE s 0 4
"Hello"
> GETRANGE s -5 -1
"World"
> GETRANGE s 0 -1
"Hello World"
```

## SETRANGE

```
SETRANGE key offset value
```

Overwrite the value starting at `offset`, zero-padding any gap if `offset` is
past the current end, and reply with the **new length**. Creates the key if it
does not exist. A negative offset is an error; a result over 64 KiB is rejected.
An empty `value` never creates or grows the key (it replies with the current
length, or `0` when absent).

```
> SET s "Hello World"
OK
> SETRANGE s 6 "Redis"
(integer) 11
> GET s
"Hello Redis"
> SETRANGE pad 5 "xy"     # zero-pads offsets 0..4
(integer) 7
```

## MSET

```
MSET key value [key value ...]
```

Set every given key to its value (replacing any type), atomically. Always
replies `+OK`.

```
> MSET a 1 b 2 c 3
OK
```

## MGET

```
MGET key [key ...]
```

Return the values of the given keys as an array, with a **nil** for any key that
is missing **or holds a non-string** — `MGET` never returns `WRONGTYPE`.

```
> MSET a 1 b 2
OK
> ZADD z 1 m
(integer) 1
> MGET a b z missing
1) "1"
2) "2"
3) (nil)
4) (nil)
```

## See also

- [keys.md](keys.md) — `DEL`, `EXISTS`, `TYPE` (any type).
- [README.md](README.md) — the scripting command families, which share this
  keyspace.
