#!/usr/bin/env python3
"""Small-zset speed: ZADD / ZSCORE / ZRANGE throughput over MANY small zsets.

Unlike zset_speed.py (one big zset), this builds a keyspace of tiny zsets
(~`members` each, so they live in Goblin's listpack / Redis's listpack / etc.)
and measures ops/sec via redis-benchmark against each engine -- the honest
head-to-head for the small-zset write/read paths the memory work touched.
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


def start_engine(kind: str, binary: Path, unix_socket: str | None = None):
    if kind == "goblin":
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off",
                            unix_socket=unix_socket)
    if kind == "redis":
        return start_redis(binary, unix_socket=unix_socket)
    if kind == "dragonfly":
        return start_dragonfly(binary, unix_socket=unix_socket)
    raise ValueError(f"unknown engine kind: {kind}")


def bench(label, kind, binary, rb, zsets, members, requests, pipeline, timeout,
          use_unix=False):
    unix_socket = free_unix_path() if use_unix else None
    server = start_engine(kind, binary, unix_socket)
    try:
        port = server.port
        sock = server.unix_socket
        # Populate ~`members` entries into each of `zsets` tiny zsets.
        redis_benchmark_rps(
            rb, port, ["zadd", "z:__rand_int__", "__rand_int__", "m:__rand_int__"],
            zsets * members, 128, zsets, timeout, unix_socket=sock)
        zadd = redis_benchmark_rps(
            rb, port, ["zadd", "z:__rand_int__", "__rand_int__", "m:__rand_int__"],
            requests, pipeline, zsets, timeout, unix_socket=sock)
        # ZSCORE on a random member: mostly the listpack miss-scan path.
        zscore = redis_benchmark_rps(
            rb, port, ["zscore", "z:__rand_int__", "m:__rand_int__"],
            requests, pipeline, zsets, timeout, unix_socket=sock)
        # ZRANGE 0 -1: reads the whole tiny zset (listpack iteration).
        zrange = redis_benchmark_rps(
            rb, port, ["zrange", "z:__rand_int__", "0", "-1"],
            requests, pipeline, zsets, timeout, unix_socket=sock)
        return (label, zadd, zscore, zrange)
    finally:
        server.stop()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--engine", action="append", required=True,
                   help="label:kind:path")
    p.add_argument("--redis-benchmark", required=True, type=Path)
    p.add_argument("--zsets", type=int, default=200_000)
    p.add_argument("--members", type=int, default=8)
    p.add_argument("--requests", type=int, default=2_000_000)
    p.add_argument("--pipeline", type=int, default=16)
    p.add_argument("--timeout", type=float, default=600.0)
    p.add_argument("--unix", action="store_true",
                   help="run every engine + redis-benchmark over a Unix domain socket")
    args = p.parse_args()

    rows = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        r = bench(label, kind, Path(path), args.redis_benchmark, args.zsets,
                  args.members, args.requests, args.pipeline, args.timeout,
                  use_unix=args.unix)
        rows.append(r)
        print(f"  {r[0]:>10}: zadd {r[1]:>12,.0f}  zscore {r[2]:>12,.0f}  "
              f"zrange {r[3]:>12,.0f} rps", file=sys.stderr)

    print(f"\n**Small-zset throughput (ops/sec, ~{args.members} members/zset, "
          f"{args.zsets:,} zsets, pipeline {args.pipeline}):**\n")
    print("| engine | ZADD | ZSCORE | ZRANGE 0 -1 |")
    print("| --- | ---: | ---: | ---: |")
    for label, zadd, zscore, zrange in rows:
        print(f"| {label} | `{zadd:,.0f}` | `{zscore:,.0f}` | `{zrange:,.0f}` |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
