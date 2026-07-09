# WREN.EVAL

```
WREN.EVAL script numkeys [key ...] [arg ...]
```

Run a script on **[Wren](https://wren.io)** — a small, class-based scripting
language. Wren is the most different of the three interpreters: it is not a Lua
dialect, and its embedding model forces a different shape for both *returning a
value* and *calling commands*. As with the others, `WREN.EVAL` shares the key
space with [`EVAL`](EVAL.md) / [`LUAU.EVAL`](LUAU.EVAL.md) but has its own VM and
its own [script cache](WREN.SCRIPT.md).

`numkeys` splits keys from args; they are exposed as `KEYS` and `ARGV`, which are
**0-based** Lists (Wren, unlike Lua, indexes from `0`).

## How returning a value works — Wren has no top-level `return`

In Lua you write `return X` at the top level of a script. **Wren has no
module-level `return`** — `return` is only valid inside a function or method
body. A Wren "script" is a module, and a module cannot return a value.

Goblin Core bridges this by wrapping your script in a function and capturing its
result. The body you send is placed inside an `Fn` (a Wren function value), which
is called immediately, and its result is handed to the host through a foreign
method, `Redis.setReply_`. Concretely, `WREN.EVAL "return 1 + 1" 0` is run as if
you had written:

```wren
import "goblin" for Redis

Redis.setReply_((Fn.new {
  var KEYS = Redis.keys      // injected, 0-based
  var ARGV = Redis.argv
  return 1 + 1               // <-- your script body goes here
}).call())
```

The practical consequences:

- **Use `return` to produce a reply**, exactly as in Lua — it returns from the
  wrapper function, and that value becomes the RESP reply.
- **A script with no `return` replies with nil.** A Wren function with no
  `return` yields `null`, which converts to a null bulk (`$-1`):

  ```
  > WREN.EVAL "return 1 + 1" 0
  (integer) 2

  > WREN.EVAL "var x = 5" 0            # no return
  (nil)
  ```

- `KEYS` and `ARGV` are ordinary locals inside that function, so you can use
  them anywhere in the body.

## Calling commands — the `Redis` class

Wren has no varargs, so `Redis.call` and `Redis.pcall` take a **List** of the
command and its arguments (each a string or a number):

| Binding | Purpose |
|---|---|
| `Redis.call(list)` | Run a command. A command error **aborts** the script (a Wren fiber abort) and becomes the reply. |
| `Redis.pcall(list)` | Like `call`, but a command error is *returned* as a `{"err": ...}` map instead of aborting. |
| `Redis.error(msg)` | Returns `{"err": msg}` — converts to a RESP error reply. |
| `Redis.status(msg)` | Returns `{"ok": msg}` — converts to a RESP status reply. |
| `Redis.sha1hex(s)` | 40-character SHA1 hex digest of `s`. |
| `Redis.log(level, msg)` | Write `msg` to the server log. |
| `KEYS`, `ARGV` | 0-based Lists, injected into every script (see above). |

```
> WREN.EVAL "return Redis.call([\"zadd\", KEYS[0], 5, \"x\"])" 1 board
(integer) 1

> WREN.EVAL "return Redis.call([\"zscore\", KEYS[0], \"x\"])" 1 board
"5"

> ZSCORE board x
"5"
```

Recovering from an error with `pcall` (note Wren map indexing, `r["err"]`):

```
> WREN.EVAL "var r = Redis.pcall([\"zscore\"]); if (r[\"err\"] != null) return \"caught\"" 0
"caught"
```

Shaping the reply:

```
> WREN.EVAL "return Redis.status(\"OK\")" 0
OK

> WREN.EVAL "return Redis.error(\"nope\")" 0
(error) nope
```

## Return value: Wren → RESP

| Wren value | RESP reply |
|---|---|
| `Num` | integer, truncated toward zero (`3.9` → `:3`) |
| `String` | bulk string |
| `null` | null bulk (`$-1`) |
| `false` | null bulk (`$-1`) |
| `true` | integer `1` |
| `List` | multi-bulk array (elements converted recursively) |
| `Map` with a string `"err"` | error reply |
| `Map` with a string `"ok"` | status reply |
| any other `Map` | null bulk |

`Redis.call` performs the inverse mapping: integers become `Num`, bulk strings
become `String`, arrays become `List`, a status reply becomes `{"ok": ...}`, a
null becomes `null`, and an error reply becomes `{"err": ...}` (which
`Redis.call` re-raises, and `Redis.pcall` returns).

## More examples

Wren's expressive standard library is available (this chain runs under
`WREN.EVAL` but is a syntax error under either Lua engine):

```
> WREN.EVAL "return [1, 2, 3, 4].where{|n| n % 2 == 0}.toList" 0
1) (integer) 2
2) (integer) 4
```

Iterating over `ARGV` (0-based) and summing:

```
> WREN.EVAL "var sum = 0
for (n in ARGV) { sum = sum + Num.fromString(n) }
return sum" 0 10 20 12
(integer) 42
```

## Sandbox

Only Wren's built-in modules are available; `import "random"` and `import "meta"`
work, but host modules do not — the VM has no module loader, so filesystem or I/O
imports fail:

```
> WREN.EVAL "import \"io\" for File
return 1" 0
(error) ERR Could not load module 'io'.
```

`System.print(...)` output is written to the server log, not returned to the
client. To keep the scripting VM lean, its heap is capped well below Wren's 10 MB
default.

## See also

- [`WREN.EVALSHA`](WREN.EVALSHA.md) — run a cached Wren script by digest.
- [`WREN.SCRIPT`](WREN.SCRIPT.md) — manage the Wren script cache.
- [`EVAL`](EVAL.md), [`LUAU.EVAL`](LUAU.EVAL.md) — the Lua-family interpreters.
