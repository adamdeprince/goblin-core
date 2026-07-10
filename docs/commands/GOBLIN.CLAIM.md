# GOBLIN.CLAIM

```
GOBLIN.CLAIM claim_key result_key token seconds
```

**Claim a piece of work exactly once, or read the result of the run that already
did it.** Try to `SET claim_key = token NX EX seconds` — set the claim only if it
is free, with a `seconds` lease. If that **won** the claim, reply `CLAIMED` and the
caller proceeds to do the work (and later stores its outcome under `result_key`).
If the claim was **already held**, reply the value stored at `result_key` — the
result of the run that got there first (or nil if it has not finished yet). It is
the atomic native form of the **idempotency-guard** idiom.

## Why this is newly possible

The guard only works because the claim **expires**. Without a TTL, a worker that
crashes mid-run leaves the claim held forever and the work is never retried — a
permanent poison. The `EX seconds` lease means a dead claim is reclaimed
automatically after `seconds`, so the next caller re-`CLAIMED`s it and retries.
That expiring claim is the piece real per-key TTLs made possible, which is why the
idiom is worth a command:

```lua
if redis.call("set", KEYS[1], ARGV[1], "NX", "EX", ARGV[2]) then
  return "CLAIMED"
end
return redis.call("get", KEYS[2])
```

`GOBLIN.CLAIM claim_key result_key token seconds` does exactly this in one atomic
C++ op — a set-if-absent-with-expiry and a conditional get. Both `redis.call`s
become **direct calls to the store's primitives**, with no interpreter, no
bytecode, and no re-entry into the command processor.

## How it is used

The two keys play different roles: `claim_key` is the lock, `result_key` is where
the winner publishes its answer.

1. A request arrives with an idempotency id. The handler calls
   `GOBLIN.CLAIM claim:<id> result:<id> <worker-token> <lease-seconds>`.
2. **`CLAIMED`** → this caller owns the work. It runs it, then writes the outcome
   with `SET result:<id> <outcome>` (typically with its own, longer, TTL).
3. **anything else** → the work was already claimed. The reply is the stored
   outcome, so the duplicate request returns the same answer without re-running —
   or **nil**, meaning the winner is still working (the caller retries/polls).

`token` is stored verbatim in `claim_key`, so it can carry the owner's identity
(a worker id, a request id) for debugging or for a later value-checked release with
[`GOBLIN.CAD`](GOBLIN.CAD.md).

## Semantics and edge cases

- The claim uses **NX**, which only tests *existence*: a `claim_key` that already
  holds a value of **any type** simply fails the claim (no `WRONGTYPE`) and the
  command falls through to read `result_key`.
- The fallthrough read is a plain **GET**, so a `result_key` holding a non-string
  (a hash or sorted set) replies `WRONGTYPE` — exactly as the scripted `GET` would.
- Both keys are **lazily expired** before use: an elapsed claim reads as free, and
  an elapsed result reads as nil.
- `seconds` must be a **positive** integer (as `SET ... EX` requires); `0`,
  negative, or non-integer values are rejected.

## Return value

| Situation | Reply |
|---|---|
| the claim was free and is now taken | `CLAIMED` (bulk string) |
| the claim was held; `result_key` has a value | that value (bulk string) |
| the claim was held; `result_key` is absent | nil |
| the claim was held; `result_key` is a non-string | `WRONGTYPE` error |
| `seconds` is not a positive integer | `ERR value is not an integer or out of range` / `ERR invalid expire time in 'goblin.claim' command` |

## Examples

Process a payment webhook exactly once, retried safely:

```
> GOBLIN.CLAIM claim:evt88 result:evt88 worker-3 30
"CLAIMED"                 # we own it -> charge the card, then publish the result
> SET result:evt88 "charged:txn_551" EX 86400
OK

# a duplicate delivery of the same event, seconds later:
> GOBLIN.CLAIM claim:evt88 result:evt88 worker-9 30
"charged:txn_551"         # already done -> return the same answer, do not re-charge

# a duplicate that races in while worker-3 is still charging:
> GOBLIN.CLAIM claim:evt88 result:evt88 worker-9 30
(nil)                     # claimed but not finished -> caller retries shortly

# worker-3 crashes mid-charge and never publishes; 30s later the lease expires:
> GOBLIN.CLAIM claim:evt88 result:evt88 worker-9 30
"CLAIMED"                 # slot freed by the TTL -> the work is retried
```

The client's rule is: `CLAIMED` means *you* do the work; a value means it is
already done (use it); nil means it is in flight (retry).

## See also

- [`GOBLIN.CAD`](GOBLIN.CAD.md) — compare-and-delete, to release a claim early by
  its `token` instead of waiting for the lease to expire.
- [`GOBLIN.INCREX`](GOBLIN.INCREX.md) / [`GOBLIN.ZWINDOW`](GOBLIN.ZWINDOW.md) — the
  other TTL-backed guards (fixed-window and sliding-window limiters).
- [`SET`](strings.md) (`NX` / `EX`) and [`GET`](strings.md) — the two operations it
  fuses atomically.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.CLAIM` replaces the `set` + `get` pair with one
  native op.
