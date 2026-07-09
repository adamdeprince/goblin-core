# WREN.EVALSHA

```
WREN.EVALSHA sha1 numkeys [key ...] [arg ...]
```

Run a cached **Wren** script by the SHA1 hex digest of its source. This is the
Wren counterpart of [`EVALSHA`](EVALSHA.md); semantics (`numkeys`, the 0-based
`KEYS`/`ARGV` Lists, the `Redis` class, the wrapping that captures the return
value, and the conversions) match [`WREN.EVAL`](WREN.EVAL.md).

A script enters the Wren cache when first run with [`WREN.EVAL`](WREN.EVAL.md) or
loaded with [`WREN.SCRIPT LOAD`](WREN.SCRIPT.md). The Wren cache is **independent**
of the PUC-Lua and Luau caches.

If the digest is not cached:

```
(error) NOSCRIPT No matching script. Please use WREN.EVAL.
```

## Example

```
> WREN.SCRIPT LOAD "return 42"
"1fa00e76656cc152ad327c13fe365858fd7be306"

> WREN.EVALSHA 1fa00e76656cc152ad327c13fe365858fd7be306 0
(integer) 42

> WREN.EVALSHA ffffffffffffffffffffffffffffffffffffffff 0
(error) NOSCRIPT No matching script. Please use WREN.EVAL.
```

## See also

- [`WREN.EVAL`](WREN.EVAL.md), [`WREN.SCRIPT`](WREN.SCRIPT.md).
- [`EVALSHA`](EVALSHA.md) — the PUC-Lua equivalent.
