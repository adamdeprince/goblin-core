#!/usr/bin/env python3
"""Many-small-zsets memory benchmark ("small zset, large capacity").

Creates a large number of small sorted sets -- the workload where Goblin's fixed
per-collection arena chunk used to hurt most -- and compares resident memory per
zset across Goblin Core and the Redis-family engines (Redis, Valkey, Dragonfly).
Redis/Valkey keep a small zset in a compact listpack and Dragonfly in its own
small-set encoding, so this is the honest head-to-head for Goblin's growable,
page-aligned arena (a brand-new zset starts sub-page and grows).

Primary metric is process RSS delta / zset (fair and allocator-honest across
engines); engine-reported used_memory/zset is shown for the Redis family too.
Reuses zset_benchmark.py's transport/launch/RSS code.
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
    process_rss_mib,
    redis_used_memory_mib,
    start_dragonfly,
    start_goblin,
    start_redis,
)


def start_engine(kind: str, binary: Path):
    if kind == "goblin":
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off")
    if kind == "redis":
        return start_redis(binary)
    if kind == "dragonfly":
        return start_dragonfly(binary)
    raise ValueError(f"unknown engine kind: {kind}")


def zadd_commands(num_zsets: int, members_per_zset: int):
    # Distinct members per zset: each zset gets its OWN arena. Reusing identical
    # members across zsets would trip Goblin's shared-member-layer optimization
    # (one arena for all), which is not the many-small-collections workload.
    for z in range(num_zsets):
        command: list[object] = ["ZADD", f"z:{z:09d}"]
        for m in range(members_per_zset):
            command.append(float(m))
            command.append(f"z{z:09d}:m{m:03d}")
        yield command


@dataclass
class Row:
    engine: str
    num_zsets: int
    rss_bytes_per_zset: float
    used_bytes_per_zset: float | None
    rss_delta_mib: float


def bench_once(label: str, kind: str, binary: Path, num_zsets: int,
               members_per_zset: int, pipeline: int, settle: float) -> Row:
    server = start_engine(kind, binary)
    try:
        client = RespClient("127.0.0.1", server.port, timeout=600.0)
        try:
            rss_base = process_rss_mib(server.process.pid)
            client.pipeline(zadd_commands(num_zsets, members_per_zset), pipeline)
            time.sleep(settle)
            rss_after = process_rss_mib(server.process.pid)
            rss_delta = max(0.0, rss_after - rss_base)
            used = None if kind == "goblin" else redis_used_memory_mib(client)
            return Row(
                engine=label,
                num_zsets=num_zsets,
                rss_bytes_per_zset=rss_delta * 1024.0 * 1024.0 / num_zsets,
                used_bytes_per_zset=(used * 1024.0 * 1024.0 / num_zsets
                                     if used is not None else None),
                rss_delta_mib=rss_delta,
            )
        finally:
            client.close()
    finally:
        server.stop()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True,
                        metavar="LABEL:KIND:PATH")
    parser.add_argument("--counts", default="10000,50000,100000,250000")
    parser.add_argument("--members-per-zset", type=int, default=4)
    parser.add_argument("--pipeline", type=int, default=512)
    parser.add_argument("--settle-seconds", type=float, default=0.3)
    parser.add_argument("--repeats", type=int, default=2)
    args = parser.parse_args()

    engines = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        engines.append((label, kind, Path(path)))
    counts = [int(s) for s in args.counts.split(",") if s]

    results: list[Row] = []
    for n in counts:
        for label, kind, binary in engines:
            runs = [bench_once(label, kind, binary, n, args.members_per_zset,
                               args.pipeline, args.settle_seconds)
                    for _ in range(max(1, args.repeats))]
            best = min(runs, key=lambda r: r.rss_bytes_per_zset)
            results.append(best)
            print(f"  {label:>10} {n:>9,} zsets x{args.members_per_zset}: "
                  f"{best.rss_bytes_per_zset:8.1f} B/zset RSS "
                  f"({best.rss_delta_mib:7.1f} MiB total), "
                  f"{(best.used_bytes_per_zset or 0):8.1f} B/zset used",
                  file=sys.stderr)

    labels = [label for label, _, _ in engines]
    print(f"\n**RSS bytes per zset ({args.members_per_zset} members each):**\n")
    print("| zsets | " + " | ".join(labels) + " |")
    print("| --- | " + " | ".join("---:" for _ in labels) + " |")
    for n in counts:
        cells = []
        for label in labels:
            row = next((r for r in results
                        if r.engine == label and r.num_zsets == n), None)
            cells.append(f"`{row.rss_bytes_per_zset:.0f}`" if row else "-")
        print(f"| {n:,} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
