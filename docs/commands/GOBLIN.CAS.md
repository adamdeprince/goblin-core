# GOBLIN.CAS

```
GOBLIN.CAS key expected new
```

**Compare-and-set.** If `key` holds a string equal to `expected`, overwrite it with
`new` and reply `OK`; otherwise reply `0`. **The existing TTL is preserved** — this
is the native, single-op form of the check-and-swap idiom, and it does the
`KEEPTTL` the safe version of the script must remember:

```lua
if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("set", KEYS[1], ARGV[2], "KEEPTTL")
end
return 0
```

Running that through [`EVAL`](EVAL.md) compiles and enters a Lua interpreter to
make two `redis.call`s. `GOBLIN.CAS` does the same work directly in C++ — one hash
lookup, an in-place value compare (no allocation on the common inline path), and a
value overwrite that keeps the key's expiry — with no interpreter and no script
cache.

## The TTL gotcha — `GOBLIN.CAS` preserves it by design

A **bare `SET` clears the key's TTL.** So the naive compare-and-set —
`if GET == expected then SET key new` — silently drops the expiry, turning a
leased value into a permanent one. That is a bug people ship constantly. The
script above avoids it with `KEEPTTL`, and `GOBLIN.CAS` does the same
**by default**: a successful swap keeps whatever TTL the key had (or none, if it
had none). There is no option to clear it — if you want a fresh value *and* a
fresh lease, set them separately.

```
> SET config v1 PX 30000
OK
> GOBLIN.CAS config v1 v2      # compare-and-set, TTL kept
OK
> PTTL config
(integer) 29997               # still ticking

> SET config v3               # a bare SET, for contrast
OK
> PTTL config
(integer) -1                  # expiry gone -- the bug GOBLIN.CAS avoids
```

## Return value

| Situation | Reply |
|---|---|
| `key` holds a string equal to `expected` | `OK` (status) — the value was swapped, TTL kept |
| `key` holds a string ≠ `expected` | `(integer) 0` — unchanged |
| `key` does not exist | `(integer) 0` — not created |
| `key` holds a non-string (zset / hash) | `WRONGTYPE` error |
| `new` cannot fit the configured value encoding | error (larger values belong in a blob store) |

The reply mirrors the script exactly: `OK` is what `redis.call("set", …)` returns
on a swap, and `0` is the script's fall-through — so a client checks *did I get
`OK`?*. `GOBLIN.CAS` never creates a missing key (the `GET` must match first).

## Examples

```
> SET counter:42 v1
OK

> GOBLIN.CAS counter:42 v0 v2
(integer) 0                   # stale expected value -- no swap

> GOBLIN.CAS counter:42 v1 v2
OK                            # matched -- swapped to v2

> GET counter:42
"v2"
```

An optimistic-concurrency update loop (read, compute, swap-if-unchanged):

```
> GET doc:1            -> "current"
# ... compute next from "current" ...
> GOBLIN.CAS doc:1 current next
OK                     # nobody else changed it; commit
# (a 0 here means someone raced us -- re-read and retry)
```

## See also

- [`GOBLIN.CAD`](GOBLIN.CAD.md) — compare-and-delete, and
  [`GOBLIN.CAEXPIRE`](GOBLIN.CAEXPIRE.md) — compare-and-expire (the lock
  release / renew counterparts).
- [`SET`](strings.md#set) with `KEEPTTL` — the option this bakes in; a bare `SET`
  clears the TTL.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The same idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua),
  [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md),
  [`TCL.EVAL`](TCL.EVAL.md), [`UPYTHON.EVAL`](UPYTHON.EVAL.md),
  [`QUICKJS.EVAL`](QUICKJS.EVAL.md) — remember the `KEEPTTL`.
