#!/usr/bin/env python3
"""ZADD throughput vs connection count, driven by the C++ multi-threaded load
generator (zadd_bench) so the client is not the bottleneck -- one OS thread per
connection, unlike redis-benchmark's single event loop. Launches each engine via
the shared harness, warms the keyspace, then sweeps thread counts. TCP or --unix.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402
    free_unix_path,
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


def run_bench(bench, server, threads, keyspace, pipeline, duration):
    cmd = [str(bench), "--threads", str(threads), "--keyspace", str(keyspace),
           "--pipeline", str(pipeline), "--duration", str(duration)]
    if server.unix_socket:
        cmd += ["--unixsocket", server.unix_socket]
    else:
        cmd += ["--host", "127.0.0.1", "--port", str(server.port)]
    out = subprocess.run(cmd, capture_output=True, text=True,
                         timeout=duration + 60)
    if not out.stdout.strip():
        raise RuntimeError(f"zadd_bench produced no output: {out.stderr!r}")
    return float(out.stdout.strip())


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--engine", action="append", required=True,
                   help="label:kind:path")
    p.add_argument("--bench", required=True, type=Path,
                   help="compiled zadd_bench binary")
    p.add_argument("--threads", default="1,8,32,64",
                   help="comma-separated connection counts to sweep")
    p.add_argument("--keyspace", type=int, default=200_000)
    p.add_argument("--pipeline", type=int, default=1)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--warmup", type=float, default=2.0)
    p.add_argument("--unix", action="store_true")
    args = p.parse_args()

    thread_list = [int(x) for x in args.threads.split(",")]
    rows = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        unix_socket = free_unix_path() if args.unix else None
        server = start_engine(kind, Path(path), unix_socket)
        try:
            if args.warmup > 0:  # populate so every point measures a warm keyspace
                run_bench(args.bench, server, max(thread_list), args.keyspace,
                          args.pipeline, args.warmup)
            res = {}
            for t in thread_list:
                res[t] = run_bench(args.bench, server, t, args.keyspace,
                                   args.pipeline, args.duration)
                print(f"  {label:>12} {t:>3} conn: {res[t]:>12,.0f} zadd/s",
                      file=sys.stderr)
            rows.append((label, res))
        finally:
            server.stop()

    transport = "UDS" if args.unix else "TCP"
    print(f"\n**ZADD/s vs connections (C++ client, pipeline={args.pipeline}, "
          f"{transport}):**\n")
    print("| engine | " + " | ".join(f"{t} conn" for t in thread_list) + " |")
    print("| --- | " + " | ".join("---:" for _ in thread_list) + " |")
    for label, res in rows:
        cells = " | ".join(f"`{res[t]:,.0f}`" for t in thread_list)
        print(f"| {label} | {cells} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
