# LUAU.SCRIPT

```
LUAU.SCRIPT LOAD script
LUAU.SCRIPT EXISTS sha1 [sha1 ...]
LUAU.SCRIPT FLUSH [ASYNC | SYNC]
```

Manage the **Luau** script cache used by [`LUAU.EVAL`](LUAU.EVAL.md) and
[`LUAU.EVALSHA`](LUAU.EVALSHA.md). Behavior mirrors [`SCRIPT`](SCRIPT.md), but on
a cache that is **independent** of the PUC-Lua ([`SCRIPT`](SCRIPT.md)) and Wren
([`WREN.SCRIPT`](WREN.SCRIPT.md)) caches.

- **`LUAU.SCRIPT LOAD script`** — compile (do not run) and cache the script,
  returning its 40-character SHA1 digest. Syntactically invalid scripts are
  rejected.
- **`LUAU.SCRIPT EXISTS sha1 …`** — array of `1`/`0`, one per digest.
- **`LUAU.SCRIPT FLUSH`** — clear the Luau cache, reply `+OK`.

## Example

```
> LUAU.SCRIPT LOAD "local x: number = 5 return x"
"9d1e0c..."                        # accepts Luau type annotations

> LUAU.SCRIPT EXISTS 9d1e0c...
1) (integer) 1

> LUAU.SCRIPT FLUSH
OK
```

## See also

- [`LUAU.EVAL`](LUAU.EVAL.md), [`LUAU.EVALSHA`](LUAU.EVALSHA.md).
- [`SCRIPT`](SCRIPT.md) — the PUC-Lua equivalent.
