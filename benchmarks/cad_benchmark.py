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

It reports four numbers per implementation, from one server listening on both
transports: sequential (one request/response at a time -- the per-op round trip,
where the naive baseline pays for two) and pipelined (many in flight -- round
trip amortized, so server-side cost shows), each over TCP loopback and over a
Unix domain socket (the realistic transport for a co-located lock client). A
single Python connection is client-bound, not a peak-throughput load generator;
read the ratios, and use a C driver for absolute server throughput.

Example:

    python3 benchmarks/cad_benchmark.py --goblin-bin build-release/goblin-core \
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


def compile_scripts(client: zbench.RespClient, announce: bool = True) -> dict[str, tuple[str, str]]:
    """SCRIPT LOAD every idiom (once, before timing) and return {label: (prefix, sha)}."""
    if announce:
        print("Compiling scripts (SCRIPT LOAD) before benchmarking:")
    loaded: dict[str, tuple[str, str]] = {}
    for label, prefix, src in ENGINES:
        sha = client.command(f"{prefix}SCRIPT", "LOAD", src)
        sha = sha.decode() if isinstance(sha, bytes) else str(sha)
        if len(sha) != 40:
            raise RuntimeError(f"{label}: unexpected SCRIPT LOAD reply {sha!r}")
        loaded[label] = (prefix, sha)
        if announce:
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
    parser.add_argument("--unix-socket", type=str, default=None,
                        help="UDS path to use (default: a short auto path in /tmp; "
                             "sun_path is capped at ~104 bytes)")
    args = parser.parse_args(argv)

    # goblin-core binds either TCP or a Unix socket, not both, so run two servers
    # -- one per transport -- and drive each with its own client. The scripts are
    # SCRIPT LOAD-compiled on both (each has its own cache); the SHA1 digests are
    # identical (same source), so the approaches are the same on either.
    sock = args.unix_socket or f"/tmp/goblin-cad-{os.getpid()}.sock"
    server_tcp = zbench.start_goblin(binary=args.goblin_bin, rank_cache=False)
    server_uds = zbench.start_goblin(binary=args.goblin_bin, rank_cache=False,
                                     unix_socket=sock)
    try:
        tcp = zbench.RespClient("127.0.0.1", server_tcp.port)            # TCP loopback
        uds = zbench.RespClient("127.0.0.1", server_uds.port, unix_socket=sock)  # Unix socket
        keys = [f"cad:bench:{i}" for i in range(args.keys)]
        scripts = compile_scripts(tcp)
        compile_scripts(uds, announce=False)  # same digests, populate the UDS cache too
        approaches = build_approaches(scripts)

        print("\nVerifying correctness of each implementation (both transports)...")
        for a in approaches:
            verify(tcp, a["op"], TOKEN)
            verify(uds, a["op"], TOKEN)
        print("  all implementations agree (match -> 1 + deleted, mismatch -> 0).")

        depth = args.pipeline
        pop = args.populate_pipeline

        def med(fn: Callable[[], float]) -> float:
            return statistics.median([fn() for _ in range(args.rounds)]) * 1e6

        # Four numbers per implementation: sequential (one op at a time -- per-op
        # round trip) and pipelined (`depth` in flight -- round trip amortized),
        # each over TCP loopback and over the Unix socket. GET+DEL has no single
        # pipelined request (its DEL is conditional on the GET), so it is seq-only.
        print(f"\nBenchmarking {args.keys} ops/round, median of {args.rounds} rounds, "
              f"pipeline depth {depth}.\n")
        rows = []
        for a in approaches:
            op, cmds = a["op"], a["cmds"]
            tcp_seq = med(lambda op=op: measure_sequential(tcp, op, keys, TOKEN, pop))
            uds_seq = med(lambda op=op: measure_sequential(uds, op, keys, TOKEN, pop))
            if cmds is not None:
                tcp_pipe = med(lambda c=cmds: measure_pipelined(tcp, c, keys, TOKEN, pop, depth))
                uds_pipe = med(lambda c=cmds: measure_pipelined(uds, c, keys, TOKEN, pop, depth))
            else:
                tcp_pipe = uds_pipe = None
            rows.append((a["label"], a["rtt"], tcp_seq, uds_seq, tcp_pipe, uds_pipe))

        def cell(x: float | None) -> str:
            return f"{x:10.2f}" if x is not None else f"{'--':>10}"

        head = (f"{'implementation':32} {'RTT':>4}"
                f"{'TCP seq':>11}{'UDS seq':>11}{'TCP p' + str(depth):>11}{'UDS p' + str(depth):>11}")
        print(head)
        print("-" * len(head))
        for label, rtt, ts, us, tp, up in rows:
            print(f"{label:32} {rtt:>4}{cell(ts)}{cell(us)}{cell(tp)}{cell(up)}")
        print(f"\n  us/op. seq = one request at a time (per-op round trip); "
              f"p{depth} = {depth} requests in flight.\n"
              "  One Python connection (client-bound), so read the ratios across a row,\n"
              "  not absolute peak throughput. Lower is better.")
        tcp.close()
        uds.close()
    finally:
        server_tcp.stop()
        server_uds.stop()
        if not args.unix_socket:
            try:
                os.unlink(sock)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
