#!/usr/bin/env python3
"""Benchmark each native GOBLIN.* idiom command against the Lua (EVAL) script it
replaces, on one Goblin Core server over a Unix domain socket.

Every native command in this suite exists because Redis's own answer to the idiom
is a Lua script. Each native op calls the store's C++ primitives directly -- no
interpreter, no re-entry into the command processor -- while the Lua form runs the
same logic through the embedded PUC-Lua 5.1 VM via `redis.call`. This measures the
gap.

Commands covered (native  vs  the Lua idiom, precompiled with SCRIPT LOAD):

  GOBLIN.CAD        compare-and-delete a key
  GOBLIN.CAEXPIRE   compare-and-renew a key's TTL
  GOBLIN.CAS        compare-and-set a key (keeping its TTL)
  GOBLIN.ZWINDOW    sliding-window admit (evict + count + add + re-arm TTL)
  GOBLIN.INCRBOUND  bounded increment (consume a quota up to a ceiling)
  GOBLIN.DECRPOS    decrement-if-positive (reserve stock / take a permit)
  GOBLIN.HCAD       compare-and-delete a hash field
  GOBLIN.HSETGT     set-if-greater on a hash field (the ZADD GT hashes lack)
  GOBLIN.CLAIM      idempotency guard (SET NX EX, else GET the prior result)

Each Lua script is SCRIPT LOAD-compiled once before timing, so the Lua numbers are
EVALSHA execution -- not compilation. For every command the keys are populated into
the required state untimed, then one op per key is timed: sequentially (one request
at a time -- the per-op round trip) and pipelined (many in flight -- round trip
amortized, so the server-side cost dominates). All traffic is over the Unix socket,
the realistic transport for a co-located client.

A single Python connection is client-bound, so read the native/Lua ratio across a
row, not the absolute throughput.

    python3 benchmarks/idiom_native_vs_lua.py --goblin-bin build/goblin-core \
        --keys 20000 --rounds 5 --pipeline 256
"""
from __future__ import annotations

import argparse
import os
import statistics
import time
from pathlib import Path
from typing import Callable, Sequence

import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zset_benchmark as zbench  # noqa: E402 - path set above.

TOKEN = "owner-token-a1b2c3d4e5f6"
NEWVAL = "swapped-value-9z8y7x6w"

# ---------------------------------------------------------------------------
# The Lua idioms, verbatim from docs/commands/GOBLIN.*.md (1-based KEYS/ARGV).
# ---------------------------------------------------------------------------
LUA_CAD = (
    'if redis.call("get", KEYS[1]) == ARGV[1] then\n'
    '  return redis.call("del", KEYS[1])\n'
    'end\n'
    'return 0'
)
LUA_CAEXPIRE = (
    'if redis.call("get", KEYS[1]) == ARGV[1] then\n'
    '  return redis.call("pexpire", KEYS[1], ARGV[2])\n'
    'end\n'
    'return 0'
)
LUA_CAS = (
    'if redis.call("get", KEYS[1]) == ARGV[1] then\n'
    '  redis.call("set", KEYS[1], ARGV[2], "KEEPTTL")\n'
    '  return "OK"\n'
    'end\n'
    'return 0'
)
LUA_ZWINDOW = (
    'local now = tonumber(ARGV[1])\n'
    'redis.call("zremrangebyscore", KEYS[1], 0, now - tonumber(ARGV[2]))\n'
    'if redis.call("zcard", KEYS[1]) < tonumber(ARGV[3]) then\n'
    '  redis.call("zadd", KEYS[1], now, ARGV[4])\n'
    '  redis.call("expire", KEYS[1], ARGV[2])\n'
    '  return 1\n'
    'end\n'
    'return 0'
)
LUA_INCRBOUND = (
    'local v = tonumber(redis.call("get", KEYS[1]) or "0")\n'
    'if v + tonumber(ARGV[1]) <= tonumber(ARGV[2]) then\n'
    '  return redis.call("incrby", KEYS[1], ARGV[1])\n'
    'end\n'
    'return -1'
)
LUA_DECRPOS = (
    'local v = tonumber(redis.call("get", KEYS[1]) or "0")\n'
    'if v > 0 then return redis.call("decr", KEYS[1]) end\n'
    'return -1'
)
LUA_HCAD = (
    'if redis.call("hget", KEYS[1], ARGV[1]) == ARGV[2] then\n'
    '  return redis.call("hdel", KEYS[1], ARGV[1])\n'
    'end\n'
    'return 0'
)
LUA_HSETGT = (
    'local cur = tonumber(redis.call("hget", KEYS[1], ARGV[1]) or "-inf")\n'
    'if tonumber(ARGV[2]) > cur then\n'
    '  redis.call("hset", KEYS[1], ARGV[1], ARGV[2])\n'
    '  return 1\n'
    'end\n'
    'return 0'
)
LUA_CLAIM = (
    'if redis.call("set", KEYS[1], ARGV[1], "NX", "EX", ARGV[2]) then\n'
    '  return "CLAIMED"\n'
    'end\n'
    'return redis.call("get", KEYS[2])'
)


