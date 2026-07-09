# TCL.SCRIPT

```
TCL.SCRIPT LOAD script
TCL.SCRIPT EXISTS sha1 [sha1 ...]
TCL.SCRIPT FLUSH [ASYNC | SYNC]
```

Manage the **Tcl** script cache used by [`TCL.EVAL`](TCL.EVAL.md) and
[`TCL.EVALSHA`](TCL.EVALSHA.md). Behavior mirrors [`SCRIPT`](SCRIPT.md), on a
cache that is **independent** of the PUC-Lua ([`SCRIPT`](SCRIPT.md)), Luau
([`LUAU.SCRIPT`](LUAU.SCRIPT.md)), and Wren ([`WREN.SCRIPT`](WREN.SCRIPT.md))
caches.

- **`TCL.SCRIPT LOAD script`** — cache the script and return its 40-character
  SHA1 digest. Tcl has no separate compile phase — parse errors normally surface
  only when a command runs — so `LOAD` runs `info complete` to reject the common
  unbalanced-braces/brackets/quotes case **without executing the script**. Other
  errors (an unknown command, a bad argument) still surface at run time.
- **`TCL.SCRIPT EXISTS sha1 …`** — array of `1`/`0`, one per digest.
- **`TCL.SCRIPT FLUSH`** — clear the Tcl cache, reply `+OK`.

## Example

```
> TCL.SCRIPT LOAD "return [string toupper hello]"
"c1a2b3..."

> TCL.SCRIPT EXISTS c1a2b3...
1) (integer) 1

> TCL.SCRIPT LOAD "set x {"
(error) ERR Error compiling script: script is not complete (unbalanced braces, brackets, or quotes)

> TCL.SCRIPT FLUSH
OK
```

## See also

- [`TCL.EVAL`](TCL.EVAL.md), [`TCL.EVALSHA`](TCL.EVALSHA.md).
- [`SCRIPT`](SCRIPT.md) — the PUC-Lua equivalent.
