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
| [`GOBLIN.CAS`](GOBLIN.CAS.md) | Compare-and-set: swap a key's value only if it still holds the expected one, keeping its TTL. |
| [`GOBLIN.TD_LEADERBOARD_RESCORE`](GOBLIN.TD_LEADERBOARD_RESCORE.md) | Time-decay leaderboard rescore: return the top k members by recency weight. |
| [`GOBLIN.INCREX`](GOBLIN.INCREX.md) | Increment a counter, arming a TTL on the first write (fixed-window rate limit). |
| [`GOBLIN.ZWINDOW`](GOBLIN.ZWINDOW.md) | Sliding-window admit/reject (also a mutex or counting semaphore). |
| [`GOBLIN.INCRBOUND`](GOBLIN.INCRBOUND.md) | Bounded increment: consume a quota up to a ceiling, else reply -1. |
| [`GOBLIN.DECRPOS`](GOBLIN.DECRPOS.md) | Decrement only while positive: reserve stock / take a permit, else -1. |
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

## GOBLIN.CAS

```
GOBLIN.CAS key expected new
```

Compare-and-set — check-and-swap in one atomic op. Overwrites `key` with `new` and
replies `OK` when it holds a string equal to `expected`, otherwise replies `0`; a
non-string key is `WRONGTYPE`. **The TTL is preserved** (`KEEPTTL`) — a bare `SET`
would clear it. See the full page: **[GOBLIN.CAS](GOBLIN.CAS.md)**.

## GOBLIN.TD_LEADERBOARD_RESCORE

```
GOBLIN.TD_LEADERBOARD_RESCORE key now half_life k mode
```

Time-decay leaderboard rescore — read a zset whose score is each member's
last-activity timestamp, recompute a recency weight (`LINEAR`, `EXP`, or `STEP`
decay), and return the top `k` by that weight, most recent first. The native form
of the whole-zset rescore idiom, ~10× faster than the same script in any embedded
interpreter. See the full page: **[GOBLIN.TD_LEADERBOARD_RESCORE](GOBLIN.TD_LEADERBOARD_RESCORE.md)**.

## GOBLIN.INCREX

```
GOBLIN.INCREX key seconds
```

Increment-with-expiry-on-first-write — the atomic native form of the fixed-window
rate-limit idiom (`INCR`; if the result is `1`, `EXPIRE` by `seconds`). Returns the
new counter. The TTL is armed only when the key is created and left ticking on
later increments, so the window is fixed from the first hit and resets when it
elapses. Redis has no single command for this and its docs recommend the Lua
version — the tell. See the full page: **[GOBLIN.INCREX](GOBLIN.INCREX.md)**.

## GOBLIN.ZWINDOW

```
GOBLIN.ZWINDOW key now window limit member
```

Sliding-window admit/reject — the native form of the sliding rate-limiter idiom
(`ZREMRANGEBYSCORE` to evict entries older than `now - window`; if fewer than
`limit` remain, `ZADD` this request and `EXPIRE` by `window`). Replies `1` if
admitted, `0` if the window is full; a non-zset key is `WRONGTYPE`. The trailing
`EXPIRE` — which per-key TTLs made possible — lets an idle window reap itself.
`limit 1` makes it a self-healing mutex, `limit N` a counting semaphore. The
sliding companion to [`GOBLIN.INCREX`](GOBLIN.INCREX.md)'s fixed window. See the
full page: **[GOBLIN.ZWINDOW](GOBLIN.ZWINDOW.md)**.

## GOBLIN.INCRBOUND

```
GOBLIN.INCRBOUND key delta max
```

Bounded increment — the native form of the capped-quota idiom (`GET`; if
`value + delta <= max`, `INCRBY delta`, else return `-1`). Applies the increment
and returns the new value when it stays within `max`, otherwise leaves the key
untouched and returns `-1`; a non-string key is `WRONGTYPE`. The bound is
inclusive, `delta` may be negative, and any TTL is preserved. For quota
consumption and inventory draw-down. See the full page:
**[GOBLIN.INCRBOUND](GOBLIN.INCRBOUND.md)**.

## GOBLIN.DECRPOS

```
GOBLIN.DECRPOS key
```

Decrement-if-positive — the native form of the stock-reservation idiom (`GET`; if
`value > 0`, `DECR`, else return `-1`). Decrements and returns the new value when
the counter is positive, otherwise leaves the key untouched — never creating it —
and returns `-1`; a non-string key is `WRONGTYPE`. The count never goes negative.
For stock reservation and semaphore-by-counter (`DECRPOS` to acquire, `INCR` to
release). See the full page: **[GOBLIN.DECRPOS](GOBLIN.DECRPOS.md)**.

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
this is an explicit trigger, not a requirement. The process-wide signal for *when*
compaction is worth running is [`INFO`](INFO.md)'s `mem_fragmentation_ratio` (an
internal-fragmentation measure — how much of `used_memory` is reclaimable dead
weight), which drops back to `1.00` after a compaction.

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

- [`INFO`](INFO.md) — process-wide `used_memory` / `mem_fragmentation_ratio`, and
  how Goblin's internal-fragmentation meaning differs from Redis's external one.
- [Scripting commands](README.md) — the embedded interpreters (`EVAL`,
  `QUICKJS.EVAL`, and the rest), which also live under prefixes.
- [String commands](strings.md), [key commands](keys.md),
  [TTL commands](ttl.md) — the Redis-compatible surface.
