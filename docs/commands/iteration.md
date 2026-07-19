# Bounded iteration

Goblin Core implements the complete Redis cursor family for walking the
keyspace and collection members without materializing an entire object in one
reply:

```text
SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]
HSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]
SSCAN key cursor [MATCH pattern] [COUNT count]
ZSCAN key cursor [MATCH pattern] [COUNT count]
```

Use these commands for operational inspection, migration, debugging, and other
workflows where `HGETALL`, `SMEMBERS`, or a whole sorted-set range could create
an unbounded pause or reply.

## Cursor contract

Start a traversal with cursor `0`. Every response is a two-element array: the
next cursor as a bulk string, followed by the page of results. Continue using
the returned cursor until it is `0` again.

The cursor is opaque. Do not derive a position from it, compare it with a cursor
from another server, or persist it across Goblin Core versions. `COUNT` must be
positive and defaults to 10. It is a work hint, not a promised page length:

- `MATCH` and `TYPE` consume visited work before filtering, so a nonzero cursor
  may arrive with an empty page.
- Compact representations may return a different number of elements than
  requested.
- A complete traversal over an unchanged keyspace or collection visits every
  element. Mutation during traversal may duplicate or omit elements.

Goblin bounds the number of storage positions visited by each call. It does not
build the complete key or collection list before slicing the reply.

## SCAN

```text
SCAN cursor [MATCH pattern] [COUNT count] [TYPE type]
```

`SCAN` visits the unified keyspace. `MATCH` applies Redis glob syntax to key
bytes. `TYPE` is case-insensitive and accepts `string`, `zset`, `hash`, `list`,
`set`, or `array`; an unknown type matches nothing. Expired keys are omitted,
although an expired slot may still consume the call's work budget.

The result page contains keys:

```text
1) "17"
2) 1) "session:42"
   2) "session:81"
```

## HSCAN

```text
HSCAN key cursor [MATCH pattern] [COUNT count] [NOVALUES]
```

Without `NOVALUES`, the result page alternates field and value strings. With
`NOVALUES`, it contains fields only. `MATCH` applies to fields, not values.
Missing keys return a completed empty traversal; a non-hash key returns
`WRONGTYPE`.

The same command is available as `GOBLIN.RT.HSCAN` and
`GOBLIN.EFFICENT.HSCAN`. As a read, each qualified form scans the representation
already stored at the key; the implementation prefix matters when a hash-creating
write first creates that key.

## SSCAN

```text
SSCAN key cursor [MATCH pattern] [COUNT count]
```

The result page contains set members. `MATCH` applies to member bytes. Missing
keys return a completed empty traversal; a non-set key returns `WRONGTYPE`.

## ZSCAN

```text
ZSCAN key cursor [MATCH pattern] [COUNT count]
```

The result page alternates member and score strings under RESP. `MATCH` applies
to members. Typed SBE carries each score as a native `double`. Missing keys
return a completed empty traversal; a non-sorted-set key returns `WRONGTYPE`.
See the [sorted-set reference](sorted-sets.md#incremental-scan) for storage and
score details.

## SBE

Typed SBE exposes `Scan` and `HScan` requests plus the existing `SScan` and
`ZScan` requests. `SCAN` and `HSCAN` return `StringScanReply`; `ZSCAN` returns
`ScoredScanReply`. The typed C++ client exposes all four operations: `scan()`,
`hscan()`, and `zscan()` return cursor-bearing result structures, while the
existing `sscan()` returns a flat cursor/member array.
