# GOBLIN.INCRBOUND

```
GOBLIN.INCRBOUND key delta max
```

**Increment a counter, but only if it stays within a ceiling.** Read the integer
at `key` (an absent key is `0`); if `value + delta <= max`, apply the increment —
creating the key if needed, preserving any TTL — and return the new value.
Otherwise leave the key untouched and return `-1`. It is the atomic native form of
the **bounded-increment** idiom: quota consumption, inventory draw-down, any
"consume up to a cap" counter.

## The idiom it replaces

A plain `INCRBY` has no ceiling — it will happily run a quota past its limit, and
checking first with a separate `GET` is a race (two clients both read `9`, both
`INCRBY 1`, both think they were under a cap of `10`). So the correct version is a
read-compare-increment done atomically, which in Redis means **Lua** — the tell
that it wants to be a command:

```lua
local v = tonumber(redis.call("get", KEYS[1]) or "0")
if v + tonumber(ARGV[1]) <= tonumber(ARGV[2]) then
  return redis.call("incrby", KEYS[1], ARGV[1])
end
return -1
```

`GOBLIN.INCRBOUND key delta max` does exactly this in one atomic C++ op — an
integer parse, a bound check, and a conditional store. The `redis.call("get")` and
`redis.call("incrby")` become **direct calls to the store's primitives**, with no
interpreter, no bytecode, and no re-entry into the command processor.

## Semantics

- The bound is **inclusive**: landing exactly on `max` is admitted (`<= max`).
- `delta` may be **negative** (returning budget) or **zero** (a bounded no-op that
  still reports the current value while it is `<= max`).
- On **reject** the key is not modified at all — no partial increment, no key
  creation for an over-budget first request.
- On **admit** an absent key is created from `0` (so a first `delta <= max`
  succeeds and stores `delta`), exactly as `INCRBY` on a missing key would.
- Any existing **TTL is preserved** across an admitted increment, matching
  `INCRBY`/`DECR` — the counter's window keeps ticking, it is not re-armed.

## Return value

| Situation | Reply |
|---|---|
| admitted (`value + delta <= max`) | `(integer)` the new value; the store is updated |
| rejected (would exceed `max`) | `(integer) -1`; the key is left untouched |
| `key` holds a non-integer string | `ERR value is not an integer or out of range` |
| `delta` or `max` is not an integer | `ERR value is not an integer or out of range` |
| `key` holds a non-string (zset / hash) | `WRONGTYPE` error |

> The `-1` reject sentinel is the idiom's own — as in the Lua, an admitted result
> that happens to equal `-1` is indistinguishable *in the reply* from a rejection.
> They still differ in effect: an admit writes the new value, a reject writes
> nothing. Choose a `max` domain where `-1` is not a valid balance if you need to
> tell them apart from the reply alone.

## Examples

A quota — let a user consume up to 10 units, in variable-sized draws:

```
> GOBLIN.INCRBOUND quota:user42 3 10
(integer) 3          # 3 of 10 used
> GOBLIN.INCRBOUND quota:user42 4 10
(integer) 7          # 7 of 10
> GOBLIN.INCRBOUND quota:user42 4 10
(integer) -1         # would be 11 > 10 -> denied, counter stays at 7
> GOBLIN.INCRBOUND quota:user42 3 10
(integer) 10         # exactly the cap -> allowed
> GOBLIN.INCRBOUND quota:user42 -5 10
(integer) 5          # a refund frees budget
```

Inventory reservation — never oversell `max` seats:

```
> GOBLIN.INCRBOUND seats:show7 2 100   # reserve 2 of 100
(integer) 2
# ... reply < max means the reservation held; -1 means sold out for that request
```

The client's rule is simply: **`-1` means denied**, any other reply is the new
running total.

## See also

- [`GOBLIN.DECRPOS`](GOBLIN.DECRPOS.md) — decrement-if-positive, the draw-down
  counterpart (reserve a unit while stock remains).
- [`GOBLIN.INCREX`](GOBLIN.INCREX.md) — increment that arms a TTL on first write
  (fixed-window rate limit); pair with `INCRBOUND` for a capped, expiring counter.
- [`INCRBY`](../../README.md) / `DECR` — the unconditional integer ops it guards
  with a ceiling.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.INCRBOUND` replaces the `get` + `incrby` pair with
  one native op.
