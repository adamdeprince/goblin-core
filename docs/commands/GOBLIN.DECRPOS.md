# GOBLIN.DECRPOS

```
GOBLIN.DECRPOS key
```

**Decrement a counter, but only while it is positive.** Read the integer at `key`
(an absent key is `0`); if it is `> 0`, decrement it by one — preserving any TTL —
and return the new value. Otherwise leave the key untouched (never creating it) and
return `-1`. It is the atomic native form of the **decrement-if-positive** idiom:
stock reservation, a permit draw from a counting semaphore, any "take one while
some remain" counter.

## The idiom it replaces

A plain `DECR` has no floor — it runs straight past zero into negative numbers, so
a stock counter would happily "reserve" units that do not exist. Guarding it with a
separate `GET` is a race (two clients both read `1`, both `DECR`, and the count
goes to `-1` — two reservations against one unit). The correct version is a
read-test-decrement done atomically, which in Redis means **Lua** — the tell that
it wants to be a command:

```lua
local v = tonumber(redis.call("get", KEYS[1]) or "0")
if v > 0 then return redis.call("decr", KEYS[1]) end
return -1
```

`GOBLIN.DECRPOS key` does exactly this in one atomic C++ op — an integer parse, a
positivity test, and a conditional store. The `redis.call("get")` and
`redis.call("decr")` become **direct calls to the store's primitives**, with no
interpreter and no command re-entry.

## Semantics

- The floor is at **zero**: a counter of `1` decrements to `0` and returns `0`; the
  next call rejects. The count never goes negative through this command.
- A key that is **absent, zero, or already negative** is `<= 0`, so it is rejected
  with `-1` and **not created or modified** — a missing stock key stays missing.
- Any existing **TTL is preserved** across a successful decrement (matching `DECR`).

## Return value

| Situation | Reply |
|---|---|
| decremented (value was `> 0`) | `(integer)` the new value (`>= 0`) |
| rejected (value `<= 0`, or key absent) | `(integer) -1`; the key is left untouched |
| `key` holds a non-integer string | `ERR value is not an integer or out of range` |
| `key` holds a non-string (zset / hash) | `WRONGTYPE` error |

Because the floor is zero, a successful reply is always `>= 0` and a reject is
always `-1`, so — unlike [`GOBLIN.INCRBOUND`](GOBLIN.INCRBOUND.md) — the reply
alone distinguishes the two cases unambiguously.

## Examples

Stock reservation — hand out units until they run out:

```
> SET stock:item9 3
OK
> GOBLIN.DECRPOS stock:item9
(integer) 2          # reserved one, 2 left
> GOBLIN.DECRPOS stock:item9
(integer) 1
> GOBLIN.DECRPOS stock:item9
(integer) 0          # last one reserved
> GOBLIN.DECRPOS stock:item9
(integer) -1         # sold out -> reservation denied, count stays 0
```

A counting semaphore — `N` permits, `DECRPOS` to acquire, `INCR` to release:

```
> SET sem:pool 5      # 5 permits
OK
> GOBLIN.DECRPOS sem:pool
(integer) 4          # acquired a permit
# ... work ...
> INCR sem:pool       # release it back
(integer) 5
# when DECRPOS returns -1, all permits are out -> caller must wait or back off
```

The client's rule is simply: **`-1` means none left** (denied); any other reply is
the number remaining after taking one.

## See also

- [`GOBLIN.INCRBOUND`](GOBLIN.INCRBOUND.md) — bounded increment, the capped-growth
  counterpart (consume a quota up to a ceiling).
- [`GOBLIN.ZWINDOW`](GOBLIN.ZWINDOW.md) — a semaphore/mutex with self-healing
  leases when holders can crash; `DECRPOS`/`INCR` is the simpler pair when releases
  are reliable.
- [`DECR`](../../README.md) / `INCR` — the unconditional integer ops it guards with
  a floor.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.DECRPOS` replaces the `get` + `decr` pair with one
  native op.