# ---------------------------------------------------------------------------
# Per-command spec. `populate` puts the N keys into the state the op expects
# (untimed, re-run each round). `native(k)` / `lua(k, sha)` build one command for
# key k. `numkeys` is the EVALSHA key count. `expect` is the reply both forms must
# return in the verify pass (bytes/int), used only to confirm they agree.
# ---------------------------------------------------------------------------
def set_all(cmd: str, *tail):
    def populate(client, keys, pipe):
        client.pipeline(((cmd, k, *tail) for k in keys), flush_every=pipe)
    return populate


def del_all(client, keys, pipe):
    client.pipeline((("DEL", k) for k in keys), flush_every=pipe)


def hset_all(field, value):
    def populate(client, keys, pipe):
        client.pipeline((("HSET", k, field, value) for k in keys), flush_every=pipe)
    return populate


def zseed_stale(client, keys, pipe):
    # One already-expired entry (score 0) per window, so each timed ZWINDOW does a
    # real eviction (zremrangebyscore) before the count-and-add, as the idiom does.
    client.pipeline((("ZADD", k, 0, "stale") for k in keys), flush_every=pipe)


SPECS = [
    dict(name="GOBLIN.CAD", lua=LUA_CAD, numkeys=1,
         populate=set_all("SET", TOKEN),
         native=lambda k: ("GOBLIN.CAD", k, TOKEN),
         argv=lambda k: (k, TOKEN), expect=1),
    dict(name="GOBLIN.CAEXPIRE", lua=LUA_CAEXPIRE, numkeys=1,
         populate=set_all("SET", TOKEN),
         native=lambda k: ("GOBLIN.CAEXPIRE", k, TOKEN, 100000),
         argv=lambda k: (k, TOKEN, 100000), expect=1),
    dict(name="GOBLIN.CAS", lua=LUA_CAS, numkeys=1,
         populate=set_all("SET", TOKEN),
         native=lambda k: ("GOBLIN.CAS", k, TOKEN, NEWVAL),
         argv=lambda k: (k, TOKEN, NEWVAL), expect="OK"),
    dict(name="GOBLIN.ZWINDOW", lua=LUA_ZWINDOW, numkeys=1,
         populate=zseed_stale,
         native=lambda k: ("GOBLIN.ZWINDOW", k, 1000, 100, 100, "m"),
         argv=lambda k: (k, 1000, 100, 100, "m"), expect=1),
    dict(name="GOBLIN.INCRBOUND", lua=LUA_INCRBOUND, numkeys=1,
         populate=set_all("SET", 0),
         native=lambda k: ("GOBLIN.INCRBOUND", k, 1, 1000000000),
         argv=lambda k: (k, 1, 1000000000), expect=1),
    dict(name="GOBLIN.DECRPOS", lua=LUA_DECRPOS, numkeys=1,
         populate=set_all("SET", 1000000000),
         native=lambda k: ("GOBLIN.DECRPOS", k),
         argv=lambda k: (k,), expect=999999999),
    dict(name="GOBLIN.HCAD", lua=LUA_HCAD, numkeys=1,
         populate=hset_all("f", TOKEN),
         native=lambda k: ("GOBLIN.HCAD", k, "f", TOKEN),
         argv=lambda k: (k, "f", TOKEN), expect=1),
    dict(name="GOBLIN.HSETGT", lua=LUA_HSETGT, numkeys=1,
         populate=hset_all("f", 5),
         native=lambda k: ("GOBLIN.HSETGT", k, "f", 10),
         argv=lambda k: (k, "f", 10), expect=1),
    dict(name="GOBLIN.CLAIM", lua=LUA_CLAIM, numkeys=2,
         populate=del_all,
         native=lambda k: ("GOBLIN.CLAIM", k, k + ":r", TOKEN, 100),
         argv=lambda k: (k, k + ":r", TOKEN, 100), expect="CLAIMED"),
]


def load_script(client: zbench.RespClient, src: str) -> str:
    sha = client.command("SCRIPT", "LOAD", src)
    sha = sha.decode() if isinstance(sha, bytes) else str(sha)
    if len(sha) != 40:
        raise RuntimeError(f"unexpected SCRIPT LOAD reply {sha!r}")
    return sha


def lua_cmd(spec, sha, k):
    return ("EVALSHA", sha, spec["numkeys"], *spec["argv"](k))


def norm(x):
    # Collapse RESP type differences that are semantically equal: a simple string
    # (+OK, decoded to str) and a bulk string ($2 OK, bytes) both normalize to "OK".
    if isinstance(x, bytes):
        return x.decode()
    if isinstance(x, int):
        return str(x)
    return str(x)


