#!/usr/bin/env python3
"""Depth-1 latency, PING concurrency, and write-path tail on one host.

Per engine: the C probe (write_tail_latency) in --ping mode (pure round trip)
and ZADD mode (write-path tail), plus redis-benchmark PING at 1/50/500 clients.
The client side is pinned to a fixed core; on an idle 128-core host the server
gets its own. Reuses zset_benchmark.py's engine launch.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import start_goblin, start_redis  # noqa: E402


def start_engine(kind: str, binary: Path):
    if kind == "goblin":
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off")
    if kind == "redis":
        return start_redis(binary)
    raise ValueError(kind)


def probe(pin_core: int, wtl: Path, port: int, n: int, ping: bool):
    cmd = ["taskset", "-c", str(pin_core), str(wtl), "127.0.0.1", str(port), str(n)]
    if ping:
        cmd.append("--ping")
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=600).stdout
    m = re.search(r"p50=([\d.]+) p90=([\d.]+) p99=([\d.]+) p99\.9=([\d.]+) "
                  r"p99\.99=([\d.]+) max=([\d.]+)", out)
    keys = ["p50", "p90", "p99", "p999", "p9999", "max"]
    return {k: float(v) for k, v in zip(keys, m.groups())} if m else None


def rb_ping(pin_core: int, rb: Path, port: int, clients: int, n: int):
    out = subprocess.run(
        ["taskset", "-c", str(pin_core), str(rb), "-h", "127.0.0.1", "-p", str(port),
         "-n", str(n), "-c", str(clients), "-P", "1", "-q", "ping"],
        capture_output=True, text=True, timeout=600).stdout
    rps = re.search(r"([\d.]+) requests per second", out)
    p50 = re.search(r"p50=([\d.]+) msec", out)
    return (float(rps.group(1)) if rps else None,
            float(p50.group(1)) if p50 else None)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True,
                        metavar="LABEL:KIND:PATH")
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--redis-benchmark", required=True, type=Path)
    parser.add_argument("--pin-core", type=int, default=4)
    parser.add_argument("--server-core", type=int, default=2)
    parser.add_argument("--tail-ops", type=int, default=1_000_000)
    parser.add_argument("--ping-ops", type=int, default=500_000)
    parser.add_argument("--rb-ops", type=int, default=500_000)
    args = parser.parse_args()

    engines = [tuple(spec.split(":", 2)) for spec in args.engine]
    for label, kind, path in engines:
        server = start_engine(kind, Path(path))
        try:
            # Pin the (single-threaded) server to its own core, separate from
            # the client, so depth-1 latency isn't dominated by migration.
            subprocess.run(["taskset", "-pc", str(args.server_core),
                            str(server.process.pid)], capture_output=True)
            port = server.port
            ping = probe(args.pin_core, args.probe, port, args.ping_ops, True)
            conc = {c: rb_ping(args.pin_core, args.redis_benchmark, port, c, args.rb_ops)
                    for c in (1, 50, 500)}
            tail = probe(args.pin_core, args.probe, port, args.tail_ops, False)
            print(f"{label:>10}: ping-p50={ping['p50']:.1f}us p99={ping['p99']:.1f}us | "
                  f"c1 {conc[1][0]/1000:.0f}K/{conc[1][1]}ms  "
                  f"c50 {conc[50][0]/1000:.0f}K/{conc[50][1]}ms  "
                  f"c500 {conc[500][0]/1000:.0f}K/{conc[500][1]}ms | "
                  f"ZADD-tail p50={tail['p50']:.1f} p99={tail['p99']:.1f} "
                  f"p999={tail['p999']:.1f} max={tail['max']:.0f}us")
        finally:
            server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
