# SCRIPT

```
SCRIPT LOAD script
SCRIPT EXISTS sha1 [sha1 ...]
SCRIPT FLUSH [ASYNC | SYNC]
```

Manage the **Lua 5.1** script cache used by [`EVAL`](EVAL.md) and
[`EVALSHA`](EVALSHA.md). (The Luau and Wren engines have their own caches; see
[`LUAU.SCRIPT`](LUAU.SCRIPT.md) and [`WREN.SCRIPT`](WREN.SCRIPT.md).)

## SCRIPT LOAD

```
SCRIPT LOAD script
```

Compile `script` and add it to the cache **without running it**, returning the
40-character SHA1 hex digest of its source. A syntactically invalid script is
rejected (and not cached):

```
> SCRIPT LOAD "return 1 + 1"
"e0e1f9ca3a2a4b0e..."

> SCRIPT LOAD "return ("
(error) ERR Error compiling script (new function): ...
```

## SCRIPT EXISTS

```
SCRIPT EXISTS sha1 [sha1 ...]
```

For each digest, return `1` if a script with that digest is cached, else `0`, as
an array in the same order. Digests are matched case-insensitively.

```
> SCRIPT EXISTS e0e1f9ca3a2a4b0e... 0000000000000000000000000000000000000000
1) (integer) 1
2) (integer) 0
```

## SCRIPT FLUSH

```
SCRIPT FLUSH [ASYNC | SYNC]
```

Remove every script from the cache and reply `+OK`. The optional `ASYNC`/`SYNC`
modifier is accepted and ignored (the flush is immediate).

```
> SCRIPT FLUSH
OK
> SCRIPT EXISTS e0e1f9ca3a2a4b0e...
1) (integer) 0
```

## See also

- [`EVAL`](EVAL.md), [`EVALSHA`](EVALSHA.md).
