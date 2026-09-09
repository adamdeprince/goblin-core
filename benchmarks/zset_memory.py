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
    start_dragonfly,
    start_goblin,
    start_redis,
)


PACKED_ZSET_KINDS = (
    "INT32_FLOAT32",
    "INT32_FLOAT64",
    "INT64_FLOAT32",
    "INT64_FLOAT64",
    "UUID_FLOAT32",
    "UUID_FLOAT64",
)


def member_for(i: int, packed_zset_kind: str | None = None) -> str:
    if packed_zset_kind is not None:
        if packed_zset_kind.startswith(("INT32_", "INT64_")):
            return str(i)
        # Keep the UUID form deterministic if UUID variants are benchmarked in
        # the future. The current packed campaign intentionally runs only ints.
        return f"00000000-0000-0000-0000-{i:012x}"
    return f"member:{i:010d}"


def score_for(i: int) -> float:
    # Deterministic but scattered, so the sorted order differs from insert order.
    return float((i * 1_103_515_245 + 12_345) & 0xFFFFFFFF)


def zadd_commands(count: int, key: str, batch: int,
                  packed_zset_kind: str | None = None):
    for start in range(0, count, batch):
        command: list[object] = ["ZADD", key]
        for i in range(start, min(start + batch, count)):
            command.append(score_for(i))
            command.append(member_for(i, packed_zset_kind))
        yield command


@dataclass
class Row:
    engine: str
    members: int
    rss_bytes_per_member: float
    used_bytes_per_member: float | None
    fragmentation: float | None


def start_engine(kind: str, binary: Path,
                 packed_zset_kind: str | None = None,
                 packed_zset_merge_exponent: float | None = None):
    if kind == "goblin":
        extra_args: tuple[str, ...] = ()
        if packed_zset_kind is not None:
            implementation = "packed-" + packed_zset_kind.lower().replace(
                "_", "-")
            extra_args += ("--zset-implementation", implementation)
        if packed_zset_merge_exponent is not None:
            extra_args += (
                "--packed-zset-merge-exponent",
                format(packed_zset_merge_exponent, ".17g"),
            )
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off",
                            extra_args=extra_args)
    if kind == "redis":
        return start_redis(binary)
    if kind == "dragonfly":
        return start_dragonfly(binary)
    raise ValueError(f"unknown engine kind: {kind}")


def bench_once(label: str, kind: str, binary: Path, members: int, batch: int,
               pipeline: int, density: str | None, settle: float,
               packed_zset_kind: str | None,
               packed_zset_merge_exponent: float) -> Row:
    server = start_engine(
        kind,
        binary,
        packed_zset_kind,
        packed_zset_merge_exponent if packed_zset_kind is not None else None,
    )
    try:
        client = RespClient("127.0.0.1", server.port, timeout=600.0)
        try:
            key = f"zbench:{label}"
            rss_base = process_rss_mib(server.process.pid)
            client.pipeline(
                zadd_commands(members, key, batch, packed_zset_kind), pipeline)
            if kind == "goblin" and density is not None:
                client.command("GOBLIN.OPTIMIZE", key, density)
            if packed_zset_kind is not None:
                cardinality = client.command("ZCARD", key)
                if cardinality != members:
                    raise RuntimeError(
                        f"{label}: expected {members} members, got {cardinality}"
                    )
            time.sleep(settle)
            rss_after = process_rss_mib(server.process.pid)
            rss_delta = rss_after - rss_base
            if kind == "goblin":
                stats = goblin_memory_stats(client, key)
                allocated = (
                    stats.get("total_allocated_bytes") if stats is not None
                    else None
                )
                used = (allocated / (1024.0 * 1024.0)
                        if isinstance(allocated, int) else None)
                if packed_zset_kind is not None:
                    if stats is None:
                        raise RuntimeError(f"{label}: GOBLIN.MEMORY failed")
                    expected_exponent = format(
                        packed_zset_merge_exponent, ".17g")
                    actual_exponent = str(stats.get("merge_exponent"))
                    if stats.get("representation") != packed_zset_kind:
                        raise RuntimeError(
                            f"{label}: expected representation "
                            f"{packed_zset_kind}, got "
                            f"{stats.get('representation')}"
                        )
                    if float(actual_exponent) != packed_zset_merge_exponent:
                        raise RuntimeError(
                            f"{label}: expected merge exponent "
                            f"{expected_exponent}, got {actual_exponent}"
                        )
                    if stats.get("member_count") != members:
                        raise RuntimeError(
                            f"{label}: memory stats report "
                            f"{stats.get('member_count')} members, expected "
                            f"{members}"
                        )
                    if stats.get("sorted_entries") != members or \
                            stats.get("unsorted_entries") != 0:
                        raise RuntimeError(
                            f"{label}: optimize did not leave a canonical "
                            f"sorted run: {stats}"
                        )
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
    parser.add_argument("--packed-zset-kind", choices=PACKED_ZSET_KINDS,
                        help="Select the matching packed implementation as "
                             "the server's ordinary-Z* default")
    parser.add_argument("--packed-zset-merge-exponent", type=float,
                        default=0.5, metavar="K",
                        help="Packed per-leaf dirty-tail exponent "
                             "(default: 0.5)")
    args = parser.parse_args()

    engines = []
    for spec in args.engine:
        label, kind, path = spec.split(":", 2)
        engines.append((label, kind, Path(path)))
    if args.packed_zset_kind is not None and any(
            kind != "goblin" for _, kind, _ in engines):
        parser.error("--packed-zset-kind can only be used with goblin engines")
    if not 0.0 <= args.packed_zset_merge_exponent <= 1.0:
        parser.error("--packed-zset-merge-exponent must be in [0, 1]")
    sizes = [int(s) for s in args.sizes.split(",") if s]

    results: list[Row] = []
    for members in sizes:
        for label, kind, binary in engines:
            runs = [bench_once(label, kind, binary, members, args.zadd_batch,
                               args.pipeline, args.optimize_density,
                               args.settle_seconds, args.packed_zset_kind,
                               args.packed_zset_merge_exponent)
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
