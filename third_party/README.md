# Vendored third-party code

These are runtime and helper libraries embedded by Goblin Core. Each was taken
**verbatim from its own public upstream**; none is Redis (or Valkey/Dragonfly)
source. The scripting C sources are built into separate static libraries so they
remain isolated from `goblin_core`'s strict warning flags.

| Directory        | Library      | Version | License      | Upstream |
|------------------|--------------|---------|--------------|----------|
| `lua/`           | Lua          | 5.1.5   | MIT          | https://www.lua.org/ftp/lua-5.1.5.tar.gz |
| `lua-cjson/`     | lua-cjson    | 2.1.0   | MIT          | https://github.com/mpx/lua-cjson |
| `lua-cmsgpack/`  | lua-cmsgpack | master  | BSD-2-Clause | https://github.com/antirez/lua-cmsgpack |
| `lua-struct/`    | Lua `struct` | —       | MIT          | https://www.inf.puc-rio.br/~roberto/struct/ |
| `lua-bitop/`     | LuaBitOp     | 1.0.2   | MIT          | https://bitop.luajit.org/ |
| `unordered_dense/` | unordered_dense | 4.8.1 | MIT | https://github.com/martinus/unordered_dense |
| `xxhash/`          | xxHash (XXH3) | 0.8.3 | BSD-2-Clause | https://github.com/Cyan4973/xxHash |
| `fast_float/`      | fast_float (header-only) | 8.0.2 | MIT OR Apache-2.0 | https://github.com/fastfloat/fast_float |
| `librdkafka/`      | librdkafka | 2.15.0 | BSD-2-Clause | https://github.com/confluentinc/librdkafka |
| `xlio/`            | NVIDIA XLIO | 3.61.2 (`ae821447`) | GPL-2.0-only OR BSD-2-Clause; BSD selected | https://github.com/Mellanox/libxlio |
| `libdpcp/`         | NVIDIA DPCP | 1.1.61 (`4cc43b30`) | BSD-3-Clause | https://github.com/Mellanox/libdpcp |

`librdkafka/` contains the upstream C client, its CMake support, license files,
and the small C++ wrapper directory expected by upstream CMake. Examples, tests,
packaging, and repository metadata are omitted. Goblin Core links only the C
static library and disables optional system-dependent TLS/SASL, curl, zlib, and
zstd integrations; librdkafka's bundled Snappy and LZ4 support remains enabled.

`xlio/` and `libdpcp/` are complete pinned upstream source trees with repository
metadata omitted. At the July 22, 2026 dependency audit, XLIO 3.61.2 was the
latest non-prerelease line with the server-side Ultra API. DPCP is its mandatory
packet-control dependency; 1.1.61 satisfies XLIO's minimum 1.1.58 requirement.
Goblin Core uses XLIO under its BSD-2-Clause option. XLIO's small event-worker
file set uses the alternative BSD-3-Clause option, its bundled json-c is MIT
licensed, and DPCP is BSD-3-Clause (with Apache-2.0 CMake support). All are
compatible with Goblin Core's Apache-2.0 distribution. XLIO's retained lwIP,
FreeBSD-derived TCP congestion-control, and test-only GoogleTest sources also
use permissive BSD-style licenses.

One XLIO source patch is carried locally: device discovery skips verbs devices
whose physical ports are all InfiniBand link-layer ports. XLIO supports Ethernet,
and attempting to open an unrelated InfiniBand-only Connect-IB device through
DPCP otherwise aborts startup on a mixed-fabric host before XLIO reaches the
ConnectX Ethernet adapter.

The pinned dependency and ConnectX-5 qualification record are documented in
[`docs/xlio.md`](../docs/xlio.md). XLIO is vendored but is not yet wired into
the Goblin Core build or runtime.

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

## MicroPython (fifth interpreter, `UPYTHON.*` commands)

`micropython/` holds a self-contained C tree produced by
[MicroPython](https://micropython.org)'s embed port (`ports/embed`): the whole
`micropython_embed/` package (`py/`, `shared/runtime/`, `port/`, and the
generated `genhdr/`), plus the `mpconfigport.h` it was generated from. The config
is MicroPython's minimal ROM level plus byte-strings (binary-safe keys/values),
floats, big integers, and readable error messages. Only `port/mphalport.c` is
modified from upstream, to route `print()` to the server log rather than stdout.
It builds as its own C static library (`goblin_upy`); its `mp_`-prefixed symbols
collide with nothing. To regenerate after a config change, run the embed port's
makefile against `mpconfigport.h`.

Python has no top-level return, so a `UPYTHON.EVAL` script produces its reply by
assigning to the module global `reply` (the Python analogue of Wren's
`Redis.setReply_`). `redis.call(...)` talks to the store and raises a Python
exception on a command error (so `try/except` works); `redis.error`/`status`/
`sha1hex`/`log` are also provided. KEYS/ARGV are 0-based lists. The GC heap is
allocated lazily on the first script.

## QuickJS (sixth interpreter, `QUICKJS.*` commands)

`quickjs/` holds the four core sources of
[quickjs-ng](https://github.com/quickjs-ng/quickjs) 0.15.1 (a maintained fork of
Bellard's QuickJS) — `quickjs.c`, `libregexp.c`, `libunicode.c`, and `dtoa.c` —
with their headers and `LICENSE`. It is a small, complete JavaScript (ES2023)
engine. It builds as its own C11 static library (`goblin_qjs`); its `JS_`-prefixed
symbols collide with nothing else. **`quickjs-libc.c` is deliberately not
vendored** — that is the file/os/network layer — so the embedded engine is pure
computation plus the host binding, with no way to reach the host.

JavaScript has no top-level return, so a `QUICKJS.EVAL` script body runs **inside
a function**: `return <value>` produces the reply and any declaration stays
script-local (no leakage through the shared context). The host bindings live on a
`redis` global: `redis.call(cmd, ...args)` / `redis.pcall(...)` re-enter the
command pipeline (`call` throws a JS `Error` on a command error, so `try/catch`
works; `pcall` returns `{ err }`), plus `redis.error`/`status`/`sha1hex`/`log`.
KEYS/ARGV are **0-based** arrays. The runtime is sandboxed with a memory limit and
a maximum stack size, and is created lazily on the first script.

## What is *not* vendored

libsodium is a required system dependency used for Argon2id credential hashing;
it is discovered and linked by CMake and is not copied into `third_party/`.

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
