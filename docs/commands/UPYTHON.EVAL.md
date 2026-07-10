# UPYTHON.EVAL

```
UPYTHON.EVAL script numkeys [key ...] [arg ...]
```

Run a script on **[MicroPython](https://micropython.org)**, a lean Python. This
is a distinct interpreter from [`EVAL`](EVAL.md) (PUC-Lua),
[`LUAU.EVAL`](LUAU.EVAL.md) (Luau), [`WREN.EVAL`](WREN.EVAL.md) (Wren), and
[`TCL.EVAL`](TCL.EVAL.md) (Tcl); it shares the key space with them but has its own
VM and its own [script cache](UPYTHON.SCRIPT.md).

`numkeys` splits keys from args; they are exposed as the lists `KEYS` and `ARGV`,
which are **0-based** (`KEYS[0]` is the first key).

## How returning a value works — Python has no top-level `return`

`return` at module level is a `SyntaxError` in Python, so a `UPYTHON.EVAL` script
produces its reply by **assigning to the module global `reply`** (the Python
analogue of Wren's `Redis.setReply_`). If the script never assigns `reply`, the
reply is nil.

```
> UPYTHON.EVAL "reply = 1 + 2" 0
(integer) 3

> UPYTHON.EVAL "reply = [x * x for x in range(4)]" 0
1) (integer) 0
2) (integer) 1
3) (integer) 4
4) (integer) 9

> UPYTHON.EVAL "x = 5" 0            # never assigns reply
(nil)
```

The script runs in a fresh module namespace each time (so globals do not leak
between scripts), with the Python builtins available.

## Return value: Python → RESP

| `reply` value | RESP reply |
|---|---|
| `int` | integer reply |
| `str` / `bytes` | bulk string |
| `float` | bulk string (its text form) |
| `None` | nil |
| `True` | integer `1`; `False` | nil |
| `list` / `tuple` | array (elements converted recursively) |
| `dict` with a str `"err"` | error reply |
| `dict` with a str `"ok"` | status reply |

## The `redis` module

```
> UPYTHON.EVAL "reply = redis.call('zadd', KEYS[0], 1, 'a', 2, 'b')" 1 board
(integer) 2

> UPYTHON.EVAL "reply = redis.call('zrange', KEYS[0], 0, -1, 'WITHSCORES')" 1 board
1) "a"
2) "1"
3) "b"
4) "2"
```

| Function | Purpose |
|---|---|
| `redis.call(cmd, *args)` | Run a command. A command error **raises a Python exception** — catch it with `try/except`. |
| `redis.pcall(cmd, *args)` | Like `call`, but returns the error message string instead of raising. |
| `redis.error(msg)` | Returns `{'err': msg}` — an error reply. |
| `redis.status(msg)` | Returns `{'ok': msg}` — a status reply. |
| `redis.sha1hex(s)` | 40-character SHA1 hex digest of `s`. |
| `redis.log(level, msg)` | Write `msg` to the server log. |

Because a command error raises, error handling is idiomatic Python `try/except`:

```
> UPYTHON.EVAL "try:\n redis.call('zscore')\nexcept Exception:\n reply = 'caught'" 0
"caught"
```

A Python exception that escapes the script (a command error, `1 // 0`, a
`NameError`, ...) becomes a RESP error reply:

```
> UPYTHON.EVAL "reply = 1 // 0" 0
(error) ERR ZeroDivisionError: divide by zero
```

`redis.call` maps the reply back to a Python value: an integer becomes `int`, a
bulk/status string becomes `str`, an array becomes a `list`, and a null becomes
`None`. (Values come back as `str`, so a numeric reply like a `ZSCORE` of `5` is
the string `'5'`, not an `int` — unlike the other engines there is no value-based
promotion.)

## Sandbox

The VM is built at MicroPython's minimal ROM level: there is no filesystem, no
`os`/`sys`, no sockets, no `import` of host modules. `print()` output goes to the
server log, not to the client. The GC heap is created lazily on the first script.

## See also

- [`UPYTHON.EVALSHA`](UPYTHON.EVALSHA.md) — run a cached script by digest.
- [`UPYTHON.SCRIPT`](UPYTHON.SCRIPT.md) — manage the Python script cache.
- [`EVAL`](EVAL.md), [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md),
  [`TCL.EVAL`](TCL.EVAL.md) — the other interpreters.
