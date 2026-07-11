# goblin_core — a redis-py-shaped client over shared-memory rings

`goblin_core` is a Python client for [goblin-core](../README.md) that talks to the
server over a **shared-memory ring buffer** (see [docs/ring-buffers.md](../docs/ring-buffers.md))
instead of a socket. The API mirrors [redis-py](https://github.com/redis/redis-py),
so existing code reads the same:

```python
from goblin_core import Redis

r = Redis("/tmp/a", decode_responses=True)   # server: goblin-core --ring /tmp/a 64kb
r.set("user:42", "alice")
r.get("user:42")                              # 'alice'
r.zadd("board", {"alice": 10, "bob": 7})
r.zrange("board", 0, -1, withscores=True)     # [('bob', 7.0), ('alice', 10.0)]
```

The transport, the busy-poll, and RESP parsing are a nanobind (>= 2.12) C++23
extension; the redis-py surface is a thin Python layer on top.

## How the fast path works

- **No socket, no syscall on the request path.** A command is encoded to RESP,
  written into the ring's submission queue, and the reply is read from the
  completion queue — all in shared memory.
- **The PAUSE/YIELD relax hint while polling.** The C++ client busy-polls the
  completion queue and issues a CPU relax hint (`PAUSE` on x86, `YIELD` on ARM, a
  barrier on LoongArch) between spins, so the spinning core lets sibling cores land
  the stores it is waiting on.
- **The GIL is released for the whole round trip.** The spin runs off the GIL, so
  one thread busy-waiting on a ring never starves other Python threads.

## What's implemented

Only the redis-py methods whose commands goblin-core actually implements are
present — there is no `lpush`/`sadd`/`xadd`, because the server has no lists, sets,
or streams. Covered: connection (`ping`, `echo`, `info`), strings (`get`/`set`
with `ex`/`px`/`nx`/`xx`/`keepttl`/`get`, `incr`/`decr`/`incrby`/`incrbyfloat`,
`append`, `strlen`, `getrange`/`setrange`, `mset`/`mget`, `getset`, `setnx`,
`getdel`), keys/TTL (`delete`, `exists`, `type`, `expire`/`pexpire`/`expireat`/
`pexpireat`, `ttl`/`pttl`, `persist`, `expiretime`/`pexpiretime`), hashes
(`hset`/`hget`/`hmget`/`hgetall`/`hkeys`/`hvals`/`hdel`/`hlen`/`hexists`/`hstrlen`/
`hincrby`/`hsetnx`), sorted sets (`zadd`, `zcard`, `zrange`/`zrevrange` with
`withscores`/`desc`, `zrank`/`zrevrank`, `zrem`, `zremrangebyscore`, `zscore`), and
scripting (`eval`, `evalsha`, `script_load`). `execute_command(*args)` reaches
anything else.

`GOBLIN.*` native commands (no redis-py equivalent) are exposed as extension
methods: `cad`, `cas`, `caexpire`, `increx`, `incrbound`, `decrpos`, `hcad`,
`hsetgt`, `zwindow`, `claim`.

`decode_responses=True` returns `str` instead of `bytes`; RESP `-` errors raise
`goblin_core.ResponseError`; a missing ring or a timed-out reply raises
`goblin_core.RingError`.

## Build

Needs `nanobind >= 2.12` and a C++23 compiler.

```sh
python3 -m venv .venv
.venv/bin/pip install "nanobind>=2.12"

# plain CMake build (drops the extension next to goblin_core/__init__.py):
cmake -S python -B python/build -DPython_EXECUTABLE=$(.venv/bin/python -c 'import sys;print(sys.executable)')
cmake --build python/build

# or a pip install (scikit-build-core):
.venv/bin/pip install ./python
```

## Test

Start nothing by hand — the test launches a server itself:

```sh
PYTHONPATH=python .venv/bin/python python/tests/test_ring_roundtrip.py ./build/goblin-core
```

## Caveats

- **Co-located only.** The client and server must share memory (same host). For a
  remote server, use ordinary redis-py over the socket interface.
- **One client per ring file** (a ring is single-writer/single-reader). Run several
  `--ring`s for several clients.
- **Busy-poll burns a core.** This is the low-latency trade; the server does too
  while rings are active.