def verify(client: zbench.RespClient, spec, sha) -> None:
    """One correctness pass: the native form and the Lua form must return the same
    reply for the same starting state (a parity check that doubles as a smoke test)."""
    want = norm(spec["expect"])
    for build, tag in ((spec["native"], "native"), (lambda k: lua_cmd(spec, sha, k), "lua")):
        key = f"verify:{spec['name']}"
        spec["populate"](client, [key], 1)
        got = norm(client.command(*build(key)))
        if got != want:
            raise RuntimeError(f"{spec['name']} {tag}: expected {want!r}, got {got!r}")
        client.command("DEL", key)
        client.command("DEL", key + ":r")


def measure_seq(client, build, keys, populate, pipe) -> float:
    populate(client, keys, pipe)
    start = time.perf_counter()
    for k in keys:
        client.command(*build(k))
    return (time.perf_counter() - start) / len(keys)


def measure_pipe(client, build, keys, populate, pop_pipe, depth) -> float:
    populate(client, keys, pop_pipe)
    cmds = [build(k) for k in keys]
    start = time.perf_counter()
    client.pipeline(cmds, flush_every=depth)
    return (time.perf_counter() - start) / len(keys)


def main(argv: Sequence[str]) -> int:
    default_goblin = Path("build-release/goblin-core")
    if not default_goblin.exists():
        default_goblin = Path("build/goblin-core")

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--keys", type=int, default=20000,
                        help="ops per timed round (one per key)")
    parser.add_argument("--rounds", type=int, default=5,
                        help="timed rounds per measurement (median reported)")
    parser.add_argument("--pipeline", type=int, default=256,
                        help="in-flight depth for the pipelined view")
    parser.add_argument("--populate-pipeline", type=int, default=512)
    parser.add_argument("--unix-socket", type=str, default=None)
    args = parser.parse_args(argv)

    sock = args.unix_socket or f"/tmp/goblin-idiom-{os.getpid()}.sock"
    server = zbench.start_goblin(binary=args.goblin_bin, rank_cache=False,
                                 unix_socket=sock)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, unix_socket=sock)
        # Each command gets its own key namespace so leftover state from one (e.g.
        # CAS leaving strings behind) cannot WRONGTYPE the next (ZWINDOW's ZADD).
        keys_for = {spec["name"]: [f"{spec['name']}:{i}" for i in range(args.keys)]
                    for spec in SPECS}

        print("Compiling Lua idioms (SCRIPT LOAD) before timing:")
        shas = {}
        for spec in SPECS:
            shas[spec["name"]] = load_script(client, spec["lua"])
            print(f"  {spec['name']:20}  {shas[spec['name']]}")

        print("\nVerifying native and Lua agree on each idiom (over UDS)...")
        for spec in SPECS:
            verify(client, spec, shas[spec["name"]])
        print("  all idioms: native reply == Lua reply.")

        depth, pop = args.pipeline, args.populate_pipeline

        def med(fn) -> float:
            return statistics.median([fn() for _ in range(args.rounds)]) * 1e6

        print(f"\nBenchmarking {args.keys} ops/round, median of {args.rounds} rounds, "
              f"pipeline depth {depth}, over the Unix socket.\n")
        head = (f"{'command':18}{'native seq':>12}{'lua seq':>11}{'seq x':>8}"
                f"{'native p'+str(depth):>13}{'lua p'+str(depth):>12}{'pipe x':>8}")
        print(head)
        print("-" * len(head))
        rows = []
        for spec in SPECS:
            sha = shas[spec["name"]]
            keys = keys_for[spec["name"]]
            nat = spec["native"]
            lua = lambda k, sha=sha, spec=spec: lua_cmd(spec, sha, k)
            pop_fn = spec["populate"]
            ns = med(lambda nat=nat, pop_fn=pop_fn: measure_seq(client, nat, keys, pop_fn, pop))
            ls = med(lambda lua=lua, pop_fn=pop_fn: measure_seq(client, lua, keys, pop_fn, pop))
            npi = med(lambda nat=nat, pop_fn=pop_fn: measure_pipe(client, nat, keys, pop_fn, pop, depth))
            lpi = med(lambda lua=lua, pop_fn=pop_fn: measure_pipe(client, lua, keys, pop_fn, pop, depth))
            rows.append((spec["name"], ns, ls, npi, lpi))
            print(f"{spec['name']:18}{ns:12.2f}{ls:11.2f}{ls/ns:7.2f}x"
                  f"{npi:13.2f}{lpi:12.2f}{lpi/npi:7.2f}x")

        print(f"\n  us/op over the Unix socket. seq = one request at a time (per-op round\n"
              f"  trip); p{depth} = {depth} in flight (server-side cost dominates). 'x' is the\n"
              f"  Lua/native ratio -- how many times the native op's throughput beats the\n"
              f"  precompiled Lua script. One client connection, so read the ratios.")
        client.close()
    finally:
        server.stop()
        if not args.unix_socket:
            try:
                os.unlink(sock)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
