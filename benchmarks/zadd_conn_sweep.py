#!/usr/bin/env python3
"""ZADD throughput vs concurrent connection count, at pipeline=1 (one ZADD per
round trip per connection). Sweeps -c over a keyspace of tiny zsets so you can
see how many synchronous connections it takes to saturate each server, and the
ceiling once saturated.

Single-threaded servers (Goblin / Redis / Valkey) plateau at one core's worth;
Dragonfly here runs --proactor_threads=1 (the single-core comparison used
everywhere else), so it plateaus too -- with more proactors it would keep
scaling across cores. Use --unix to run everything over a Unix domain socket.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402
    free_unix_path,
    redis_benchmark_rps,
    start_dragonfly,
    start_goblin,
    start_redis,
)

ZADD = ["zadd", "z:__rand_int__", "__rand_int__", "m:__rand_int__"]


def start_engine(kind: str, binary: Path, unix_socket: str | None = None):
    if kind == "goblin":
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off",
                            unix_socket=unix_socket)
    if kind == "redis":
        return start_redis(binary, unix_socket=unix_socket)
    if kind == "dragonfly":
        return start_dragonfly(binary, unix_socket=unix_socket)
    raise ValueError(f"unknown engine kind: {kind}")


def sweep(label, kind, binary, rb, zsets, members, requests, clients_list,
          timeout, use_unix):
    unix_socket = free_unix_path() if use_unix else None
    server = start_engine(kind, binary, unix_socket)
    try:
        port, sock = server.port, server.unix_socket
        redis_benchmark_rps(rb, port, ZADD, zsets * members, 128, zsets,
                            timeout, unix_socket=sock)  # populate
        results = {}
        for c in clients_list:
            rps = redis_benchmark_rps(rb, port, ZADD, requests, 1, zsets,
                                      timeout, unix_socket=sock, clients=c)
            results[c] = rps
            print(f"  {label:>12} -c {c:>3}: {rps:>12,.0f} zadd/s",
                  file=sys.stderr)
        return (label, results)
    finally:
        server.stop()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--engine", action="append", required=True,
                   help="label:kind:path")
    p.add_argument("--redis-benchmark", required=True, type=Path)
    p.add_argument("--zsets", type=int, default=200_000)
    p.add_argument("--members", type=int, default=4)
    p.add_argument("--requests", type=int, default=1_000_000)
    p.add_argument("--clients", default="1,8,32,64",
                   help="comma-separated -c values to sweep")
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--unix", action="store_true",
                   help="run every engine + redis-benchmark over a Unix socket")
    args = p.parse_args()

    clients_list = [int(x) for x in args.clients.split(",")]
    rows = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        rows.append(sweep(label, kind, Path(path), args.redis_benchmark,
                          args.zsets, args.members, args.requests,
                          clients_list, args.timeout, args.unix))

    transport = "UDS" if args.unix else "TCP"
    print(f"\n**ZADD/s vs concurrent connections (pipeline=1, {transport}):**\n")
    print("| engine | " + " | ".join(f"-c {c}" for c in clients_list) + " |")
    print("| --- | " + " | ".join("---:" for _ in clients_list) + " |")
    for label, results in rows:
        cells = " | ".join(f"`{results[c]:,.0f}`" for c in clients_list)
        print(f"| {label} | {cells} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
