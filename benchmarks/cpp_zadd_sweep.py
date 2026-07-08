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
                   help="connection counts to sweep (fixed count if --pipelines)")
    p.add_argument("--pipelines", default=None,
                   help="if set, sweep these pipeline depths at a fixed --threads")
    p.add_argument("--keyspace", type=int, default=200_000)
    p.add_argument("--pipeline", type=int, default=1)
    p.add_argument("--duration", type=float, default=5.0)
    p.add_argument("--warmup", type=float, default=2.0)
    p.add_argument("--unix", action="store_true")
    args = p.parse_args()

    if args.pipelines:  # sweep pipeline depth at a fixed connection count
        conns = int(args.threads.split(",")[0])
        values = [int(x) for x in args.pipelines.split(",")]
        col = lambda v: f"pipe {v}"
        title = f"ZADD/s vs pipeline depth ({conns} conn)"

        def measure(server, v):
            return run_bench(args.bench, server, conns, args.keyspace, v,
                             args.duration)

        def warmup(server):
            run_bench(args.bench, server, conns, args.keyspace, max(values),
                      args.warmup)
    else:  # sweep connection count at a fixed pipeline depth
        values = [int(x) for x in args.threads.split(",")]
        col = lambda v: f"{v} conn"
        title = f"ZADD/s vs connections (pipeline={args.pipeline})"

        def measure(server, v):
            return run_bench(args.bench, server, v, args.keyspace,
                             args.pipeline, args.duration)

        def warmup(server):
            run_bench(args.bench, server, max(values), args.keyspace,
                      args.pipeline, args.warmup)

    rows = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        unix_socket = free_unix_path() if args.unix else None
        server = start_engine(kind, Path(path), unix_socket)
        try:
            if args.warmup > 0:  # warm the keyspace so every point is steady state
                warmup(server)
            res = {}
            for v in values:
                res[v] = measure(server, v)
                print(f"  {label:>12} {col(v):>9}: {res[v]:>12,.0f} zadd/s",
                      file=sys.stderr)
            rows.append((label, res))
        finally:
            server.stop()

    transport = "UDS" if args.unix else "TCP"
    print(f"\n**{title}, C++ client, {transport}:**\n")
    print("| engine | " + " | ".join(col(v) for v in values) + " |")
    print("| --- | " + " | ".join("---:" for _ in values) + " |")
    for label, res in rows:
        cells = " | ".join(f"`{res[v]:,.0f}`" for v in values)
        print(f"| {label} | {cells} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
