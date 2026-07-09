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
