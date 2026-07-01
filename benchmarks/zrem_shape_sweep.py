#!/usr/bin/env python3
"""Sweep ZREM behavior across set sizes and removal fractions."""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

import zset_benchmark as zbench


ROOT = Path(__file__).resolve().parents[1]


@dataclass
class ZRemShapeResult:
    target: str
    members: int
    remove_fraction: float
    remove_order: str
    remove_members: int
    score_shape: str
    pipeline_depth: int
    zadd_batch: int
    zrem_batch: int
    zadd_commands: int
    zadd_seconds: float
    zadd_members_per_second: float
    zadd_commands_per_second: float
    zrem_commands: int
    zrem_seconds: float
    zrem_members_per_second: float
    zrem_commands_per_second: float
    rss_baseline_mib: float
    rss_after_load_mib: float
    rss_after_remove_mib: float
    redis_used_memory_load_mib: float | None
    redis_used_memory_final_mib: float | None
    goblin_memory_load: dict[str, int] | None
    goblin_memory_final: dict[str, int] | None


def fmt_int(value: float | int | None) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):,.0f}"


def fmt_float(value: float | int | None, digits: int = 2) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):,.{digits}f}"


def fmt_ratio(value: float | int | None) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):.2f}x"


def mib(stats: dict[str, int] | None, name: str) -> float | None:
    if stats is None:
        return None
    return stats.get(name, 0) / (1024.0 * 1024.0)


def stat(stats: dict[str, int] | None, name: str) -> int | None:
    if stats is None:
        return None
    return stats.get(name)


def result_path(path: Path, report: Path) -> str:
    path = path.resolve()
    parent = report.resolve().parent
    try:
        return str(path.relative_to(parent))
    except ValueError:
        return str(path)


def remove_count(members: int, fraction: float) -> int:
    return min(members, max(0, int(round(members * fraction))))


def remove_ids(load_ids: Sequence[int],
               count: int,
               order: str,
               seed: int) -> list[int]:
    if count <= 0:
        return []
    if order == "load-prefix":
        return list(load_ids[:count])
    if order == "load-suffix":
        return list(load_ids[-count:])
    ids = list(load_ids)
    import random

    random.Random(seed).shuffle(ids)
    return ids[:count]


def start_targets(args: argparse.Namespace) -> list[zbench.ServerProcess]:
    servers: list[zbench.ServerProcess] = []
    if args.target in ("both", "goblin"):
        servers.append(
            zbench.start_goblin(
                args.goblin_bin,
                args.goblin_rank_cache,
                score_string_cache=args.goblin_score_string_cache,
            )
        )
    if args.target in ("both", "redis"):
        servers.append(zbench.start_redis(args.redis_server))
    return servers


