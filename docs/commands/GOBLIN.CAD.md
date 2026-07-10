# GOBLIN.CAD

```
GOBLIN.CAD key expected
```

**Compare-and-delete.** If `key` holds a string equal to `expected`, delete it and
reply `1`; otherwise reply `0`. It is the native, single-op form of the
most-copied Redis script in existence — the
[Redlock](https://redis.io/docs/latest/develop/use/patterns/distributed-locks/)
lock-release idiom:

```lua
if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("del", KEYS[1])
end
return 0
```

Running that through [`EVAL`](EVAL.md) compiles and enters a Lua interpreter to
make two `redis.call`s. `GOBLIN.CAD` does exactly the same work directly in C++ —
one hash lookup, an in-place value compare (no allocation on the common inline
path), and a delete — with no interpreter and no script cache.

## Why it exists — safe lock release

The pattern releases a distributed lock **only if you still hold it**. The lock
value is a unique token the holder wrote with `SET key token NX PX ttl`. A naive
`DEL key` could delete a lock that another client acquired after yours expired;
compare-and-delete removes the key only when the stored token is still yours, and
does it atomically, so nobody can slip in between the check and the delete.

## Return value

| Situation | Reply |
|---|---|
| `key` holds a string equal to `expected` | `(integer) 1` — the key was deleted |
| `key` holds a string ≠ `expected` | `(integer) 0` — unchanged |
| `key` does not exist | `(integer) 0` |
| `key` holds a non-string (zset / hash) | `WRONGTYPE` error |

The reply mirrors the script exactly: `1` is what `redis.call("del", …)` returns
for the one key it removed, and `0` is the script's fall-through.

## Examples

```
> SET lock:resource client-42-uuid
OK

> GOBLIN.CAD lock:resource wrong-owner
(integer) 0                       # not my token -- left alone

> GOBLIN.CAD lock:resource client-42-uuid
(integer) 1                       # my token -- released

> EXISTS lock:resource
(integer) 0
```

A typical acquire / release cycle:

```
> SET lock:job unique-token NX PX 30000
OK
# ... do the work ...
> GOBLIN.CAD lock:job unique-token
(integer) 1
```

## See also

- [`GOBLIN.CAEXPIRE`](GOBLIN.CAEXPIRE.md) — compare-and-expire (renew the lease),
  and [`GOBLIN.CAS`](GOBLIN.CAS.md) — compare-and-set (swap the value, keeping the
  TTL): the other conditional-write commands.
- The same idiom written in each embedded language: [`EVAL`](EVAL.md) (Lua),
  [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md),
  [`TCL.EVAL`](TCL.EVAL.md), [`UPYTHON.EVAL`](UPYTHON.EVAL.md),
  [`QUICKJS.EVAL`](QUICKJS.EVAL.md).
- [`SET`](strings.md#set) with `NX PX` — how the lock token is written.
- [`GETDEL`](strings.md#getdel) — unconditional get-and-delete.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
