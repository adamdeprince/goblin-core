#!/usr/bin/env python3
"""Benchmark every way to do compare-and-delete (the Redlock unlock idiom).

Compares, on one Goblin Core server, all the implementations of "delete a key
only if it still holds the expected token":

  * GOBLIN.CAD          -- the native, single-op command (1 round trip, C++)
  * <engine>.EVALSHA    -- the idiom scripted in each embedded language and run
                           by digest: PUC-Lua, Luau, Wren, Jim Tcl, MicroPython,
                           QuickJS (1 round trip each, interpreter)
  * GET + DEL           -- the client-side, non-pipelined version: GET, compare
                           in the client, then DEL on a match. Two round trips,
                           and *racy* -- another client can change the key between
                           the GET and the DEL, which is exactly the bug the
                           atomic forms avoid. Included as the naive baseline.

Every script is compiled once with SCRIPT LOAD *before* timing starts, so the
per-engine numbers measure execution of the precompiled script, not compilation.

This is a sequential (non-pipelined) latency benchmark: each operation is a full
request/response, so the numbers reflect per-op round-trip + server cost from a
client's point of view -- which is the whole point, since the naive baseline
pays two round trips. A single Python connection is not a peak-throughput load
generator (it is client-bound); for absolute server throughput use a C driver.

Example:

    python3 benchmarks/cad_benchmark.py --goblin-bin build-release/goblin-core \
        --keys 20000 --rounds 5
"""
from __future__ import annotations

import argparse
import statistics
import time
from pathlib import Path
from typing import Callable, Sequence

import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zset_benchmark as zbench  # noqa: E402 - path set above.


# The compare-and-delete idiom in each language, verbatim from the docs
# (docs/commands/*.EVAL.md). Lua is 1-based; Wren/Tcl/Python/JS are 0-based.
LUA_SRC = (
    'if redis.call("get", KEYS[1]) == ARGV[1] then\n'
    '  return redis.call("del", KEYS[1])\n'
    'end\n'
    'return 0'
)
WREN_SRC = (
    'if (Redis.call(["get", KEYS[0]]) == ARGV[0]) {\n'
    '  return Redis.call(["del", KEYS[0]])\n'
    '}\n'
    'return 0'
)
TCL_SRC = (
    'if {[redis call get [lindex $KEYS 0]] eq [lindex $ARGV 0]} {\n'
    '  return [redis call del [lindex $KEYS 0]]\n'
    '}\n'
    'return 0'
)
PY_SRC = (
    'if redis.call("get", KEYS[0]) == ARGV[0]:\n'
    '    reply = redis.call("del", KEYS[0])\n'
    'else:\n'
    '    reply = 0'
)
JS_SRC = (
    'if (redis.call("get", KEYS[0]) === ARGV[0]) {\n'
    '  return redis.call("del", KEYS[0])\n'
    '}\n'
    'return 0'
)

# (label, command prefix, source). The prefix drives SCRIPT LOAD / EVALSHA.
ENGINES = [
    ("EVAL (PUC-Lua 5.1)", "", LUA_SRC),
    ("LUAU.EVAL (Luau)", "LUAU.", LUA_SRC),
    ("WREN.EVAL (Wren)", "WREN.", WREN_SRC),
    ("TCL.EVAL (Jim Tcl)", "TCL.", TCL_SRC),
    ("UPYTHON.EVAL (MicroPython)", "UPYTHON.", PY_SRC),
    ("QUICKJS.EVAL (JavaScript)", "QUICKJS.", JS_SRC),
]

TOKEN = "unique-owner-token-a1b2c3d4"


def compile_scripts(client: zbench.RespClient) -> dict[str, tuple[str, str]]:
    """SCRIPT LOAD every idiom (once, before timing) and return {label: (prefix, sha)}."""
    print("Compiling scripts (SCRIPT LOAD) before benchmarking:")
    loaded: dict[str, tuple[str, str]] = {}
    for label, prefix, src in ENGINES:
        sha = client.command(f"{prefix}SCRIPT", "LOAD", src)
        sha = sha.decode() if isinstance(sha, bytes) else str(sha)
        if len(sha) != 40:
            raise RuntimeError(f"{label}: unexpected SCRIPT LOAD reply {sha!r}")
        loaded[label] = (prefix, sha)
        print(f"  {label:30}  {sha}")
    return loaded


