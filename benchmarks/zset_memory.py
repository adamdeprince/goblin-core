#!/usr/bin/env python3
"""Sorted-set memory sweep, mirroring hash_benchmark.py.

Loads N members into one sorted set with ZADD (scattered scores), runs
GOBLIN.OPTIMIZE on Goblin Core, and measures RSS/member, engine-reported
memory/member, and the fragmentation ratio across Goblin Core, Redis 7.2.4,
Redis 8.8, and Valkey under the shared parity config. Reuses zset_benchmark.py's
transport/launch/RSS code so it matches the hash memory benchmark exactly.
"""
from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402
    RespClient,
    goblin_memory_stats,
    process_rss_mib,
    redis_used_memory_mib,
    start_goblin,
    start_redis,
)


def member_for(i: int) -> str:
    return f"member:{i:010d}"


def score_for(i: int) -> float:
    # Deterministic but scattered, so the sorted order differs from insert order.
    return float((i * 1_103_515_245 + 12_345) & 0xFFFFFFFF)


def zadd_commands(count: int, key: str, batch: int):
    for start in range(0, count, batch):
        command: list[object] = ["ZADD", key]
        for i in range(start, min(start + batch, count)):
            command.append(score_for(i))
            command.append(member_for(i))
        yield command


@dataclass
class Row:
    engine: str
    members: int
    rss_bytes_per_member: float
    used_bytes_per_member: float | None
    fragmentation: float | None


def start_engine(kind: str, binary: Path):
    if kind == "goblin":
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off")
    if kind == "redis":
        return start_redis(binary)
    raise ValueError(f"unknown engine kind: {kind}")


def bench_once(label: str, kind: str, binary: Path, members: int, batch: int,
               pipeline: int, density: str | None, settle: float) -> Row:
    server = start_engine(kind, binary)
    try:
        client = RespClient("127.0.0.1", server.port, timeout=600.0)
        try:
            key = f"zbench:{label}"
            rss_base = process_rss_mib(server.process.pid)
            client.pipeline(zadd_commands(members, key, batch), pipeline)
            if kind == "goblin" and density is not None:
                client.command("GOBLIN.OPTIMIZE", key, density)
            time.sleep(settle)
            rss_after = process_rss_mib(server.process.pid)
            rss_delta = rss_after - rss_base
            if kind == "goblin":
                stats = goblin_memory_stats(client, key)
                used = (stats["total_allocated_bytes"] / (1024.0 * 1024.0)
                        if stats and "total_allocated_bytes" in stats else None)
            else:
                used = redis_used_memory_mib(client)
            return Row(
                engine=label,
                members=members,
                rss_bytes_per_member=rss_delta * 1024.0 * 1024.0 / members,
                used_bytes_per_member=(used * 1024.0 * 1024.0 / members
                                       if used is not None else None),
                fragmentation=(rss_after / used if used not in (None, 0) else None),
            )
        finally:
            client.close()
    finally:
        server.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True,
                        metavar="LABEL:KIND:PATH")
    parser.add_argument("--sizes", default="250000,500000,1000000,2000000,4000000")
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--optimize-density", default="0.97")
    parser.add_argument("--settle-seconds", type=float, default=0.2)
    parser.add_argument("--repeats", type=int, default=2)
    args = parser.parse_args()

    engines = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        engines.append((label, kind, Path(path)))
    sizes = [int(s) for s in args.sizes.split(",") if s]

    results: list[Row] = []
    for members in sizes:
        for label, kind, binary in engines:
            runs = [bench_once(label, kind, binary, members, args.zadd_batch,
                               args.pipeline, args.optimize_density,
                               args.settle_seconds)
                    for _ in range(max(1, args.repeats))]
            best = min(runs, key=lambda r: r.rss_bytes_per_member)
            results.append(best)
            print(f"  {label:>10} {members:>9,} members: "
                  f"{best.rss_bytes_per_member:6.1f} B/member RSS, "
                  f"{(best.used_bytes_per_member or 0):6.1f} B/member used, "
                  f"frag {best.fragmentation or float('nan'):.2f}", file=sys.stderr)

    labels = [label for label, _, _ in engines]
    print("\n**RSS bytes/member:**\n")
    print("| members | " + " | ".join(labels) + " |")
    print("| --- | " + " | ".join("---:" for _ in labels) + " |")
    for members in sizes:
        cells = []
        for label in labels:
            row = next((r for r in results
                        if r.engine == label and r.members == members), None)
            cells.append(f"`{row.rss_bytes_per_member:.1f}`" if row else "-")
        print(f"| {members:,} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
