# GOBLIN.HSETGT

```
GOBLIN.HSETGT key field value
```

**Set a hash field only if the new value is greater than the current one.** Read
the number in `field` (an absent field counts as `-inf`); if `value` is strictly
greater, store it and return `1`. Otherwise leave the field unchanged and return
`0`. It is the atomic native form of the **set-if-greater** idiom: high-water
marks, monotonic version numbers, last-write-wins by timestamp.

## Why this command exists

Sorted sets already have this — `ZADD key GT score member` updates a member's
score only when the new score is higher. **Hash fields have no equivalent.** That
asymmetry is exactly why the idiom is written as a script: to get GT semantics on a
hash field you must read, compare, and conditionally write, and doing that
atomically in Redis means **Lua** — the tell that it wants to be a command:

```lua
local cur = tonumber(redis.call("hget", KEYS[1], ARGV[1]) or "-inf")
if tonumber(ARGV[2]) > cur then
  redis.call("hset", KEYS[1], ARGV[1], ARGV[2])
  return 1
end
return 0
```

`GOBLIN.HSETGT key field value` does exactly this in one atomic C++ op — an
`HGET`, a numeric compare against `-inf` for the absent case, and a conditional
`HSET`. The `redis.call`s become **direct calls to the store's primitives**, with
no interpreter, no bytecode, and no re-entry into the command processor.

## Semantics

- The comparison is **strict** (`>`): an equal value does not update and returns
  `0`, so repeated writes of the same watermark are idempotent no-ops.
- An **absent field** (or absent key) compares as `-inf`, so the first write of any
  finite `value` always wins and creates the field (and the key if needed).
- The **raw `value` bytes are stored**, not a reparsed/canonical form — `HSETGT h f
  9.50` leaves `HGET h f` as `9.50`, exactly as the `HSET` in the script would.
- Values are compared as doubles, so integers, decimals, and negatives all work
  (e.g. Unix timestamps, monotonically increasing versions).
- Any existing **TTL on the key is preserved** across an update (`HSET` does not
  touch it).

## Return value

| Situation | Reply |
|---|---|
| `value` strictly greater than the current (or field absent) | `(integer) 1`; the field is updated |
| `value` not greater than the current | `(integer) 0`; the field is unchanged |
| `value` is not a valid number | `ERR value is not a valid float` |
| the current field value is present but not a number | `ERR hash value is not a float` |
| `key` holds a non-hash (string / zset) | `WRONGTYPE` error |

Only finite numbers are accepted for `value` (`inf`/`nan` are rejected); the
`-inf` for an absent field is an internal sentinel, never a value you store.

## Examples

A monotonic high-water mark — keep the latest sequence number seen per stream:

```
> GOBLIN.HSETGT offsets stream:7 100
(integer) 1          # first write -> stored
> GOBLIN.HSETGT offsets stream:7 98
(integer) 0          # a late/duplicate lower offset is ignored
> GOBLIN.HSETGT offsets stream:7 100
(integer) 0          # equal -> no-op (strictly greater required)
> GOBLIN.HSETGT offsets stream:7 137
(integer) 1          # advances the watermark
> HGET offsets stream:7
"137"
```

Last-write-wins by timestamp — store the newest observed value's time per key,
racing writers converging on the maximum:

```
> GOBLIN.HSETGT seen sensor:3 1720000000
(integer) 1
> GOBLIN.HSETGT seen sensor:3 1719999500   # an older reading arrives late
(integer) 0                                # rejected; the newer time stands
```

The client's rule is simply: `1` means your value was the new maximum, `0` means a
greater-or-equal one was already there.

## See also

- `ZADD key GT score member` — the sorted-set operation this brings to hash fields.
- [`GOBLIN.HCAD`](GOBLIN.HCAD.md) — compare-and-delete on a hash field, the
  conditional-delete companion.
- [`GOBLIN.CAS`](GOBLIN.CAS.md) — compare-and-set on a whole string key (equality,
  not ordering).
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.HSETGT` replaces the `hget` + `hset` pair with one
  native op.
