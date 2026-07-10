# GOBLIN.CAEXPIRE

```
GOBLIN.CAEXPIRE key expected ms
```

**Compare-and-expire** (renew). If `key` holds a string equal to `expected`, set
its TTL to `ms` milliseconds from now and reply `1`; otherwise reply `0`. It is
the native, single-op form of the lock-*renewal* half of the
[Redlock](https://redis.io/docs/latest/develop/use/patterns/distributed-locks/)
pattern — the watchdog script that
[Redisson](https://github.com/redisson/redisson) runs on a timer at `lease/3` for
every lock it still holds:

```lua
if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("pexpire", KEYS[1], ARGV[2])
end
return 0
```

Running that through [`EVAL`](EVAL.md) compiles and enters a Lua interpreter to
make two `redis.call`s, on a timer, for *every* held lock. `GOBLIN.CAEXPIRE` does
the same work directly in C++ — one hash lookup, an in-place value compare (no
allocation on the common inline path), and a TTL update — with no interpreter and
no script cache. It is the renew counterpart of the release command
[`GOBLIN.CAD`](GOBLIN.CAD.md) (compare-and-delete).

## Why it exists — keeping a lock you still hold

A distributed lock is written with a short lease so a crashed holder's lock
frees itself. A live holder that needs more time must **extend the lease, but
only while it still owns the lock** — extending a lock another client now holds
would be a correctness bug. Compare-and-expire renews the TTL only when the
stored token is still yours, atomically, so no one can slip in between the check
and the `PEXPIRE`. The lock is acquired with `SET key token NX PX lease`, and a
background watchdog calls `GOBLIN.CAEXPIRE key token lease` every `lease/3` ms.

## Return value

| Situation | Reply |
|---|---|
| `key` holds a string equal to `expected` | `(integer) 1` — the TTL was (re)set to `ms` |
| `key` holds a string ≠ `expected` | `(integer) 0` — unchanged |
| `key` does not exist | `(integer) 0` |
| `key` holds a non-string (zset / hash) | `WRONGTYPE` error |

The reply mirrors the script exactly: `1` is what `redis.call("pexpire", …)`
returns for a key it renews, and `0` is the script's fall-through. `ms` must be an
integer; a non-positive `ms` deletes the key (as `PEXPIRE` does with a past
timeout) and still replies `1`.

## Examples

```
> SET lock:job client-42-uuid NX PX 30000
OK

> GOBLIN.CAEXPIRE lock:job wrong-owner 30000
(integer) 0                       # not my token -- lease untouched

> GOBLIN.CAEXPIRE lock:job client-42-uuid 30000
(integer) 1                       # my token -- lease renewed

> PTTL lock:job
(integer) 30000
```

A watchdog renewal loop (renew at a third of the lease):

```
# acquire
> SET lock:job unique-token NX PX 30000
OK
# ... every 10s while still working ...
> GOBLIN.CAEXPIRE lock:job unique-token 30000
(integer) 1
# ... release when done ...
> GOBLIN.CAD lock:job unique-token
(integer) 1
```

## See also

- [`GOBLIN.CAD`](GOBLIN.CAD.md) — compare-and-delete (release the lock), and
  [`GOBLIN.CAS`](GOBLIN.CAS.md) — compare-and-set (swap the value, keeping the
  TTL): the other conditional-write commands.
- [`SET`](strings.md#set) with `NX PX` — how the lock token and initial lease are
  written; [`PEXPIRE`](ttl.md) / [`PTTL`](ttl.md) — the TTL commands it wraps.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The same idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua),
  [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md),
  [`TCL.EVAL`](TCL.EVAL.md), [`UPYTHON.EVAL`](UPYTHON.EVAL.md),
  [`QUICKJS.EVAL`](QUICKJS.EVAL.md) — swap `del` for `pexpire`.
