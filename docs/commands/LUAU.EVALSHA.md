# LUAU.EVALSHA

```
LUAU.EVALSHA sha1 numkeys [key ...] [arg ...]
```

Run a cached **Luau** script by the SHA1 hex digest of its source. This is the
Luau counterpart of [`EVALSHA`](EVALSHA.md); semantics (`numkeys`, `KEYS`/`ARGV`,
the `redis` API, conversions) match [`LUAU.EVAL`](LUAU.EVAL.md).

A script enters the Luau cache when first run with [`LUAU.EVAL`](LUAU.EVAL.md) or
loaded with [`LUAU.SCRIPT LOAD`](LUAU.SCRIPT.md). The Luau cache is **independent**
of the PUC-Lua and Wren caches: a digest loaded with `SCRIPT LOAD` is not visible
here.

If the digest is not cached:

```
(error) NOSCRIPT No matching script. Please use LUAU.EVAL.
```

## Example

```
> LUAU.SCRIPT LOAD "return 42"
"1fa00e76656cc152ad327c13fe365858fd7be306"

> LUAU.EVALSHA 1fa00e76656cc152ad327c13fe365858fd7be306 0
(integer) 42

> SCRIPT EXISTS 1fa00e76656cc152ad327c13fe365858fd7be306
1) (integer) 0                     # the PUC-Lua cache does not know this script
```

## See also

- [`LUAU.EVAL`](LUAU.EVAL.md), [`LUAU.SCRIPT`](LUAU.SCRIPT.md).
- [`EVALSHA`](EVALSHA.md) — the PUC-Lua equivalent.
