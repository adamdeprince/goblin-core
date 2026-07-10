# UPYTHON.EVALSHA

```
UPYTHON.EVALSHA sha1 numkeys [key ...] [arg ...]
```

Run a cached **MicroPython** script by the SHA1 hex digest of its source. This is
the Python counterpart of [`EVALSHA`](EVALSHA.md); semantics (`numkeys`, the
0-based `KEYS`/`ARGV` lists, the `redis` module, producing the reply via the
`reply` global, and the conversion) match [`UPYTHON.EVAL`](UPYTHON.EVAL.md).

A script enters the Python cache when first run with
[`UPYTHON.EVAL`](UPYTHON.EVAL.md) or loaded with
[`UPYTHON.SCRIPT LOAD`](UPYTHON.SCRIPT.md). The Python cache is **independent** of
the Lua, Wren, and Tcl caches.

If the digest is not cached:

```
(error) NOSCRIPT No matching script. Please use UPYTHON.EVAL.
```

## Example

```
> UPYTHON.SCRIPT LOAD "reply = 42"
"1fa00e76656cc152ad327c13fe365858fd7be306"

> UPYTHON.EVALSHA 1fa00e76656cc152ad327c13fe365858fd7be306 0
(integer) 42

> UPYTHON.EVALSHA ffffffffffffffffffffffffffffffffffffffff 0
(error) NOSCRIPT No matching script. Please use UPYTHON.EVAL.
```

## See also

- [`UPYTHON.EVAL`](UPYTHON.EVAL.md), [`UPYTHON.SCRIPT`](UPYTHON.SCRIPT.md).
- [`EVALSHA`](EVALSHA.md) — the PUC-Lua equivalent.