def make_script_op(prefix: str, sha: str) -> Callable[[zbench.RespClient, str, str], object]:
    def op(client: zbench.RespClient, key: str, token: str) -> object:
        return client.command(f"{prefix}EVALSHA", sha, 1, key, token)
    return op


def make_script_cmds(prefix: str, sha: str) -> Callable[[Sequence[str], str], list]:
    def cmds(keys: Sequence[str], token: str) -> list:
        return [(f"{prefix}EVALSHA", sha, 1, k, token) for k in keys]
    return cmds


def native_cmds(keys: Sequence[str], token: str) -> list:
    return [("GOBLIN.CAD", k, token) for k in keys]


def native_op(client: zbench.RespClient, key: str, token: str) -> object:
    return client.command("GOBLIN.CAD", key, token)


def get_del_op(client: zbench.RespClient, key: str, token: str) -> object:
    # The naive, non-atomic version: read, compare in the client, delete on a
    # match. Two round trips, and racy between the GET and the DEL.
    if client.command("GET", key) == token.encode():
        return client.command("DEL", key)
    return 0


def build_approaches(scripts: dict[str, tuple[str, str]]) -> list[dict]:
    # Each approach has a sequential `op` (one call at a time) and, for the atomic
    # single-command forms, a `cmds` builder for the pipelined view. GET+DEL has no
    # pipelined form: its DEL is conditional on the GET reply, so it cannot be a
    # single pipelined request.
    approaches: list[dict] = [
        dict(label="GOBLIN.CAD (native)", op=native_op, cmds=native_cmds, rtt=1),
    ]
    for label, _prefix, _src in ENGINES:
        prefix, sha = scripts[label]
        approaches.append(dict(label=f"{label} EVALSHA",
                               op=make_script_op(prefix, sha),
                               cmds=make_script_cmds(prefix, sha), rtt=1))
    approaches.append(dict(label="GET + DEL (client-side, racy)", op=get_del_op,
                           cmds=None, rtt=2))
    return approaches


def populate(client: zbench.RespClient, keys: Sequence[str], token: str, pipe: int) -> None:
    client.pipeline((("SET", k, token) for k in keys), flush_every=pipe)


def verify(client: zbench.RespClient, op: Callable, token: str) -> None:
    """One correctness pass: a matching token deletes and reports 1; a mismatch
    leaves the key and reports 0."""
    client.command("SET", "cad:verify", token)
    if int(op(client, "cad:verify", token)) != 1:
        raise RuntimeError("compare-and-delete did not report 1 on a match")
    if client.command("EXISTS", "cad:verify") != 0:
        raise RuntimeError("compare-and-delete did not delete the key on a match")
    client.command("SET", "cad:verify", token)
    if int(op(client, "cad:verify", "wrong-token")) != 0:
        raise RuntimeError("compare-and-delete did not report 0 on a mismatch")
    if client.command("EXISTS", "cad:verify") != 1:
        raise RuntimeError("compare-and-delete deleted the key on a mismatch")
    client.command("DEL", "cad:verify")


def measure_sequential(client: zbench.RespClient, op: Callable, keys: Sequence[str],
                       token: str, pipe: int) -> float:
    """Populate the keys (untimed), then time one sequential compare-and-delete
    per key (the match path: each op does the read and the delete). Returns
    seconds per operation."""
    populate(client, keys, token, pipe)
    start = time.perf_counter()
    for key in keys:
        op(client, key, token)
    elapsed = time.perf_counter() - start
    return elapsed / len(keys)


