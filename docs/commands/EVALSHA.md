# EVALSHA

```
EVALSHA sha1 numkeys [key ...] [arg ...]
```

Run a **Lua 5.1** script that is already in the script cache, identified by the
40-character SHA1 hex digest of its source. Everything else — `numkeys`, `KEYS`,
`ARGV`, the `redis` API, and the return-value conversion — is exactly as for
[`EVAL`](EVAL.md).

A script enters the cache when it is first run with [`EVAL`](EVAL.md) or loaded
with [`SCRIPT LOAD`](SCRIPT.md). The digest is case-insensitive.

If no script with that digest is cached, the reply is:

```
(error) NOSCRIPT No matching script. Please use EVAL.
```

The intended pattern is *optimistic*: send `EVALSHA`, and if it returns
`NOSCRIPT`, fall back to `EVAL` with the full source (which re-caches it). This
keeps the script body off the wire on the common path.

## Example

```
> SCRIPT LOAD "return 42"
"1fa00e76656cc152ad327c13fe365858fd7be306"

> EVALSHA 1fa00e76656cc152ad327c13fe365858fd7be306 0
(integer) 42

> EVALSHA ffffffffffffffffffffffffffffffffffffffff 0
(error) NOSCRIPT No matching script. Please use EVAL.
```

`EVAL` also populates the cache, so its scripts are immediately `EVALSHA`-able:

```
> EVAL "return redis.sha1hex('return 7')" 0
"..."                              # the digest of "return 7"
> EVAL "return 7" 0                # runs and caches
(integer) 7
> EVALSHA <that digest> 0
(integer) 7
```

## See also

- [`EVAL`](EVAL.md) — full script semantics and conversion tables.
- [`SCRIPT`](SCRIPT.md) — `LOAD` / `EXISTS` / `FLUSH`.
- [`LUAU.EVALSHA`](LUAU.EVALSHA.md), [`WREN.EVALSHA`](WREN.EVALSHA.md) — the
  other interpreters' caches (independent from this one).
