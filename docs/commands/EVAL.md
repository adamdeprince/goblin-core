# EVAL

```
EVAL script numkeys [key ...] [arg ...]
```

Run a **Lua 5.1** script. This is the compatibility engine: it embeds PUC-Rio Lua
5.1 (the dialect Redis scripts are written against) so scripts written for Redis
run unchanged. For the other interpreters see [`LUAU.EVAL`](LUAU.EVAL.md) and
[`WREN.EVAL`](WREN.EVAL.md).

- `script` — the Lua source.
- `numkeys` — how many of the following arguments are keys.
- The first `numkeys` arguments are exposed as the `KEYS` table; the rest as
  `ARGV`. Both are **1-based** (`KEYS[1]` is the first key).

`EVAL` compiles the script, caches it by the SHA1 of its source (so a later
[`EVALSHA`](EVALSHA.md) of the same body hits), and runs it.

## The `redis` API

Inside the script, the global `redis` table (also aliased `server`) exposes:

| Function | Purpose |
|---|---|
| `redis.call(cmd, ...)` | Run a command. A command-level error **aborts** the script and becomes the reply. |
| `redis.pcall(cmd, ...)` | Like `call`, but a command error is *returned* as a `{err=...}` table instead of aborting. |
| `redis.error_reply(s)` | Returns `{err = s}` — a value that converts to a RESP error. |
| `redis.status_reply(s)` | Returns `{ok = s}` — converts to a RESP status (`+`) reply. |
| `redis.sha1hex(s)` | The 40-character SHA1 hex digest of `s`. |
| `redis.log(level, msg, ...)` | Write to the server log. `level` is one of `redis.LOG_DEBUG/LOG_VERBOSE/LOG_NOTICE/LOG_WARNING`. |
| `redis.setresp(n)` | Accepts 2 or 3. Script-side `redis.call` conversion remains RESP2; client connections negotiate RESP3 independently with `HELLO 3`. |
| `redis.replicate_commands()` | Returns `true`; compatibility no-op because Goblin automatically captures successful nested writes. |
| `redis.set_repl(x)` | No-op; `redis.REPL_NONE/REPL_AOF/REPL_SLAVE/REPL_REPLICA/REPL_ALL` are defined. |
| `redis.breakpoint()`, `redis.debug()` | No-ops, return `false`. |

The standard libraries `cjson`, `cmsgpack`, `struct`, and `bit` are available as
globals. (These are Lua-5.1 C libraries and are specific to `EVAL`; the Luau and
Wren engines ship their own, different standard libraries.)

## Return value: Lua → RESP

| Lua value | RESP reply |
|---|---|
| `number` | integer, **truncated toward zero** (`3.9` → `:3`) |
| `string` | bulk string |
| `nil` | null bulk (`$-1`) |
| `false` | null bulk (`$-1`) |
| `true` | integer `1` |
| table (sequence) | multi-bulk array, **stopping at the first `nil`** |
| table with a string `err` field | error reply (`-<err>`) |
| table with a string `ok` field | status reply (`+<ok>`) |
| a script that `return`s nothing | null bulk |

## Reply value: RESP → Lua

What `redis.call` / `redis.pcall` hand back:

| RESP reply | Lua value |
|---|---|
| integer | number |
| bulk string | string |
| null bulk / null array | `false` |
| array | table (1-based) |
| status (`+OK`) | `{ok = "OK"}` |
| error (`-ERR ...`) | `{err = "ERR ..."}` — and `redis.call` re-raises it |

## Examples

```
> EVAL "return 1" 0
(integer) 1

> EVAL "return 'hello'" 0
"hello"

> EVAL "return {1, 2, 3}" 0
1) (integer) 1
2) (integer) 2
3) (integer) 3

> EVAL "return {KEYS[1], ARGV[1]}" 1 k1 v1
1) "k1"
2) "v1"

> EVAL "return #KEYS" 2 a b
(integer) 2
```

Reading and writing through `redis.call` (the write is visible to later
commands, because there is one shared store):

```
> EVAL "return redis.call('zadd', KEYS[1], 1, 'a', 2, 'b')" 1 board
(integer) 2

> EVAL "return redis.call('zrange', KEYS[1], 0, -1, 'WITHSCORES')" 1 board
1) "a"
2) "1"
3) "b"
4) "2"

> ZSCORE board a
"1"
```

Shaping the reply, and using the bundled libraries:

```
> EVAL "return redis.status_reply('DONE')" 0
DONE

> EVAL "return redis.error_reply('nope')" 0
(error) nope

> EVAL "return redis.sha1hex('')" 0
"da39a3ee5e6b4b0d3255bfef95601890afd80709"

> EVAL "return cjson.encode({1, 2, 3})" 0
"[1,2,3]"

> EVAL "return bit.band(6, 3)" 0
(integer) 2
```

`redis.pcall` lets a script recover from a command error instead of aborting:

```
> EVAL "local r = redis.pcall('zscore'); if r.err then return 'caught' end" 0
"caught"
```

## Compare-and-delete — the Redlock unlock idiom

The single most-copied Redis script is the safe lock release: delete a key only
if it still holds the token you wrote (an unconditional `DEL` could drop a lock a
later client now holds).

```lua
if redis.call("get", KEYS[1]) == ARGV[1] then
  return redis.call("del", KEYS[1])
end
return 0
```

```
> SET lock:job my-token
OK
> EVAL "if redis.call('get', KEYS[1]) == ARGV[1] then return redis.call('del', KEYS[1]) end return 0" 1 lock:job my-token
(integer) 1
```

Goblin Core also ships this as a native, single-op command — no interpreter, no
script cache: [`GOBLIN.CAD key expected`](GOBLIN.CAD.md).

## Real-time leaderboard rescoring

A heavier, stateful example: a leaderboard whose stored score is each member's
last-activity timestamp, *rescored on read* by recency —
`decay = 1 / (1 + age / half_life)`, deliberately no transcendentals — returning
the top `k` most-recent members. The top-k is kept in a bounded insertion-sorted
array, so the work is O(n·k), not a full sort of the board. `ARGV` is
`now, half_life, k`; the reply is `[member, round(decay·1e6), …]`, most recent first.

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

## Sandbox

The interpreter opens only `base`, `table`, `string`, `math`, `debug`, and a
restricted `os` (time/clock/date only). There is no `io`, no `package`/`require`,
and `dofile`, `loadfile`, `load`, `loadstring`, and `print` are removed. Creating
a global variable raises an error (use `local`):

```
> EVAL "x = 1" 0
(error) ERR ... Script attempted to create global variable 'x'
```

## See also

- [`EVALSHA`](EVALSHA.md) — run a cached script by digest.
- [`SCRIPT`](SCRIPT.md) — manage the script cache.
- [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md) — the other interpreters.
