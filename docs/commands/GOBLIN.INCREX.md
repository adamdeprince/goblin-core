# GOBLIN.INCREX

```
GOBLIN.INCREX key seconds
```

**Increment, and set an expiry on the first write.** Increment the integer at
`key` by one; if the result is `1` — meaning the key was just created — also arm a
`seconds` TTL on it. Return the new value. It is the atomic native form of the
**fixed-window rate limiter**, the single most common counter idiom.

Redis has no one command for this — `INCR` and `EXPIRE` are separate, and doing
them non-atomically is a race (two clients can both `INCR` to a fresh key and only
one arms the TTL, or a crash between the two leaves an immortal counter). So
**Redis's own documentation recommends the Lua version** — which is the tell that
it wants to be a command:

```lua
local current = redis.call("incr", KEYS[1])
if tonumber(current) == 1 then
  redis.call("expire", KEYS[1], ARGV[1])
end
return current
```

`GOBLIN.INCREX key seconds` does exactly this in one atomic C++ op — an integer
parse, an increment, and a conditional TTL set — with no interpreter and no
script.

## Why the "on first write" matters — a fixed window

The TTL is armed **only when the counter is created**, and left alone on every
later increment (the running counter's window keeps ticking down — it is *not*
reset each hit). So the window is **fixed from the first request**, not sliding:

- The 1st request in a window creates the key and starts the `seconds` clock.
- Requests 2..N increment the same key; the TTL keeps counting down.
- When the TTL elapses the key is deleted; the next request recreates it at `1`
  and starts a fresh window.

That is precisely the fixed-window rate-limit shape: "at most N per `seconds`,
counted from the first hit." (Arming the TTL on *every* write would instead give a
sliding window that never resets under sustained traffic — usually a bug.)

## Return value

| Situation | Reply |
|---|---|
| key created by this call | `(integer) 1`, and a `seconds` TTL is armed |
| key already a counter | `(integer)` the incremented value; the existing TTL is left ticking |
| `key` holds a non-integer string | `ERR value is not an integer or out of range` |
| `seconds` is not an integer | `ERR value is not an integer or out of range` |
| `key` holds a non-string (zset / hash) | `WRONGTYPE` error |

A non-positive `seconds` expires the key immediately (as `EXPIRE` does with a past
timeout); use a positive window.

## Examples

A rate limiter — allow 5 requests per 60-second window per user:

```
> GOBLIN.INCREX rate:user42 60
(integer) 1          # first hit; 60s window armed
> GOBLIN.INCREX rate:user42 60
(integer) 2          # TTL still counting down from the first hit
> TTL rate:user42
(integer) 58
...
> GOBLIN.INCREX rate:user42 60
(integer) 6          # over the limit of 5 -> reject the request
# 60s after the first hit the key expires; the next call returns 1, new window
```

The client's rule is simply: reject when the reply exceeds the limit.

## See also

- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family,
  including the conditional-write trio [`GOBLIN.CAD`](GOBLIN.CAD.md) /
  [`GOBLIN.CAEXPIRE`](GOBLIN.CAEXPIRE.md) / [`GOBLIN.CAS`](GOBLIN.CAS.md).
- [`INCR`](strings.md) and [`EXPIRE`](ttl.md) — the two commands it fuses
  atomically; [`TTL`](ttl.md) to read the remaining window.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.INCREX` replaces the two `redis.call`s with one.
