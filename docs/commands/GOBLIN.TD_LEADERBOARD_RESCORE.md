# GOBLIN.TD_LEADERBOARD_RESCORE

```
GOBLIN.TD_LEADERBOARD_RESCORE key now half_life k mode
```

**Time-decay leaderboard rescore.** A leaderboard is a sorted set whose score is
each member's *last-activity timestamp* (seconds). This command reads the whole
set, recomputes a **recency weight** for every member, and returns the **top `k`**
by that weight — most-recent first — as `[member, weight, member, weight, …]`. It
is the native form of a common idiom: keep a "hot right now" board without
constantly rewriting scores, by decaying on read instead.

The weight for a member last active at `ts` (so `age = now - ts`) depends on
`mode`:

| `mode` | weight | notes |
|---|---|---|
| `LINEAR` | `1 / (1 + age/half_life)` | hyperbolic falloff — **no transcendental** |
| `EXP` | `0.5 ^ (age/half_life)` | true half-life decay (one `pow` per member) |
| `STEP` | `1` if `ts ≥ now − half_life`, else `0` | a hard "active in the last `half_life` seconds" window |

`mode` is matched case-insensitively. The top-`k` is kept in a bounded
insertion-sorted array, so the work is **O(n·k)** with no full sort of the board,
and the pass is a single streaming read (no `ZRANGE` copy). Weights are formatted
like Lua's `tostring` (`%.14g`), so `STEP` weights print as `1` / `0`.

## The Lua it replaces

This is the whole-zset rescore scripted for real leaderboards. Run through
[`EVAL`](EVAL.md) it compiles and enters a Lua interpreter, and touches every
member through the interpreter on each call; the native command does the same work
in one C++ pass.

```lua
-- KEYS[1] = leaderboard zset (score = last-activity timestamp, seconds)
-- ARGV = now, half_life, k, mode ("LINEAR" | "EXP" | "STEP")
local now = tonumber(ARGV[1])
local hl  = tonumber(ARGV[2])
local k   = tonumber(ARGV[3])
local mode = ARGV[4]
local flat = redis.call('ZRANGE', KEYS[1], 0, -1, 'WITHSCORES')
local names, scores, n = {}, {}, 0
local function push(name, s)          -- keep the top-k sorted descending
  if n < k then
    n = n + 1; names[n], scores[n] = name, s
    local j = n
    while j > 1 and scores[j] > scores[j-1] do
      names[j], names[j-1] = names[j-1], names[j]
      scores[j], scores[j-1] = scores[j-1], scores[j]; j = j - 1
    end
  elseif s > scores[k] then
    names[k], scores[k] = name, s
    local j = k
    while j > 1 and scores[j] > scores[j-1] do
      names[j], names[j-1] = names[j-1], names[j]
      scores[j], scores[j-1] = scores[j-1], scores[j]; j = j - 1
    end
  end
end
if mode == 'LINEAR' then local inv = 1.0/hl
  for i = 1, #flat, 2 do push(flat[i], 1.0 / (1.0 + (now - tonumber(flat[i+1])) * inv)) end
elseif mode == 'EXP' then local inv = 1.0/hl
  for i = 1, #flat, 2 do push(flat[i], math.pow(0.5, (now - tonumber(flat[i+1])) * inv)) end
elseif mode == 'STEP' then local cutoff = now - hl
  for i = 1, #flat, 2 do local ts = tonumber(flat[i+1]); push(flat[i], ts >= cutoff and 1.0 or 0.0) end
else return redis.error_reply('ERR mode must be LINEAR, EXP or STEP') end
local out = {}
for i = 1, n do out[#out+1] = names[i]; out[#out+1] = tostring(scores[i]) end
return out
```

`GOBLIN.TD_LEADERBOARD_RESCORE key now half_life k mode` is a drop-in for it, with
byte-identical output (same top-k order, ties broken by `ZRANGE` order — which
matters for `STEP`, where many weights tie).

## Return value

| Situation | Reply |
|---|---|
| normal | array `[member, weight, …]`, top `k` by weight, most recent first |
| `key` does not exist | empty array |
| `k ≤ 0` | empty array |
| `mode` not `LINEAR`/`EXP`/`STEP` | `ERR mode must be LINEAR, EXP or STEP` |
| `now` / `half_life` not a number | `ERR value is not a valid float` |
| `k` not an integer | `ERR value is not an integer or out of range` |
| `key` holds a non-zset | `WRONGTYPE` error |

## Examples

```
> ZADD board 1700000000 alice 1700000600 bob 1700000900 carol
(integer) 3

> GOBLIN.TD_LEADERBOARD_RESCORE board 1700001000 300 2 EXP
1) "carol"
2) "0.79370052598410"
3) "bob"
4) "0.39685026299205"

> GOBLIN.TD_LEADERBOARD_RESCORE board 1700001000 300 3 STEP   # active in last 300s
1) "carol"
2) "1"
3) "bob"
4) "1"
5) "alice"
6) "0"
```

## Implementing it in another interpreter

The same idiom in each embedded language (0-based `KEYS`/`ARGV` except Lua/Luau).
`0.5^x` is written portably — Luau has no `math.pow`, so use the `^` operator; Jim
Tcl's minimal `expr` has **no** `pow`/`exp`, so it can do `LINEAR` and `STEP` but
**not** `EXP`.

