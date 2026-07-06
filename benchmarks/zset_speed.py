#!/usr/bin/env python3
"""Sorted-set throughput + depth-1 latency, mirroring hash_speed.py.

redis-benchmark (a C load generator, so a single pipelined connection measures
the server not the client) drives ZADD/ZSCORE/ZRANK/ZRANGE across Goblin Core,
Redis 7.2.4, Redis 8.8, and Valkey under the shared parity config. Run on a
quiet, dedicated host the per-op numbers are stable -- the reliable throughput
the softened Throughput section of BENCHMARKS.md promised.

Goblin Core runs with its default config (rank cache off), matching Redis's
out-of-the-box skiplist, so ZRANK is the fair O(log n)-vs-O(log n) comparison.
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402
    redis_benchmark_rps,
    start_goblin,
    start_redis,
)


def start_engine(kind: str, binary: Path):
    if kind == "goblin":
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off")
    if kind == "redis":
        return start_redis(binary)
    raise ValueError(f"unknown engine kind: {kind}")


@dataclass
class Speed:
    engine: str
    zadd_rps: float
    zscore_rps: float
    zrank_rps: float
    zrange_rps: float
    zscore_p1_latency_us: float


def bench(label: str, kind: str, binary: Path, rb_bin: Path, keyspace: int,
          requests: int, pipeline: int, range_size: int, timeout: float) -> Speed:
    server = start_engine(kind, binary)
    try:
        port = server.port
        key = "zspeed:big"

        # Populate so reads hit existing members; random scores give ZRANK a
        # realistic distribution.
        redis_benchmark_rps(
            rb_bin, port, ["zadd", key, "__rand_int__", "member:__rand_int__"],
            requests=keyspace * 2, pipeline=128, keyspace=keyspace, timeout=timeout)

        zadd = redis_benchmark_rps(
            rb_bin, port, ["zadd", key, "__rand_int__", "member:__rand_int__"],
            requests, pipeline, keyspace, timeout)
        zscore = redis_benchmark_rps(
            rb_bin, port, ["zscore", key, "member:__rand_int__"],
            requests, pipeline, keyspace, timeout)
        zrank = redis_benchmark_rps(
            rb_bin, port, ["zrank", key, "member:__rand_int__"],
            requests, pipeline, keyspace, timeout)
        zrange = redis_benchmark_rps(
            rb_bin, port, ["zrange", key, "0", str(range_size - 1)],
            requests, pipeline, 1, timeout)
        zscore_p1 = redis_benchmark_rps(
            rb_bin, port, ["zscore", key, "member:__rand_int__"],
            min(requests, 200_000), 1, keyspace, timeout)

        return Speed(label, zadd, zscore, zrank, zrange, 1e6 / zscore_p1)
    finally:
        server.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True,
                        metavar="LABEL:KIND:PATH",
                        help="engine (kind = goblin|redis); repeatable")
    parser.add_argument("--redis-benchmark", required=True, type=Path)
    parser.add_argument("--keyspace", type=int, default=1_000_000)
    parser.add_argument("--requests", type=int, default=2_000_000)
    parser.add_argument("--pipeline", type=int, default=16)
    parser.add_argument("--range-size", type=int, default=16)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=600.0)
    args = parser.parse_args()

    engines = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        engines.append((label, kind, Path(path)))

    results: list[Speed] = []
    for label, kind, binary in engines:
        runs = [bench(label, kind, binary, args.redis_benchmark, args.keyspace,
                      args.requests, args.pipeline, args.range_size, args.timeout)
                for _ in range(max(1, args.repeats))]
        best = Speed(
            engine=label,
            zadd_rps=max(r.zadd_rps for r in runs),
            zscore_rps=max(r.zscore_rps for r in runs),
            zrank_rps=max(r.zrank_rps for r in runs),
            zrange_rps=max(r.zrange_rps for r in runs),
            zscore_p1_latency_us=min(r.zscore_p1_latency_us for r in runs),
        )
        results.append(best)
        print(f"  {label:>10}: ZADD {best.zadd_rps/1000:7.1f}K  "
              f"ZSCORE {best.zscore_rps/1000:7.1f}K  "
              f"ZRANK {best.zrank_rps/1000:7.1f}K  "
              f"ZRANGE {best.zrange_rps/1000:7.1f}K  "
              f"ZSCORE-p1 {best.zscore_p1_latency_us:5.1f}us", file=sys.stderr)

    print("\n| metric | " + " | ".join(r.engine for r in results) + " |")
    print("| --- | " + " | ".join("---:" for _ in results) + " |")
    print("| ZADD ops/s | " + " | ".join(f"`{r.zadd_rps/1000:.0f}K`" for r in results) + " |")
    print("| ZSCORE ops/s | " + " | ".join(f"`{r.zscore_rps/1000:.0f}K`" for r in results) + " |")
    print("| ZRANK ops/s | " + " | ".join(f"`{r.zrank_rps/1000:.0f}K`" for r in results) + " |")
    print("| ZRANGE(16) ops/s | " + " | ".join(f"`{r.zrange_rps/1000:.0f}K`" for r in results) + " |")
    print("| ZSCORE p1 latency (µs) | " + " | ".join(f"`{r.zscore_p1_latency_us:.1f}`" for r in results) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
