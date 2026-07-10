# UPYTHON.SCRIPT

```
UPYTHON.SCRIPT LOAD script
UPYTHON.SCRIPT EXISTS sha1 [sha1 ...]
UPYTHON.SCRIPT FLUSH [ASYNC | SYNC]
```

Manage the **MicroPython** script cache used by
[`UPYTHON.EVAL`](UPYTHON.EVAL.md) and [`UPYTHON.EVALSHA`](UPYTHON.EVALSHA.md).
Behavior mirrors [`SCRIPT`](SCRIPT.md), on a cache that is **independent** of the
PUC-Lua ([`SCRIPT`](SCRIPT.md)), Luau ([`LUAU.SCRIPT`](LUAU.SCRIPT.md)), Wren
([`WREN.SCRIPT`](WREN.SCRIPT.md)), and Tcl ([`TCL.SCRIPT`](TCL.SCRIPT.md)) caches.

- **`UPYTHON.SCRIPT LOAD script`** — compile the script (this catches a
  `SyntaxError` **without executing it**) and cache it, returning its
  40-character SHA1 digest.
- **`UPYTHON.SCRIPT EXISTS sha1 …`** — array of `1`/`0`, one per digest.
- **`UPYTHON.SCRIPT FLUSH`** — clear the Python cache, reply `+OK`.

## Example

```
> UPYTHON.SCRIPT LOAD "reply = sum(int(x) for x in ARGV)"
"b7c9d0..."

> UPYTHON.SCRIPT EXISTS b7c9d0...
1) (integer) 1

> UPYTHON.SCRIPT LOAD "def f("
(error) ERR Error compiling script: SyntaxError: invalid syntax

> UPYTHON.SCRIPT FLUSH
OK
```

## See also

- [`UPYTHON.EVAL`](UPYTHON.EVAL.md), [`UPYTHON.EVALSHA`](UPYTHON.EVALSHA.md).
- [`SCRIPT`](SCRIPT.md) — the PUC-Lua equivalent.