**Luau** ([`LUAU.EVAL`](LUAU.EVAL.md)) — the Lua above, with `0.5 ^ (…)` instead
of `math.pow(0.5, …)`.

**JavaScript** ([`QUICKJS.EVAL`](QUICKJS.EVAL.md)):

```javascript
var now=parseFloat(ARGV[0]), hl=parseFloat(ARGV[1]), k=parseInt(ARGV[2]), mode=ARGV[3];
var flat=redis.call('zrange',KEYS[0],0,-1,'WITHSCORES');
var names=[], scores=[];
function push(name,s){
  var j;
  if (names.length<k){ names.push(name); scores.push(s); j=names.length-1; }
  else if (k>0 && s>scores[k-1]){ names[k-1]=name; scores[k-1]=s; j=k-1; }
  else return;
  while (j>0 && scores[j]>scores[j-1]){ var tn=names[j]; names[j]=names[j-1]; names[j-1]=tn;
    var tx=scores[j]; scores[j]=scores[j-1]; scores[j-1]=tx; j-=1; }
}
if (mode==='LINEAR'){ var inv=1/hl; for(var i=0;i<flat.length;i+=2) push(flat[i], 1/(1+(now-parseFloat(flat[i+1]))*inv)); }
else if (mode==='EXP'){ var inv=1/hl; for(var i=0;i<flat.length;i+=2) push(flat[i], Math.pow(0.5,(now-parseFloat(flat[i+1]))*inv)); }
else if (mode==='STEP'){ var cutoff=now-hl; for(var i=0;i<flat.length;i+=2){ var ts=parseFloat(flat[i+1]); push(flat[i], ts>=cutoff?1:0); } }
else return redis.error('ERR mode must be LINEAR, EXP or STEP');
var out=[]; for(var a=0;a<names.length;a++){ out.push(names[a]); out.push(String(scores[a])); } return out;
```

**MicroPython** ([`UPYTHON.EVAL`](UPYTHON.EVAL.md)) — `0.5 ** (…)`, reply via the
`reply` global:

```python
now=float(ARGV[0]); hl=float(ARGV[1]); k=int(ARGV[2]); mode=ARGV[3]
flat=redis.call('zrange',KEYS[0],0,-1,'WITHSCORES')
names=[]; scores=[]
def push(name,s):
    if len(names)<k:
        names.append(name); scores.append(s); j=len(names)-1
    elif k>0 and s>scores[k-1]:
        names[k-1]=name; scores[k-1]=s; j=k-1
    else:
        return
    while j>0 and scores[j]>scores[j-1]:
        names[j],names[j-1]=names[j-1],names[j]; scores[j],scores[j-1]=scores[j-1],scores[j]; j-=1
if mode=='LINEAR':
    inv=1.0/hl
    for i in range(0,len(flat),2): push(flat[i], 1.0/(1.0+(now-float(flat[i+1]))*inv))
elif mode=='EXP':
    inv=1.0/hl
    for i in range(0,len(flat),2): push(flat[i], 0.5**((now-float(flat[i+1]))*inv))
elif mode=='STEP':
    cutoff=now-hl
    for i in range(0,len(flat),2):
        ts=float(flat[i+1]); push(flat[i], 1.0 if ts>=cutoff else 0.0)
else:
    reply={'err':'ERR mode must be LINEAR, EXP or STEP'}
if mode in ('LINEAR','EXP','STEP'):
    out=[]
    for a in range(len(names)): out.append(names[a]); out.append(str(scores[a]))
    reply=out
```

**Wren** ([`WREN.EVAL`](WREN.EVAL.md)) — `(0.5).pow(…)`, 0-based Lists, a `Fn`
for `push`; and **Jim Tcl** ([`TCL.EVAL`](TCL.EVAL.md)) — `redis call`, lists via
`lindex`/`lset`, a `proc push` with `upvar`, `LINEAR`/`STEP` only — are in
[`benchmarks/leaderboard_rescore_benchmark.py`](../../benchmarks/leaderboard_rescore_benchmark.py),
which runs all six against the native command.

## Performance

Pipelined over a Unix socket, rescoring a 1000-member board (top 10), median
µs/op — native vs the same idiom in each interpreter (full table and methodology
in **[BENCHMARK-LANGUAGES.md](../../BENCHMARK-LANGUAGES.md)**, reproduce with
[`benchmarks/leaderboard_rescore_benchmark.py`](../../benchmarks/leaderboard_rescore_benchmark.py)):

| | LINEAR | EXP | STEP |
|---|---:|---:|---:|
| **native `GOBLIN.TD_LEADERBOARD_RESCORE`** | **`48`** | **`49`** | **`19`** |
| fastest interpreter (Luau) | `477` | `479` | `127` |
| slowest (Jim Tcl) | `5834` | n/a | `852` |

The native command is **~10× faster than the fastest interpreter and up to ~50×**
faster than the slowest, because it skips the interpreter and the `ZRANGE` copy
and streams the board in one pass. `STEP` is cheapest everywhere (a compare, no
arithmetic); `EXP`'s `pow` is not the bottleneck — per-member interpreter overhead
is. (Numbers are from a single-connection Python driver, so read the ratios.)

## See also

- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- [`ZADD`](README.md) — how the leaderboard's activity timestamps are written.
- [Scripting](README.md) — the six interpreters this idiom can also run in.
