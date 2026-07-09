# TCL.EVALSHA

```
TCL.EVALSHA sha1 numkeys [key ...] [arg ...]
```

Run a cached **Tcl** script by the SHA1 hex digest of its source. This is the Tcl
counterpart of [`EVALSHA`](EVALSHA.md); semantics (`numkeys`, the `KEYS`/`ARGV`
Tcl lists, the `redis` command, and the result-to-reply conversion) match
[`TCL.EVAL`](TCL.EVAL.md).

A script enters the Tcl cache when first run with [`TCL.EVAL`](TCL.EVAL.md) or
loaded with [`TCL.SCRIPT LOAD`](TCL.SCRIPT.md). The Tcl cache is **independent**
of the PUC-Lua, Luau, and Wren caches.

If the digest is not cached:

```
(error) NOSCRIPT No matching script. Please use TCL.EVAL.
```

## Example

```
> TCL.SCRIPT LOAD "return 42"
"1fa00e76656cc152ad327c13fe365858fd7be306"

> TCL.EVALSHA 1fa00e76656cc152ad327c13fe365858fd7be306 0
(integer) 42

> TCL.EVALSHA ffffffffffffffffffffffffffffffffffffffff 0
(error) NOSCRIPT No matching script. Please use TCL.EVAL.
```

## See also

- [`TCL.EVAL`](TCL.EVAL.md), [`TCL.SCRIPT`](TCL.SCRIPT.md).
- [`EVALSHA`](EVALSHA.md) — the PUC-Lua equivalent.
