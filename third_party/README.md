# Vendored third-party code

These are the runtime and helper libraries embedded to implement Lua scripting
(`EVAL` / `EVALSHA` / `SCRIPT`). Each was taken **verbatim from its own public
upstream** — none is Redis (or Valkey/Dragonfly) source. They are built into the
separate `goblin_lua` static library so they compile as plain C, isolated from
`goblin_core`'s strict warning flags.

| Directory        | Library      | Version | License      | Upstream |
|------------------|--------------|---------|--------------|----------|
| `lua/`           | Lua          | 5.1.5   | MIT          | https://www.lua.org/ftp/lua-5.1.5.tar.gz |
| `lua-cjson/`     | lua-cjson    | 2.1.0   | MIT          | https://github.com/mpx/lua-cjson |
| `lua-cmsgpack/`  | lua-cmsgpack | master  | BSD-2-Clause | https://github.com/antirez/lua-cmsgpack |
| `lua-struct/`    | Lua `struct` | —       | MIT          | https://www.inf.puc-rio.br/~roberto/struct/ |
| `lua-bitop/`     | LuaBitOp     | 1.0.2   | MIT          | https://bitop.luajit.org/ |

Lua 5.1 was chosen deliberately: it is the language dialect Redis scripts target
(so existing scripts port unchanged), it is pure ANSI C that builds on every ISA
Goblin Core targets — including LoongArch, where LuaJIT has no upstream support —
and its resident footprint is small and predictable, which matters for the
project's memory-first design. The VM is created lazily on the first script, so a
server that never runs one pays nothing.

## Luau (second interpreter, `LUAU.*` commands)

`luau/` holds the minimal subset of [Luau](https://github.com/luau-lang/luau)
(Roblox's typed, sandboxed Lua dialect) needed to compile and run scripts:
`Common`, `Ast`, `Bytecode`, `Compiler`, and `VM` (no `CodeGen` JIT, no
`Analysis` type-checker). It builds as its own C++17 static library
(`goblin_luau_vm`). Because Luau's public API has C++ linkage while PUC-Lua's has
C linkage, the two runtimes coexist in one binary; the only overlap is three
internal *data* symbols (`lua_ident`, `luaO_nilobject_`, `luaT_typenames`), which
the build renames on the Luau side. Each engine source is handed only its own
runtime's headers (both ship a `lua.h`) via a per-source include in CMakeLists.

Luau is the deliberately *different* interpreter reached through `LUAU.EVAL`: it
speaks the Luau dialect (type annotations, `continue`, string interpolation) and
ships Luau's standard library (`bit32`, `buffer`, `utf8`, `vector`) rather than
the PUC helper libraries above. The PUC engine stays behind `EVAL` for bug-for-bug
compatibility with other Redis implementations.

## Wren (third interpreter, `WREN.*` commands)

`wren/` holds the [Wren](https://wren.io) runtime (the `src/vm`, `src/optional`,
and `src/include` trees, including the checked-in `*.wren.inc` generated core
sources). It builds as its own C static library (`goblin_wren`) with the meta and
random optional modules enabled. Wren's symbols are `wren`-prefixed C symbols, so
they collide with neither Lua runtime.

Wren is class-based with no top-level `return` and no in-language `eval`, so
`WREN.EVAL` runs the script body inside a function and captures its result
through a foreign `Redis.setReply_`. The host bindings live on a foreign `Redis`
class: `Redis.call(list)` / `Redis.pcall(list)` (Wren has no varargs, so
arguments are passed as a List), `Redis.error` / `Redis.status` / `Redis.sha1hex`
/ `Redis.log`, and `KEYS` / `ARGV` as **0-based** Lists (Wren is 0-indexed). The
VM's `initialHeapSize` is lowered from Wren's 10 MB default to keep the scripting
footprint small.

## Jim Tcl (fourth interpreter, `TCL.*` commands)

`jimtcl/` holds a minimal subset of [Jim Tcl](https://jim.tcl.tk), a small
embeddable Tcl: the interpreter core (`jim.c`) plus `format`, `regexp`, `subcmd`,
`iocompat`, and `package`, and the generated `_stdlib.c` / `_tclcompat.c`
Tcl-library sources. No I/O, exec, socket, event-loop, child-interp, oo, or tree
extensions are compiled in. `jimautoconf.h` is **hand-written** (portable, only
the features the subset references) so the copy builds identically on Linux and
macOS with no autosetup step, and `_load-static-exts.c` is a hand-written loader
that initializes only stdlib and tclcompat. It builds as its own C static library
(`goblin_jim`); its symbols are `Jim_`-prefixed and collide with nothing else.

Tcl is string-centric, so `TCL.EVAL` maps the script result to the reply as: a
canonical integer replies as an integer, everything else as a bulk string; the
`redis` command provides `redis call`/`pcall` plus explicit reply builders
(`redis error`/`status`/`integer`/`array`/`nil`). KEYS/ARGV are Tcl lists. The
engine deletes process- and host-touching commands (`exit`, `source`, `popen`,
`puts`) and unsets `env` at startup.

## What is *not* vendored

The `EVAL` / `EVALSHA` / `SCRIPT` commands, the `redis` / `server` Lua API table
(`redis.call`, `redis.pcall`, `redis.error_reply`, `redis.status_reply`,
`redis.sha1hex`, `redis.log`, `redis.setresp`, `redis.replicate_commands`, the
`REPL_*` / `LOG_*` constants), the sandbox, and the bidirectional RESP↔Lua value
conversion live in `src/script.cpp`. They were written from the **public Redis
scripting specification**, not from any other engine's source.

## Local modifications

The Lua drop is the upstream `src/` tree with only the standalone `lua.c`
interpreter and `luac.c`/`print.c` compiler mains removed (an embedded runtime
has no use for them). lua-cjson keeps the `snprintf`-based float path and omits
the optional David Gay `dtoa.c` / `g_fmt.c`. Every other file is upstream,
byte-for-byte, with its original license header intact.

## Updating

Re-download the upstream tarball/file, copy the same file set into place, and
rebuild. No patches need to be re-applied.
