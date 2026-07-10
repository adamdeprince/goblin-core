# TCL.EVAL

```
TCL.EVAL script numkeys [key ...] [arg ...]
```

Run a script on **[Jim Tcl](https://jim.tcl.tk)**, a small embeddable Tcl. This
is a distinct interpreter from [`EVAL`](EVAL.md) (PUC-Lua),
[`LUAU.EVAL`](LUAU.EVAL.md) (Luau), and [`WREN.EVAL`](WREN.EVAL.md) (Wren); it
shares the key space with them but has its own VM and its own
[script cache](TCL.SCRIPT.md).

`numkeys` splits keys from args; they are exposed as the Tcl list variables
`KEYS` and `ARGV`. Tcl lists are read with `lindex` (0-based):
`[lindex $KEYS 0]` is the first key.

## The script result becomes the reply

Tcl is string-centric — every value is a string. The result of the script (the
result of its last command, or an explicit `return`) is converted to a reply:

| Script result | RESP reply |
|---|---|
| a canonical integer (e.g. `42`, `-7`) | integer reply |
| anything else | bulk string |
| a Tcl error (`error ...`, or any runtime error) | error reply |

```
> TCL.EVAL "expr {1 + 2}" 0
(integer) 3

> TCL.EVAL "return hello" 0
"hello"

> TCL.EVAL "return 5.5" 0
"5.5"

> TCL.EVAL "set s 0; foreach n $ARGV { incr s $n }; return $s" 0 10 20 12
(integer) 42

> TCL.EVAL "error {something broke}" 0
(error) ERR something broke
```

> A numeric string returned from `redis call` (for example a `ZSCORE` of `5`)
> replies as an *integer* under this value-based rule. Use the explicit builders
> below when you need a specific reply type.

## The `redis` command

The `redis` command talks to the store and builds typed replies. Because Tcl has
no varargs, `call`/`pcall` take the command and its arguments as ordinary words:

| Form | Purpose |
|---|---|
| `redis call cmd ?arg ...?` | Run a command. A command error raises a Tcl error (catch it with `catch`). |
| `redis pcall cmd ?arg ...?` | Like `call`, but a command error is *returned* as its message string instead of raising. |
| `redis error msg` | Reply with a RESP error. |
| `redis status msg` | Reply with a RESP status (`+`) line. |
| `redis integer n` | Reply with a RESP integer. |
| `redis array list` | Reply with a RESP array (one element per list item). |
| `redis nil` | Reply with a null. |
| `redis sha1hex s` | The 40-character SHA1 hex digest of `s`. |
| `redis log level msg` | Write `msg` to the server log. |

```
> TCL.EVAL "redis call zadd [lindex $KEYS 0] 1 a 2 b" 1 board
(integer) 2

> TCL.EVAL "redis call zrange [lindex $KEYS 0] 0 -1 WITHSCORES" 1 board
1) "a"
2) "1"
3) "b"
4) "2"

> TCL.EVAL "redis status DONE" 0
DONE

> TCL.EVAL "redis array {a b c}" 0
1) "a"
2) "b"
3) "c"

> TCL.EVAL "redis error {bad input}" 0
(error) bad input
```

Recover from a command error with Tcl's `catch` (the idiomatic form) or `pcall`:

```
> TCL.EVAL "if {[catch {redis call zscore} e]} {return caught}" 0
"caught"
```

## Reply value: RESP → Tcl

`redis call` maps the reply back to a Tcl value: an integer becomes a number, a
bulk/status string becomes a string, an array becomes a Tcl list, a null becomes
the empty string, and an error raises a Tcl error (`pcall` returns its message).

## Sandbox

Only the interpreter core plus Tcl's `stdlib`/`tclcompat` libraries are compiled
in — there is no `open`, `socket`, `exec`, `file`, `glob`, event loop, or child
interpreter. At startup the engine additionally removes `exit`, `source`,
`popen`, and `puts`, and unsets the `env` array:

```
> TCL.EVAL "exit" 0
(error) ERR invalid command name "exit"

> TCL.EVAL "source /etc/passwd" 0
(error) ERR invalid command name "source"
```

## Compare-and-delete — the Redlock unlock idiom

The most-copied Redis script — safe lock release, deleting a key only if it still
holds the token you wrote — in Tcl: the `redis` command talks to the store, and
`KEYS`/`ARGV` are lists read with `lindex` (0-based), compared with `eq`:

```tcl
if {[redis call get [lindex $KEYS 0]] eq [lindex $ARGV 0]} {
  return [redis call del [lindex $KEYS 0]]
}
return 0
```

```
> SET lock:job my-token
OK
> TCL.EVAL "if {[redis call get [lindex $KEYS 0]] eq [lindex $ARGV 0]} { return [redis call del [lindex $KEYS 0]] } return 0" 1 lock:job my-token
(integer) 1
```

Goblin Core also ships this as a native, single-op command — no interpreter:
[`GOBLIN.CAD key expected`](GOBLIN.CAD.md).

## See also

- [`TCL.EVALSHA`](TCL.EVALSHA.md) — run a cached Tcl script by digest.
- [`TCL.SCRIPT`](TCL.SCRIPT.md) — manage the Tcl script cache.
- [`EVAL`](EVAL.md), [`LUAU.EVAL`](LUAU.EVAL.md), [`WREN.EVAL`](WREN.EVAL.md) —
  the other interpreters.
