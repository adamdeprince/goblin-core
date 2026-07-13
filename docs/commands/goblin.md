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
| [`GOBLIN.HCAD`](GOBLIN.HCAD.md) | Compare-and-delete a hash field: delete it only if it still holds the expected value. |
| [`GOBLIN.HSETGT`](GOBLIN.HSETGT.md) | Set-if-greater on a hash field: the `ZADD GT` that hashes lack (watermarks). |
| [`GOBLIN.CLAIM`](GOBLIN.CLAIM.md) | Idempotency guard: claim work once with an expiring lease, else return the prior result. |
| `GOBLIN.MEMORY` | Per-key memory breakdown for a zset, hash, or list. |
| `GOBLIN.OPTIMIZE` | Compact a zset, hash, or list in place. |
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

## GOBLIN.HCAD

```
GOBLIN.HCAD key field expected
```

Compare-and-delete on a hash field — the field-level form of
[`GOBLIN.CAD`](GOBLIN.CAD.md) (`HGET`; if it equals `expected`, `HDEL`). Deletes
the field and replies `1` on an exact byte match, otherwise `0`; a non-hash key is
`WRONGTYPE`. Deleting the last field drops the key. See the full page:
**[GOBLIN.HCAD](GOBLIN.HCAD.md)**.

## GOBLIN.HSETGT

```
GOBLIN.HSETGT key field value
```

Set-if-greater on a hash field — **the `ZADD GT` that hashes lack**. Reads the
field (absent counts as `-inf`) and, if `value` is strictly greater, stores it and
replies `1`; otherwise leaves it and replies `0`. A non-numeric `value` or current
value is an error; a non-hash key is `WRONGTYPE`. The raw text is stored and any
TTL is preserved. For high-water marks, monotonic versions, and last-write-wins by
timestamp. See the full page: **[GOBLIN.HSETGT](GOBLIN.HSETGT.md)**.

## GOBLIN.CLAIM

```
GOBLIN.CLAIM claim_key result_key token seconds
```

Idempotency guard — the native form of the exactly-once idiom (`SET claim_key
token NX EX seconds`; if it claimed the slot return `CLAIMED`, else `GET
result_key`). Replies `CLAIMED` when the caller won the work, the previously-stored
result when it was already done, nil when it is still in flight, or `WRONGTYPE`
when `result_key` is a non-string. The `NX` claim never `WRONGTYPE`s on `claim_key`
of any type. The expiring lease — which per-key TTLs made possible — reclaims a
crashed worker's slot automatically so the work is retried. See the full page:
**[GOBLIN.CLAIM](GOBLIN.CLAIM.md)**.

## GOBLIN.MEMORY

```
GOBLIN.MEMORY key
```

Reports where a key's memory goes — a breakdown across value storage, packed
indexes or PMA order, and structural overhead — so the true footprint is
observable rather than estimated. Works on a zset, hash, or list key; replies nil
for a missing key (or one of another type). The numbers reflect the current
layout, so they change after [`GOBLIN.OPTIMIZE`](#goblin-optimize).

Lists also report `implementation` (`pma` or `segmented`). For PMA lists,
`order_capacity` is the number of PMA slots; for segmented lists it is the leaf
count. `value_allocated_bytes` is arena capacity for PMA and requested leaf-blob
bytes for segmented lists.

Hashes also report `hash_heap_allocated_bytes` and
`keyspace_accounted_bytes`. A compact hash has no heap wrapper: its 16-byte handle
lives directly in the keyspace object slot and its packed field/value blob shares
the movable keyspace arena, so those blob bytes appear as
`keyspace_accounted_bytes`. A promoted hash reports the heap bytes occupied by its
full state and storage object instead. Process-wide retained compact-blob pool
capacity is reported by [`INFO`](INFO.md), because it cannot be assigned reliably
to one key.

For a full hash, `field_compaction_active`, `field_compaction_victim_chunk`,
`field_compaction_candidates_remaining`, `field_compaction_fields_scanned`,
`field_compaction_fields_total`, and the relocated field/byte counters expose
bounded automatic arena maintenance while it is in progress. Candidate count
falls during budgeted victim selection; field progress follows during that
chunk's evacuation. The cumulative `field_compaction_selection_nanoseconds`,
`field_compaction_densify_nanoseconds`, `field_compaction_donor_nanoseconds`, and
`field_compaction_tail_settle_nanoseconds` counters attribute compaction time for
that hash. Default donor progress is a persistent streaming first-fit scan, so
each inspected field id consumes one work unit. Mutations of already-scanned ids
use a small fixed pending queue and do not rewind the forward cursor. The opt-in
exact knapsack mode materializes the full donor set and is not latency-bounded.
`--hash-compaction-work-budget` sets the automatic per-command scan budget; the
donor copy budget remains 16 KiB.

## GOBLIN.OPTIMIZE

```
GOBLIN.OPTIMIZE key [density]
```

Compacts a zset, hash, or list **in place**. Zsets and hashes reclaim dead arena
bytes and repack their member/field index to `density`, a load factor in `(0, 1]`
(default `0.97`). PMA lists rebuild their value arena and compact their PMA using
the server's list-density setting; segmented lists merge adjacent leaves and
tighten the leaf/index vectors. The command replies with bytes reclaimed, or nil
when the key is absent. Hashes also reclaim fragmented arena chunks
automatically under heavy churn, using bounded work and byte budgets on normal
mutations; that path leaves the field index intact. `GOBLIN.OPTIMIZE` immediately
drains the same in-place arena algorithm and repacks the field index. The
process-wide signal for
*when* compaction is worthwhile is [`INFO`](INFO.md)'s
`mem_fragmentation_ratio`.

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
sets and lists. Compatible default snapshots raw-copy all-raw list values into
one final representation build; canonical fallback and RDB imports also build
once per key rather than replaying individual mutations. Already-expired keys
are dropped during native snapshot load.

## See also

- [`INFO`](INFO.md) — process-wide `used_memory` / `mem_fragmentation_ratio`, and
  how Goblin's internal-fragmentation meaning differs from Redis's external one.
- [Scripting commands](README.md) — the embedded interpreters (`EVAL`,
  `QUICKJS.EVAL`, and the rest), which also live under prefixes.
- [String commands](strings.md), [key commands](keys.md),
  [TTL commands](ttl.md) — the Redis-compatible surface.
