#!/usr/bin/env python3
"""Hash throughput + depth-1 latency, mirroring the zset speed methodology.

Drives each engine with redis-benchmark (a C load generator, so a single
pipelined connection measures the server, not the client) across Goblin Core,
Redis 7.2.4, Redis 8.8, and Valkey under the shared parity config. Reports HSET
(write), HGET (point read), and HGETALL (object fetch) throughput at pipeline
depth 16, plus the depth-1 HGET round-trip latency.
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402
    RespClient,
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
    hset_rps: float
    hget_rps: float
    hgetall_rps: float
    hget_p1_latency_us: float


def bench(label: str, kind: str, binary: Path, rb_bin: Path, keyspace: int,
          requests: int, pipeline: int, small_fields: int, timeout: float) -> Speed:
    server = start_engine(kind, binary)
    try:
        port = server.port
        big, small = "hspeed:big", "hspeed:small"

        # Populate the big hash so HGET hits existing fields (redis-benchmark's
        # __rand_int__ over the keyspace).
        redis_benchmark_rps(
            rb_bin, port, ["hset", big, "field:__rand_int__", "value:__rand_int__"],
            requests=keyspace * 2, pipeline=128, keyspace=keyspace, timeout=timeout)

        # A small object-shaped hash for HGETALL.
        client = RespClient("127.0.0.1", port, timeout=timeout)
        setup = ["HSET", small]
        for i in range(small_fields):
            setup += [f"field:{i}", f"value:{i}"]
        client.command(*setup)
        client.close()

        hset_rps = redis_benchmark_rps(
            rb_bin, port, ["hset", big, "field:__rand_int__", "value:__rand_int__"],
            requests, pipeline, keyspace, timeout)
        hget_rps = redis_benchmark_rps(
            rb_bin, port, ["hget", big, "field:__rand_int__"],
            requests, pipeline, keyspace, timeout)
        hgetall_rps = redis_benchmark_rps(
            rb_bin, port, ["hgetall", small], requests, pipeline, 1, timeout)
        # Depth-1 round trip: at -c 1 -P 1 the mean latency is 1 / rps.
        hget_p1_rps = redis_benchmark_rps(
            rb_bin, port, ["hget", big, "field:__rand_int__"],
            min(requests, 200_000), 1, keyspace, timeout)

        return Speed(label, hset_rps, hget_rps, hgetall_rps, 1e6 / hget_p1_rps)
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
    parser.add_argument("--small-fields", type=int, default=20)
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
                      args.requests, args.pipeline, args.small_fields, args.timeout)
                for _ in range(max(1, args.repeats))]
        # Best-of-repeats per metric (least scheduling noise on a quiet host).
        best = Speed(
            engine=label,
            hset_rps=max(r.hset_rps for r in runs),
            hget_rps=max(r.hget_rps for r in runs),
            hgetall_rps=max(r.hgetall_rps for r in runs),
            hget_p1_latency_us=min(r.hget_p1_latency_us for r in runs),
        )
        results.append(best)
        print(f"  {label:>10}: HSET {best.hset_rps/1000:7.1f}K  "
              f"HGET {best.hget_rps/1000:7.1f}K  "
              f"HGETALL {best.hgetall_rps/1000:7.1f}K  "
              f"HGET-p1 {best.hget_p1_latency_us:5.1f}us", file=sys.stderr)

    print("\n| metric | " + " | ".join(r.engine for r in results) + " |")
    print("| --- | " + " | ".join("---:" for _ in results) + " |")
    print("| HSET ops/s | " + " | ".join(f"`{r.hset_rps/1000:.0f}K`" for r in results) + " |")
    print("| HGET ops/s | " + " | ".join(f"`{r.hget_rps/1000:.0f}K`" for r in results) + " |")
    print("| HGETALL ops/s | " + " | ".join(f"`{r.hgetall_rps/1000:.0f}K`" for r in results) + " |")
    print("| HGET p1 latency (µs) | " + " | ".join(f"`{r.hget_p1_latency_us:.1f}`" for r in results) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
