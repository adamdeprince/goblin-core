# LUAU.EVAL

```
LUAU.EVAL script numkeys [key ...] [arg ...]
```

Run a script on **[Luau](https://luau.org)** — Roblox's typed, sandboxed Lua
dialect. This is a *different interpreter* from [`EVAL`](EVAL.md): `EVAL` stays on
PUC-Lua 5.1 for bug-for-bug Redis compatibility, and `LUAU.EVAL` opts into Luau.
The two share the key space but nothing else — separate VMs, separate
[script caches](LUAU.SCRIPT.md), separate standard libraries.

Argument handling is the same as `EVAL`: `numkeys` splits keys from args, exposed
as the **1-based** `KEYS` and `ARGV` tables.

## Same `redis` API, same conversions

The host binding is identical to [`EVAL`](EVAL.md): `redis.call`, `redis.pcall`,
`redis.error_reply`, `redis.status_reply`, `redis.sha1hex`, `redis.log`,
`redis.setresp`, `redis.replicate_commands`, `redis.set_repl`, the `REPL_*` /
`LOG_*` constants, and the `server` alias. The Lua↔RESP conversion tables are the
same as [`EVAL`](EVAL.md#return-value-lua--resp).

```
> LUAU.EVAL "return 1" 0
(integer) 1

> LUAU.EVAL "return {KEYS[1], ARGV[1]}" 1 k1 v1
1) "k1"
2) "v1"

> LUAU.EVAL "return redis.call('zadd', KEYS[1], 5, 'x')" 1 board
(integer) 1
```

## What is different from EVAL

**Language dialect.** Luau is a superset of Lua with type annotations, `continue`,
string interpolation (`` `value = {x}` ``), and more. These parse under
`LUAU.EVAL` but are a syntax error under `EVAL`:

```
> LUAU.EVAL "local x: number = 40 + 2 return x" 0
(integer) 42

> EVAL "local x: number = 40 + 2 return x" 0
(error) ERR Error compiling script ...     # PUC-Lua 5.1 has no type annotations
```

**Standard library.** Luau ships its own libraries — `bit32`, `buffer`, `utf8`,
`vector`, plus the usual `string`/`table`/`math`/`os`. It does **not** have the
`bit`, `cjson`, `cmsgpack`, or `struct` libraries that `EVAL` bundles:

```
> LUAU.EVAL "return bit32.band(6, 3)" 0
(integer) 2

> LUAU.EVAL "return bit.band(6, 3)" 0
(error) ...                                 # no `bit` in Luau — use bit32
```

**Isolation.** Luau's sandbox is stronger than the PUC engine's. The base globals
are frozen (`luaL_sandbox`), and every script runs on its own sandboxed thread
(`luaL_sandboxthread`), so one script can never observe or corrupt another's
globals. Unlike `EVAL`, a script *may* create globals — but only on its own
throw-away thread, so they never leak.

**Wire protocol.** Client connections may negotiate RESP3 with `HELLO 3`.
`redis.setresp(3)` is accepted inside Luau for compatibility, but script-side
`redis.call` conversion remains RESP2.

## Sandbox

Luau's standard library is sandbox-safe by construction: there is no `io`, no
`package`/`require`, and `os` is limited to `time`/`clock`/`date`. Host modules
cannot be loaded.

## Compare-and-delete — the Redlock unlock idiom

The most-copied Redis script — safe lock release, deleting a key only if it still
holds the token you wrote — is byte-for-byte identical Luau and Lua source
(`KEYS`/`ARGV` are 1-based in both):

```lua
if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("del", KEYS[1])
end
return 0
```

```
> SET lock:job my-token
OK
> LUAU.EVAL "if redis.call('get', KEYS[1]) == ARGV[1] then return redis.call('del', KEYS[1]) end return 0" 1 lock:job my-token
(integer) 1
```

Goblin Core also ships this as a native, single-op command — no interpreter:
[`GOBLIN.CAD key expected`](GOBLIN.CAD.md).

## Real-time leaderboard rescoring

A heavier example, identical Luau and Lua source: a leaderboard whose stored
score is each member's last-activity timestamp, *rescored on read* by recency —
`decay = 1 / (1 + age / half_life)`, no transcendentals — returning the top `k`
most-recent members. The top-k is kept in a bounded insertion-sorted array
(O(n·k), not a full sort). `ARGV` is `now, half_life, k`; the reply is
`[member, round(decay·1e6), …]`, most recent first.

```lua
-- KEYS[1] = leaderboard (score = last-activity unix ts); ARGV = now, half_life, k
local now, hl, k = tonumber(ARGV[1]), tonumber(ARGV[2]), tonumber(ARGV[3])
local flat = redis.call('ZRANGE', KEYS[1], 0, -1, 'WITHSCORES')
local best, bestn = {}, 0
for i = 1, #flat, 2 do
  local m = flat[i]
  local ts = tonumber(flat[i + 1])
  local d = 1.0 / (1.0 + (now - ts) / hl)
  if bestn < k or d > best[bestn].s then       -- bounded top-k, no full sort
    local pos = (bestn < k) and bestn + 1 or bestn
    while pos > 1 and best[pos - 1].s < d do
      best[pos] = best[pos - 1]
      pos = pos - 1
    end
    best[pos] = {m = m, s = d}
    if bestn < k then bestn = bestn + 1 end
  end
end
local result = {}
for i = 1, bestn do
  result[#result + 1] = best[i].m
  result[#result + 1] = math.floor(best[i].s * 1000000 + 0.5)
end
return result
```

## See also

- [`LUAU.EVALSHA`](LUAU.EVALSHA.md) — run a cached Luau script by digest.
- [`LUAU.SCRIPT`](LUAU.SCRIPT.md) — manage the Luau script cache.
- [`EVAL`](EVAL.md) — the compatibility (PUC-Lua) engine and full conversion tables.
- [`WREN.EVAL`](WREN.EVAL.md) — the Wren interpreter.