def run_target(server: zbench.ServerProcess,
               args: argparse.Namespace,
               members: int,
               fraction: float,
               order: str) -> ZRemShapeResult:
    client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
    try:
        key = f"zremshape:{os.getpid()}:{server.name}:{order}:{members}:{fraction}"
        load_ids = zbench.shuffled_ids(members, args.seed)
        removed = remove_count(members, fraction)
        ids_to_remove = remove_ids(
            load_ids,
            removed,
            order,
            args.seed + 17,
        )

        rss_baseline = zbench.process_rss_mib(server.process.pid)
        zadd_command_count, zadd_seconds = zbench.time_pipeline(
            client,
            zbench.zadd_commands(
                load_ids,
                key,
                args.zadd_batch,
                args.score_shape,
                args.seed,
            ),
            args.pipeline,
        )
        time.sleep(args.settle_seconds)
        rss_after_load = zbench.process_rss_mib(server.process.pid)
        used_load = (
            zbench.redis_used_memory_mib(client) if server.name == "redis" else None
        )
        goblin_load = (
            zbench.goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        zrem_command_count, zrem_seconds = zbench.time_pipeline(
            client,
            zbench.zrem_commands(ids_to_remove, key, args.zrem_batch),
            args.pipeline,
        )
        time.sleep(args.settle_seconds)
        rss_after_remove = zbench.process_rss_mib(server.process.pid)
        used_final = (
            zbench.redis_used_memory_mib(client) if server.name == "redis" else None
        )
        goblin_final = (
            zbench.goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        return ZRemShapeResult(
            target=server.name,
            members=members,
            remove_fraction=fraction,
            remove_order=order,
            remove_members=removed,
            score_shape=args.score_shape,
            pipeline_depth=args.pipeline,
            zadd_batch=args.zadd_batch,
            zrem_batch=args.zrem_batch,
            zadd_commands=zadd_command_count,
            zadd_seconds=zadd_seconds,
            zadd_members_per_second=members / zadd_seconds if zadd_seconds > 0 else 0.0,
            zadd_commands_per_second=(
                zadd_command_count / zadd_seconds if zadd_seconds > 0 else 0.0
            ),
            zrem_commands=zrem_command_count,
            zrem_seconds=zrem_seconds,
            zrem_members_per_second=(
                removed / zrem_seconds if zrem_seconds > 0 else 0.0
            ),
            zrem_commands_per_second=(
                zrem_command_count / zrem_seconds if zrem_seconds > 0 else 0.0
            ),
            rss_baseline_mib=rss_baseline,
            rss_after_load_mib=rss_after_load,
            rss_after_remove_mib=rss_after_remove,
            redis_used_memory_load_mib=used_load,
            redis_used_memory_final_mib=used_final,
            goblin_memory_load=goblin_load,
            goblin_memory_final=goblin_final,
        )
    finally:
        client.close()


def run_case(args: argparse.Namespace,
             members: int,
             fraction: float,
             order: str) -> list[ZRemShapeResult]:
    servers = start_targets(args)
    results: list[ZRemShapeResult] = []
    try:
        for server in servers:
            results.append(run_target(server, args, members, fraction, order))
    finally:
        for server in servers:
            server.stop()
    return results


def write_json(args: argparse.Namespace, results: Sequence[ZRemShapeResult]) -> None:
    payload = {
        "config": {
            "target": args.target,
            "member_counts": args.member_counts,
            "remove_fractions": args.remove_fractions,
            "remove_orders": args.remove_orders,
            "score_shape": args.score_shape,
            "pipeline": args.pipeline,
            "zadd_batch": args.zadd_batch,
            "zrem_batch": args.zrem_batch,
            "goblin_rank_cache": args.goblin_rank_cache,
            "goblin_score_string_cache": args.goblin_score_string_cache,
            "seed": args.seed,
        },
        "results": [asdict(row) for row in results],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")


def rows_by_key(
    results: Sequence[ZRemShapeResult],
) -> dict[tuple[str, int, float, str], ZRemShapeResult]:
    return {
        (row.remove_order, row.members, row.remove_fraction, row.target): row
        for row in results
    }


def write_report(args: argparse.Namespace, results: Sequence[ZRemShapeResult]) -> None:
    by_key = rows_by_key(results)
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [
        "# Goblin Core ZREM Shape Sweep",
        "",
        f"Generated: {generated_at}.",
        "",
        "This benchmark loads one zset per target, removes a configured fraction "
        "of members, and records throughput plus memory before and after removal.",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- member counts: `{', '.join(f'{count:,}' for count in args.member_counts)}`",
        f"- remove fractions: `{', '.join(str(value) for value in args.remove_fractions)}`",
        f"- remove orders: `{', '.join(args.remove_orders)}`",
        f"- score shape: `{args.score_shape}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- zadd batch size: `{args.zadd_batch}`",
        f"- zrem batch size: `{args.zrem_batch}`",
        f"- Goblin rank cache: `{args.goblin_rank_cache}`",
        f"- Goblin score string cache: `{args.goblin_score_string_cache}`",
        f"- seed: `{args.seed}`",
        "",
        f"Source data: `{result_path(args.output_json, args.report)}`",
        "",
        "## ZREM Throughput",
        "",
        "| Remove order | Members | Remove % | Removed | Goblin members/sec | Redis members/sec | Goblin / Redis |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for order in args.remove_orders:
        for members in args.member_counts:
            for fraction in args.remove_fractions:
                goblin = by_key.get((order, members, fraction, "goblin"))
                redis = by_key.get((order, members, fraction, "redis"))
                goblin_rate = None if goblin is None else goblin.zrem_members_per_second
                redis_rate = None if redis is None else redis.zrem_members_per_second
                ratio = None if goblin_rate is None or not redis_rate else goblin_rate / redis_rate
                removed = remove_count(members, fraction)
                lines.append(
                    f"| `{order}` | {members:,} | {fraction * 100:.0f}% | {removed:,} | "
                    f"{fmt_int(goblin_rate)} | {fmt_int(redis_rate)} | {fmt_ratio(ratio)} |"
                )

    lines.extend([
        "",
        "## ZADD Load Throughput",
        "",
        "| Remove order | Members | Remove % | Goblin members/sec | Redis members/sec | Goblin / Redis |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ])
    for order in args.remove_orders:
        for members in args.member_counts:
            for fraction in args.remove_fractions:
                goblin = by_key.get((order, members, fraction, "goblin"))
                redis = by_key.get((order, members, fraction, "redis"))
                goblin_rate = None if goblin is None else goblin.zadd_members_per_second
                redis_rate = None if redis is None else redis.zadd_members_per_second
                ratio = None if goblin_rate is None or not redis_rate else goblin_rate / redis_rate
                lines.append(
                    f"| `{order}` | {members:,} | {fraction * 100:.0f}% | {fmt_int(goblin_rate)} | "
                    f"{fmt_int(redis_rate)} | {fmt_ratio(ratio)} |"
                )

    lines.extend([
        "",
        "## Process Memory",
        "",
        "| Remove order | Members | Remove % | Target | Load RSS MiB | Final RSS MiB | Redis load used MiB | Redis final used MiB |",
        "| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: |",
    ])
    for order in args.remove_orders:
        for members in args.member_counts:
            for fraction in args.remove_fractions:
                for target in ("goblin", "redis"):
                    row = by_key.get((order, members, fraction, target))
                    if row is None:
                        continue
                    lines.append(
                        f"| `{order}` | {members:,} | {fraction * 100:.0f}% | `{target}` | "
                        f"{fmt_float(row.rss_after_load_mib)} | "
                        f"{fmt_float(row.rss_after_remove_mib)} | "
                        f"{fmt_float(row.redis_used_memory_load_mib)} | "
                        f"{fmt_float(row.redis_used_memory_final_mib)} |"
                    )

    lines.extend([
        "",
        "## Goblin Internals",
        "",
        "| Remove order | Members | Remove % | Phase | member_count | tombstones | score_entries | score_blocks | score_capacity | total alloc MiB | score index MiB | member index MiB |",
        "| --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    for order in args.remove_orders:
        for members in args.member_counts:
            for fraction in args.remove_fractions:
                row = by_key.get((order, members, fraction, "goblin"))
                if row is None:
                    continue
                for phase, stats in (
                    ("load", row.goblin_memory_load),
                    ("final", row.goblin_memory_final),
                ):
                    lines.append(
                        f"| `{order}` | {members:,} | {fraction * 100:.0f}% | `{phase}` | "
                        f"{fmt_int(stat(stats, 'member_count'))} | "
                        f"{fmt_int(stat(stats, 'member_index_tombstones'))} | "
                        f"{fmt_int(stat(stats, 'score_entry_count'))} | "
                        f"{fmt_int(stat(stats, 'score_block_count'))} | "
                        f"{fmt_int(stat(stats, 'score_block_capacity_sum'))} | "
                        f"{fmt_float(mib(stats, 'total_allocated_bytes'))} | "
                        f"{fmt_float(mib(stats, 'score_index_allocated_bytes'))} | "
                        f"{fmt_float(mib(stats, 'member_index_allocated_bytes'))} |"
                    )

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("both", "goblin", "redis"), default="both")
    default_goblin = ROOT / "build-release" / "goblin-core"
    if not default_goblin.exists():
        default_goblin = ROOT / "build" / "goblin-core"
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--redis-server", type=Path,
                        default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--goblin-rank-cache", action="store_true")
    parser.add_argument("--goblin-score-string-cache", action="store_true")
    parser.add_argument("--member-counts", type=int, nargs="+",
                        default=[50_000, 100_000, 200_000])
    parser.add_argument("--remove-fractions", type=float, nargs="+",
                        default=[0.01, 0.1, 0.5, 0.9])
    remove_order_choices = ("load-prefix", "load-suffix", "reshuffled")
    parser.add_argument("--remove-order", choices=remove_order_choices,
                        help="single-order alias for --remove-orders")
    parser.add_argument("--remove-orders", choices=remove_order_choices, nargs="+",
                        default=None)
    parser.add_argument("--score-shape", choices=zbench.SCORE_SHAPES, default="integer")
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--zrem-batch", type=int, default=128)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "zrem-shape-sweep.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "zrem-shape-sweep.md")
    args = parser.parse_args(argv)

    if not args.member_counts or min(args.member_counts) <= 0:
        parser.error("member counts must be positive")
    if not args.remove_fractions or min(args.remove_fractions) < 0:
        parser.error("remove fractions must be non-negative")
    if max(args.remove_fractions) > 1:
        parser.error("remove fractions must be less than or equal to 1")
    if min(args.pipeline, args.zadd_batch, args.zrem_batch) <= 0:
        parser.error("pipeline, zadd-batch, and zrem-batch must be positive")
    if args.settle_seconds < 0:
        parser.error("settle-seconds must be non-negative")
    if args.remove_orders is not None and args.remove_order is not None:
        parser.error("use either --remove-order or --remove-orders, not both")
    if args.remove_orders is None:
        args.remove_orders = [args.remove_order or "load-prefix"]
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    results: list[ZRemShapeResult] = []
    for order in args.remove_orders:
        for members in args.member_counts:
            for fraction in args.remove_fractions:
                print(
                    f"case order={order} members={members} remove_fraction={fraction}",
                    flush=True,
                )
                results.extend(run_case(args, members, fraction, order))

    write_json(args, results)
    write_report(args, results)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
