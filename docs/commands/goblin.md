# GOBLIN.* extension commands

Commands under the `GOBLIN.` prefix are Goblin Core's own additions — they have
no Redis equivalent. They cover memory introspection, on-demand compaction,
snapshots, and a native atomic helper. (The `GOBLIN.` scripting families —
`QUICKJS.*` and the other interpreters — are listed separately in the
[scripting index](README.md).)

| Command | Summary |
|---|---|
| [`GOBLIN.CAD`](GOBLIN.CAD.md) | Compare-and-delete: delete a key only if it still holds the expected value. |
| [`GOBLIN.CAEXPIRE`](GOBLIN.CAEXPIRE.md) | Compare-and-expire: renew a key's TTL only if it still holds the expected value. |
| `GOBLIN.MEMORY` | Per-key memory breakdown for a zset or hash. |
| `GOBLIN.OPTIMIZE` | Compact a zset or hash in place and repack its index. |
| `GOBLIN.SAVE` | Start a background point-in-time snapshot. |
| `GOBLIN.LOAD` | Load a snapshot (or a Redis `dump.rdb`) from disk. |

## GOBLIN.CAD

```
GOBLIN.CAD key expected
```

Compare-and-delete — the native form of the Redlock lock-release script. Deletes
`key` and replies `1` when it holds a string equal to `expected`, otherwise
replies `0`; a non-string key is `WRONGTYPE`. See the full page:
**[GOBLIN.CAD](GOBLIN.CAD.md)**.

## GOBLIN.CAEXPIRE

```
GOBLIN.CAEXPIRE key expected ms
```

Compare-and-expire — the native form of the Redlock/Redisson lock-*renewal*
watchdog script. Sets `key`'s TTL to `ms` from now and replies `1` when it holds a
string equal to `expected`, otherwise replies `0`; a non-string key is
`WRONGTYPE`. The renew counterpart of `GOBLIN.CAD`. See the full page:
**[GOBLIN.CAEXPIRE](GOBLIN.CAEXPIRE.md)**.

## GOBLIN.MEMORY

```
GOBLIN.MEMORY key
```

Reports where a key's memory goes — a breakdown across value storage, the packed
member/field index, and structural overhead — so the true footprint is
observable rather than estimated. Works on a zset or a hash key; replies nil for
a missing key (or one of another type). The numbers reflect the current layout,
so they change after [`GOBLIN.OPTIMIZE`](#goblin-optimize).

## GOBLIN.OPTIMIZE

```
GOBLIN.OPTIMIZE key [density]
```

Compacts a zset or hash **in place**: it reclaims dead arena bytes left by
updates and deletes and repacks the member/field index to `density`, a load
factor in `(0, 1]` (default `0.97`). Replies with the number of bytes reclaimed,
or nil when the key is absent. This moves the reindex cost out of the serving
path — the intended pattern is to bulk-load a key, run `GOBLIN.OPTIMIZE` once,
then serve reads. Sorted sets and hashes also auto-compact under heavy churn, so
this is an explicit trigger, not a requirement.

## GOBLIN.SAVE

```
GOBLIN.SAVE path [ACCEL | NOACCEL]
```

Starts a **background** point-in-time snapshot to `path` (the child process does
the writing, so the server keeps serving). `ACCEL` (the default) writes an extra
read-accelerator section for faster loads; `NOACCEL` omits it for a smaller file.
Replies `Background saving started`, or an error if a save is already in progress
or the fork fails.

## GOBLIN.LOAD

```
GOBLIN.LOAD path
```

Loads a snapshot from `path` into the store, replying with the number of keys
loaded. It also accepts a Redis `dump.rdb` (Redis 2.6–7.2.x) to migrate sorted
sets in. Already-expired keys are dropped during the load.

## See also

- [Scripting commands](README.md) — the embedded interpreters (`EVAL`,
  `QUICKJS.EVAL`, and the rest), which also live under prefixes.
- [String commands](strings.md), [key commands](keys.md),
  [TTL commands](ttl.md) — the Redis-compatible surface.