def measure_pipelined(client: zbench.RespClient, cmds: Callable, keys: Sequence[str],
                      token: str, populate_pipe: int, depth: int) -> float:
    """Populate the keys (untimed), then fire all the compare-and-delete commands
    with `depth` in flight at once, so the server (not the round trip) is the
    bottleneck. Returns seconds per operation."""
    populate(client, keys, token, populate_pipe)
    commands = cmds(keys, token)
    start = time.perf_counter()
    client.pipeline(commands, flush_every=depth)
    elapsed = time.perf_counter() - start
    return elapsed / len(keys)


def main(argv: Sequence[str]) -> int:
    default_goblin = Path("build-release/goblin-core")
    if not default_goblin.exists():
        default_goblin = Path("build/goblin-core")

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--keys", type=int, default=20000,
                        help="compare-and-delete operations per timed round")
    parser.add_argument("--rounds", type=int, default=5,
                        help="timed rounds per approach (median reported)")
    parser.add_argument("--pipeline", type=int, default=256,
                        help="in-flight depth for the pipelined (server-throughput) view")
    parser.add_argument("--populate-pipeline", type=int, default=512,
                        help="pipeline depth for the untimed key population")
    args = parser.parse_args(argv)

    server = zbench.start_goblin(binary=args.goblin_bin, rank_cache=False)
    try:
        client = zbench.RespClient("127.0.0.1", server.port,
                                   unix_socket=server.unix_socket)
        keys = [f"cad:bench:{i}" for i in range(args.keys)]
        scripts = compile_scripts(client)
        approaches = build_approaches(scripts)

        print("\nVerifying correctness of each implementation...")
        for a in approaches:
            verify(client, a["op"], TOKEN)
        print("  all implementations agree (match -> 1 + deleted, mismatch -> 0).")

        def report(title, rows, native_us):
            head = f"{'implementation':32} {'us/op':>9} {'kops/s':>9} {'RTT':>4} {'vs CAD':>8}"
            print(f"\n{title}\n{head}\n{'-' * len(head)}")
            for label, us, rtt in sorted(rows, key=lambda r: r[1]):
                print(f"{label:32} {us:9.2f} {1000.0 / us:9.1f} {rtt:>4} {us / native_us:7.2f}x")

        # View 1 -- sequential latency (one op at a time). Shows the per-op
        # round-trip cost: this is where the naive non-pipelined GET+DEL pays its
        # second round trip. A single Python connection is client-bound, so the
        # 1-RTT forms cluster near that floor.
        print(f"\nBenchmarking {args.keys} ops/round, median of {args.rounds} rounds.")
        seq = []
        for a in approaches:
            per_op = [measure_sequential(client, a["op"], keys, TOKEN, args.populate_pipeline)
                      for _ in range(args.rounds)]
            seq.append((a["label"], statistics.median(per_op) * 1e6, a["rtt"]))
        native_seq = next(us for label, us, _ in seq if label.startswith("GOBLIN.CAD"))
        report("Sequential (per-op latency, one request at a time):", seq, native_seq)
        print("\n  RTT = round trips per op. GET+DEL's two round trips (and its race\n"
              "  window between the GET and the DEL) are what the atomic forms remove.")

        # View 2 -- pipelined server throughput (many ops in flight). Amortizes the
        # client round trip so the server-side per-op cost -- native C++ vs each
        # precompiled interpreter -- can surface. GET+DEL has no single-request
        # form, so it is omitted here.
        pipe = []
        for a in approaches:
            if a["cmds"] is None:
                continue
            per_op = [measure_pipelined(client, a["cmds"], keys, TOKEN,
                                        args.populate_pipeline, args.pipeline)
                      for _ in range(args.rounds)]
            pipe.append((a["label"], statistics.median(per_op) * 1e6, a["rtt"]))
        native_pipe = next(us for label, us, _ in pipe if label.startswith("GOBLIN.CAD"))
        report(f"Pipelined (server throughput, depth {args.pipeline}):", pipe, native_pipe)
        print("\n  With the round trip amortized, this ranks the server-side cost of the\n"
              "  native command against each precompiled interpreter. Still one Python\n"
              "  connection, so a C driver is needed for absolute peak throughput.")
        client.close()
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
