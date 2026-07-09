# WREN.SCRIPT

```
WREN.SCRIPT LOAD script
WREN.SCRIPT EXISTS sha1 [sha1 ...]
WREN.SCRIPT FLUSH [ASYNC | SYNC]
```

Manage the **Wren** script cache used by [`WREN.EVAL`](WREN.EVAL.md) and
[`WREN.EVALSHA`](WREN.EVALSHA.md). Behavior mirrors [`SCRIPT`](SCRIPT.md), on a
cache that is **independent** of the PUC-Lua ([`SCRIPT`](SCRIPT.md)) and Luau
([`LUAU.SCRIPT`](LUAU.SCRIPT.md)) caches.

- **`WREN.SCRIPT LOAD script`** — compile and cache the script **without running
  it**, returning its 40-character SHA1 digest. Because a Wren module has side
  effects at load time, the body is compiled inside an uncalled function, so
  `LOAD` validates syntax without executing any of your code. Invalid scripts are
  rejected.
- **`WREN.SCRIPT EXISTS sha1 …`** — array of `1`/`0`, one per digest.
- **`WREN.SCRIPT FLUSH`** — clear the Wren cache, reply `+OK`.

## Example

```
> WREN.SCRIPT LOAD "return [1, 2, 3].map{|n| n * 2}.toList"
"a1b2c3..."

> WREN.SCRIPT EXISTS a1b2c3...
1) (integer) 1

> WREN.SCRIPT FLUSH
OK
```

## See also

- [`WREN.EVAL`](WREN.EVAL.md), [`WREN.EVALSHA`](WREN.EVALSHA.md).
- [`SCRIPT`](SCRIPT.md) — the PUC-Lua equivalent.
