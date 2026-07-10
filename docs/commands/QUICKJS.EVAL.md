# QUICKJS.EVAL

```
QUICKJS.EVAL script numkeys [key ...] [arg ...]
```

Run a script on **[QuickJS](https://github.com/quickjs-ng/quickjs)** (the
`quickjs-ng` fork), a small embeddable JavaScript engine. This is a distinct
interpreter from [`EVAL`](EVAL.md) (PUC-Lua), [`LUAU.EVAL`](LUAU.EVAL.md) (Luau),
[`WREN.EVAL`](WREN.EVAL.md) (Wren), [`TCL.EVAL`](TCL.EVAL.md) (Tcl), and
[`UPYTHON.EVAL`](UPYTHON.EVAL.md) (MicroPython); it shares the key space with them
but has its own VM and its own [script cache](QUICKJS.SCRIPT.md).

`numkeys` splits keys from args; they are exposed as the arrays `KEYS` and
`ARGV`, which are **0-based** (`KEYS[0]` is the first key).

## How returning a value works

The script body runs **inside a function**, so `return <value>` produces the
reply — and any `var` / `let` / `const` / `function` you declare stays
script-local instead of leaking into the shared context. If the script never
returns, the reply is nil.

```
> QUICKJS.EVAL "return 1 + 2" 0
(integer) 3

> QUICKJS.EVAL "return [0, 1, 2, 3].map(x => x * x)" 0
1) (integer) 0
2) (integer) 1
3) (integer) 4
4) (integer) 9

> QUICKJS.EVAL "var x = 5;" 0            # never returns
(nil)
```

## Return value: JavaScript → RESP

| Returned value | RESP reply |
|---|---|
| a whole `number` | integer reply |
| a non-whole `number` | bulk string (its text form, e.g. `3.5`) |
| `string` | bulk string |
| `true` | integer `1`; `false` | nil |
| `null` / `undefined` | nil |
| `Array` | array (elements converted recursively) |
| object with a string `err` property | error reply |
| object with a string `ok` property | status reply |
| any other object | nil |

## The `redis` object

```
> QUICKJS.EVAL "return redis.call('zadd', KEYS[0], 1, 'a', 2, 'b')" 1 board
(integer) 2

> QUICKJS.EVAL "return redis.call('zrange', KEYS[0], 0, -1, 'WITHSCORES')" 1 board
1) "a"
2) "1"
3) "b"
4) "2"
```

| Function | Purpose |
|---|---|
| `redis.call(cmd, ...args)` | Run a command. A command error **throws** — catch it with `try/catch`. |
| `redis.pcall(cmd, ...args)` | Like `call`, but returns `{ err: msg }` instead of throwing. |
| `redis.error(msg)` | Returns `{ err: msg }` — an error reply. |
| `redis.status(msg)` | Returns `{ ok: msg }` — a status reply. |
| `redis.sha1hex(s)` | 40-character SHA1 hex digest of `s`. |
| `redis.log(level, msg)` | Write `msg` to the server log. |

Because a command error throws, error handling is idiomatic `try/catch`:

```
> QUICKJS.EVAL "try { redis.call('zscore') } catch (e) { return 'caught' }" 0
"caught"
```

An exception that escapes the script (a command error, a `ReferenceError`, a
`throw`, ...) becomes a RESP error reply:

```
> QUICKJS.EVAL "return undefinedThing()" 0
(error) ERR ReferenceError: undefinedThing is not defined
```

`redis.call` maps the reply back to a JavaScript value: an integer reply becomes
a `number`, a bulk/simple string becomes a `string`, an array becomes an
`Array`, and a null becomes `null`. Numbers are passed to commands as integers
when whole and as their `%.17g` text otherwise. (A bulk reply comes back as a
`string`, so a numeric reply like a `ZSCORE` of `5` is the string `"5"`, not a
`number` — there is no value-based promotion.)

## Sandbox

Only the core QuickJS engine is compiled in — there is **no** `quickjs-libc`, so
a script has no filesystem, no `os`/`std` modules, no sockets, and no host module
loading; it is pure computation plus the `redis` binding above. The runtime is
also capped with a memory limit and a maximum stack size. `KEYS` and `ARGV` are
the only injected globals besides `redis`. The runtime is created lazily on the
first script.

## Compare-and-delete — the Redlock unlock idiom

The most-copied Redis script — safe lock release, deleting a key only if it still
holds the token you wrote — in JavaScript: `KEYS`/`ARGV` are 0-based arrays,
compared with strict `===`:

```javascript
if (redis.call("get", KEYS[0]) === ARGV[0]) {
  return redis.call("del", KEYS[0])
}
return 0
```

```
> SET lock:job my-token
OK
> QUICKJS.EVAL "if (redis.call('get', KEYS[0]) === ARGV[0]) { return redis.call('del', KEYS[0]) } return 0" 1 lock:job my-token
(integer) 1
```

Goblin Core also ships this as a native, single-op command — no interpreter:
[`GOBLIN.CAD key expected`](GOBLIN.CAD.md).

## See also

- [`QUICKJS.EVALSHA`](QUICKJS.EVALSHA.md) — run a cached script by digest.
- [`QUICKJS.SCRIPT`](QUICKJS.SCRIPT.md) — manage the JavaScript script cache.
- [`EVAL`](EVAL.md), [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md),
  [`TCL.EVAL`](TCL.EVAL.md), [`UPYTHON.EVAL`](UPYTHON.EVAL.md) — the other
  interpreters.
