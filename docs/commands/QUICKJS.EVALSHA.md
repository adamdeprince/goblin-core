# QUICKJS.EVALSHA

```
QUICKJS.EVALSHA sha1 numkeys [key ...] [arg ...]
```

Run a cached **QuickJS** (JavaScript) script by the SHA1 hex digest of its
source. This is the JavaScript counterpart of [`EVALSHA`](EVALSHA.md); semantics
(`numkeys`, the 0-based `KEYS`/`ARGV` arrays, the `redis` object, producing the
reply via `return`, and the conversion) match [`QUICKJS.EVAL`](QUICKJS.EVAL.md).

A script enters the JavaScript cache when first run with
[`QUICKJS.EVAL`](QUICKJS.EVAL.md) or loaded with
[`QUICKJS.SCRIPT LOAD`](QUICKJS.SCRIPT.md). The JavaScript cache is
**independent** of the Lua, Luau, Wren, Tcl, and Python caches.

If the digest is not cached:

```
(error) NOSCRIPT No matching script. Please use QUICKJS.EVAL.
```

## Example

```
> QUICKJS.SCRIPT LOAD "return 42"
"1b3c5e..."

> QUICKJS.EVALSHA 1b3c5e... 0
(integer) 42

> QUICKJS.EVALSHA ffffffffffffffffffffffffffffffffffffffff 0
(error) NOSCRIPT No matching script. Please use QUICKJS.EVAL.
```

## See also

- [`QUICKJS.EVAL`](QUICKJS.EVAL.md), [`QUICKJS.SCRIPT`](QUICKJS.SCRIPT.md).
- [`EVALSHA`](EVALSHA.md) — the PUC-Lua equivalent.
