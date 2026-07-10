# QUICKJS.SCRIPT

```
QUICKJS.SCRIPT LOAD script
QUICKJS.SCRIPT EXISTS sha1 [sha1 ...]
QUICKJS.SCRIPT FLUSH [ASYNC | SYNC]
```

Manage the **QuickJS** (JavaScript) script cache used by
[`QUICKJS.EVAL`](QUICKJS.EVAL.md) and [`QUICKJS.EVALSHA`](QUICKJS.EVALSHA.md).
Behavior mirrors [`SCRIPT`](SCRIPT.md), on a cache that is **independent** of the
PUC-Lua ([`SCRIPT`](SCRIPT.md)), Luau ([`LUAU.SCRIPT`](LUAU.SCRIPT.md)), Wren
([`WREN.SCRIPT`](WREN.SCRIPT.md)), Tcl ([`TCL.SCRIPT`](TCL.SCRIPT.md)), and Python
([`UPYTHON.SCRIPT`](UPYTHON.SCRIPT.md)) caches.

- **`QUICKJS.SCRIPT LOAD script`** — compile the script (this catches a syntax
  error **without executing it**) and cache it, returning its 40-character SHA1
  digest.
- **`QUICKJS.SCRIPT EXISTS sha1 …`** — array of `1`/`0`, one per digest.
- **`QUICKJS.SCRIPT FLUSH`** — clear the JavaScript cache, reply `+OK`.

## Example

```
> QUICKJS.SCRIPT LOAD "return ARGV.reduce((a, b) => a + parseInt(b), 0)"
"a1b2c3..."

> QUICKJS.SCRIPT EXISTS a1b2c3...
1) (integer) 1

> QUICKJS.SCRIPT LOAD "return 1 +"
(error) ERR Error compiling script: unexpected token in expression: '}'

> QUICKJS.SCRIPT FLUSH
OK
```

## See also

- [`QUICKJS.EVAL`](QUICKJS.EVAL.md), [`QUICKJS.EVALSHA`](QUICKJS.EVALSHA.md).
- [`SCRIPT`](SCRIPT.md) — the PUC-Lua equivalent.
