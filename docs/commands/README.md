# Scripting commands

Goblin Core embeds **six independent scripting interpreters**. Each is reached
through its own command prefix and keeps its own script cache and VM; they share
nothing but the key space (one `Store`).

| Interpreter | Language | Commands |
|---|---|---|
| PUC-Lua 5.1 | the dialect real Redis scripts target | [`EVAL`](EVAL.md) · [`EVALSHA`](EVALSHA.md) · [`SCRIPT`](SCRIPT.md) |
| Luau | Roblox's typed, sandboxed Lua | [`LUAU.EVAL`](LUAU.EVAL.md) · [`LUAU.EVALSHA`](LUAU.EVALSHA.md) · [`LUAU.SCRIPT`](LUAU.SCRIPT.md) |
| Wren | a small class-based language ([wren.io](https://wren.io)) | [`WREN.EVAL`](WREN.EVAL.md) · [`WREN.EVALSHA`](WREN.EVALSHA.md) · [`WREN.SCRIPT`](WREN.SCRIPT.md) |
| Jim Tcl | a small embeddable Tcl ([jim.tcl.tk](https://jim.tcl.tk)) | [`TCL.EVAL`](TCL.EVAL.md) · [`TCL.EVALSHA`](TCL.EVALSHA.md) · [`TCL.SCRIPT`](TCL.SCRIPT.md) |
| MicroPython | a lean Python ([micropython.org](https://micropython.org)) | [`UPYTHON.EVAL`](UPYTHON.EVAL.md) · [`UPYTHON.EVALSHA`](UPYTHON.EVALSHA.md) · [`UPYTHON.SCRIPT`](UPYTHON.SCRIPT.md) |
| QuickJS | JavaScript ([quickjs-ng](https://github.com/quickjs-ng/quickjs)) | [`QUICKJS.EVAL`](QUICKJS.EVAL.md) · [`QUICKJS.EVALSHA`](QUICKJS.EVALSHA.md) · [`QUICKJS.SCRIPT`](QUICKJS.SCRIPT.md) |

The data-type commands that share this key space are documented separately:
**[strings.md](strings.md)** (`SET` / `GET` / `INCR` / `GETRANGE` / …),
**[keys.md](keys.md)** (`DEL` / `EXISTS` / `TYPE`), and **[ttl.md](ttl.md)**
(`EXPIRE` / `TTL` / `PERSIST` / …).

## Why more than one?

`EVAL` runs PUC-Lua 5.1 for **bug-for-bug compatibility** with other Redis
implementations — scripts written for Redis run unchanged. The prefixed commands
opt into a *different* interpreter: `LUAU.EVAL` gives you Luau, `WREN.EVAL` gives
you Wren, `QUICKJS.EVAL` gives you JavaScript. They deliberately do **not** share
behavior with `EVAL`, so choosing one is an explicit decision.

## Concepts shared by all of them

- **Arguments.** `… EVAL script numkeys [key …] [arg …]`. `numkeys` splits the
  trailing arguments into the *keys* the script touches and the plain *args*.
  They are exposed to the script as `KEYS` and `ARGV`.

  > Indexing differs by language: Lua (`EVAL`, `LUAU.EVAL`) is **1-based**
  > (`KEYS[1]`), while Wren (`WREN.EVAL`), Python (`UPYTHON.EVAL`), and
  > JavaScript (`QUICKJS.EVAL`) are **0-based** (`KEYS[0]`); Tcl (`TCL.EVAL`)
  > exposes them as lists read with `lindex` (`[lindex $KEYS 0]`).

- **Atomicity.** The server is single-threaded, so a script runs to completion
  with no other command interleaved. Every `redis.call` inside a script executes
  against the same store synchronously.

- **Calling commands.** Scripts reach the data through a host binding
  (`redis.call` / `redis.pcall` in Lua, Python, and JavaScript, `Redis.call` /
  `Redis.pcall` in Wren, `redis call` / `redis pcall` in Tcl) that re-enters the
  normal command pipeline. A script may **not** call another script command —
  `EVAL`, `EVALSHA`, `SCRIPT`, and the `LUAU.*` / `WREN.*` / `TCL.*` /
  `UPYTHON.*` / `QUICKJS.*` equivalents are rejected from inside a script.

- **Script cache.** Each interpreter caches scripts by the SHA1 of their source.
  `…EVAL` adds to the cache; `…EVALSHA` runs a cached script by its digest;
  `…SCRIPT LOAD/EXISTS/FLUSH` manage the cache. The caches are independent:
  a script loaded with `SCRIPT LOAD` is invisible to `LUAU.EVALSHA`,
  `WREN.EVALSHA`, and `QUICKJS.EVALSHA`, and vice versa.

- **Sandbox.** No interpreter can touch the filesystem, spawn processes, open
  network connections, or load host modules. See each command's page for the
  exact surface.

- **No replication / determinism constraints.** Goblin Core is a single node, so
  scripts are not required to be deterministic and non-deterministic library
  functions are left enabled.

## Return values

A script's return value is converted to a RESP reply. The rules are analogous
across the languages (numbers become integers, strings become bulk strings,
arrays/lists become multi-bulk, a designated shape becomes a status or error
reply). The exact table is on each `…EVAL` page.
