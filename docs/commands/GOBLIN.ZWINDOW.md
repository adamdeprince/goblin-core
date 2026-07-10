# GOBLIN.ZWINDOW

```
GOBLIN.ZWINDOW key now window limit member
```

**Admit a request into a sliding window, or reject it.** `key` holds a sorted set
whose members are recent requests and whose scores are the timestamps they
arrived at. In one atomic op `GOBLIN.ZWINDOW`:

1. **Evicts** every member older than `now - window` (slides the window forward),
2. checks whether fewer than `limit` members remain,
3. and if so **admits** the request — adds `member` scored `now` — and **re-arms
   the key's TTL to `window`**, so an idle key reaps itself.

It replies `1` when the request is admitted and `0` when the window is already
full. It is the native form of the sliding-window rate-limiter idiom.

## The idiom it replaces

Redis has no single command for this, so **its own documentation reaches for
Lua** — the tell that it wants to be a command:

```lua
local now = tonumber(ARGV[1])
redis.call('zremrangebyscore', KEYS[1], 0, now - tonumber(ARGV[2]))
if redis.call('zcard', KEYS[1]) < tonumber(ARGV[3]) then
  redis.call('zadd', KEYS[1], now, ARGV[4])
  redis.call('expire', KEYS[1], ARGV[2])
  return 1
end
return 0
```

`GOBLIN.ZWINDOW key now window limit member` does exactly this — a
[`ZREMRANGEBYSCORE`](ZREMRANGEBYSCORE.md), a `ZCARD`, and a conditional `ZADD` +
`EXPIRE` — in one C++ op, calling the store's primitives directly with no
interpreter, no bytecode, and no script cache. The removal is an O(log n + evicted)
seek on the score index, not a scan.

### The trailing EXPIRE is the point

The last step — re-arming the TTL on every admit — is the piece that was
impossible before Goblin Core had real per-key TTLs. Without it the sorted set
lives forever: a client that makes a burst and then goes quiet leaves its window
key resident indefinitely, and a busy keyspace slowly fills with the cold windows
of departed clients. The `EXPIRE window` means **an idle window deletes itself**
one `window` after its last request, so the keyspace self-cleans. That trailing
reap is why this is now one command instead of a script plus a separate sweeper.

## Sliding vs. fixed window

This is the *sliding* companion to [`GOBLIN.INCREX`](GOBLIN.INCREX.md)'s *fixed*
window. `GOBLIN.INCREX` counts hits in a window pinned to the first request and
resets in one step when it elapses; `GOBLIN.ZWINDOW` keeps the exact timestamps of
the last `limit` requests and lets the window glide continuously, so it enforces
"at most `limit` in **any** `window`-long span" rather than "per fixed bucket." The
sliding form has no burst at a bucket boundary, at the cost of storing one member
per in-window request instead of a single counter.

## Capacity 1 is a mutex; capacity N is a counting semaphore

The same command is three primitives depending on `limit`:

| `limit` | What it is | Reading |
|---|---|---|
| `1` | a **mutex** with a lease | `1` = you hold the lock; `0` = someone else does |
| `N` | a **counting semaphore** with `N` permits | `1` = you took a permit; `0` = all `N` are out |
| large | a **rate limiter** | `1` = under the rate; `0` = throttled |

Because the lock/permit is just a member with a TTL-backed window, it **self-heals
on client death**: a holder that crashes without releasing simply stops
refreshing, and its member ages out of the window after `window`, freeing the
slot. There is no separate unlock — the lease expires. (For an explicit,
value-checked release, pair it with [`GOBLIN.CAD`](GOBLIN.CAD.md).)

## Arguments

| Argument | Meaning |
|---|---|
| `key` | the sorted set holding the window |
| `now` | current timestamp (any monotonic unit; scores and `window` must share it) |
| `window` | window length in the same unit as `now`; also the TTL armed on admit |
| `limit` | maximum concurrent members allowed in the window |
| `member` | the identity to record — a request id, client id, or lock owner |

`now` and `window` are parsed as doubles (fractional timestamps are fine for the
window math); the TTL is armed as `⌊window⌋` seconds, matching the Lua `EXPIRE`.
Pick a unit for `now`/`window` such that `window` is a count of **seconds** if you
want the TTL to line up with the window exactly (the common case).

## Return value

| Situation | Reply |
|---|---|
| admitted (window had room) | `(integer) 1`; `member` recorded and the TTL re-armed to `window` |
| rejected (window already full) | `(integer) 0`; the set is left unchanged |
| `now` or `window` not a number | `ERR value is not a valid float` |
| `limit` not an integer | `ERR value is not an integer or out of range` |
| `key` holds a non-zset (string / hash) | `WRONGTYPE` error |

Re-using the same `member` inside the window updates its timestamp (a `ZADD`
score update) rather than adding a second entry — so a client refreshing its own
lease keeps one slot, which is what a mutex/semaphore holder wants.

## Examples

A sliding limiter — at most 3 requests per 10-second window per user:

```
> GOBLIN.ZWINDOW rl:user42 1000 10 3 req-a
(integer) 1          # 1st in the window
> GOBLIN.ZWINDOW rl:user42 1001 10 3 req-b
(integer) 1          # 2nd
> GOBLIN.ZWINDOW rl:user42 1002 10 3 req-c
(integer) 1          # 3rd — window now full
> GOBLIN.ZWINDOW rl:user42 1003 10 3 req-d
(integer) 0          # over the limit -> reject
> GOBLIN.ZWINDOW rl:user42 1015 10 3 req-e
(integer) 1          # req-a/b/c aged out (now-10=1005) -> room again
> TTL rl:user42
(integer) 10         # re-armed on the last admit; reaps itself if traffic stops
```

A mutex (`limit 1`) with a 30-second lease:

```
> GOBLIN.ZWINDOW lock:job7 5000 30 1 worker-1
(integer) 1          # worker-1 holds the lock
> GOBLIN.ZWINDOW lock:job7 5001 30 1 worker-2
(integer) 0          # contended — worker-1 still holds it
# worker-1 refreshes by calling again with its own id before 30s elapse;
# if worker-1 dies, the lease ages out and worker-2's next call returns 1
```

## See also

- [`GOBLIN.INCREX`](GOBLIN.INCREX.md) — the fixed-window counterpart (one counter
  instead of per-request members).
- [`ZREMRANGEBYSCORE`](ZREMRANGEBYSCORE.md) — the eviction primitive it builds on,
  usable on its own for score-range trimming.
- [`GOBLIN.CAD`](GOBLIN.CAD.md) — value-checked release, to pair with the mutex
  form for an explicit unlock.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.ZWINDOW` replaces the four `redis.call`s with one.
