#!/usr/bin/env python3
"""Benchmark read performance after deleting a fraction of a loaded zset."""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import shutil
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Sequence

import zset_benchmark as zbench


ROOT = Path(__file__).resolve().parents[1]

READ_METRICS = (
    ("ZSCORE ops", "zscore_ops"),
    ("ZRANK ops", "zrank_ops"),
    ("ZREVRANK ops", "zrevrank_ops"),
    ("ZRANGE ops", "zrange_ops"),
    ("ZRANGE WITHSCORES ops", "zrange_withscores_ops"),
    ("ZREVRANGE ops", "zrevrange_ops"),
    ("ZREVRANGE WITHSCORES ops", "zrevrange_withscores_ops"),
)

LATENCY_METRICS = (
    ("ZSCORE", "zscore_latency"),
    ("ZRANK", "zrank_latency"),
    ("ZREVRANK", "zrevrank_latency"),
    ("ZRANGE", "zrange_latency"),
    ("ZREVRANGE", "zrevrange_latency"),
)


@dataclass
class PostDeleteResult:
    target: str
    metric: str
    members_before: int
    members_after: int
    remove_fraction: float
    remove_order: str
    removed_members: int
    score_shape: str
    range_size: int
    pipeline_depth: int
    zadd_batch: int
    zrem_batch: int
    count: int
    seconds: float
    per_second: float
    rss_baseline_mib: float
    rss_after_load_mib: float
    rss_after_remove_mib: float
    redis_used_memory_load_mib: float | None
    redis_used_memory_final_mib: float | None
    goblin_memory_load: dict[str, int] | None
    goblin_memory_final: dict[str, int] | None
    latency_min_us: float | None = None
    latency_p50_us: float | None = None
    latency_p95_us: float | None = None
    latency_p99_us: float | None = None
    latency_max_us: float | None = None


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


def fmt_unit(value: float | int | None, unit: str, digits: int = 2) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):,.{digits}f} {unit}"


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
    random.Random(seed).shuffle(ids)
    return ids[:count]


def start_targets(args: argparse.Namespace) -> list[zbench.ServerProcess]:
    servers: list[zbench.ServerProcess] = []
    if args.target in ("both", "goblin"):
        servers.append(
            zbench.start_goblin(
                args.goblin_bin,
                args.goblin_rank_cache,
                args.goblin_max_output_buffer_mib,
                args.goblin_score_string_cache,
            )
        )
    if args.target in ("both", "redis"):
        servers.append(zbench.start_redis(args.redis_server))
    return servers


def make_result(server: zbench.ServerProcess,
                metric: str,
                count: int,
                seconds: float,
                args: argparse.Namespace,
                members_after: int,
                removed: int,
                rss_baseline: float,
                rss_after_load: float,
                rss_after_remove: float,
                redis_used_memory_load: float | None,
                redis_used_memory_final: float | None,
                goblin_memory_load: dict[str, int] | None,
                goblin_memory_final: dict[str, int] | None,
                latency_stats: dict[str, float] | None = None) -> PostDeleteResult:
    return PostDeleteResult(
        target=server.name,
        metric=metric,
        members_before=args.members,
        members_after=members_after,
        remove_fraction=args.remove_fraction,
        remove_order=args.remove_order,
        removed_members=removed,
        score_shape=args.score_shape,
        range_size=args.range_size,
        pipeline_depth=args.pipeline,
        zadd_batch=args.zadd_batch,
        zrem_batch=args.zrem_batch,
        count=count,
        seconds=seconds,
        per_second=count / seconds if seconds > 0 else 0.0,
        rss_baseline_mib=rss_baseline,
        rss_after_load_mib=rss_after_load,
        rss_after_remove_mib=rss_after_remove,
        redis_used_memory_load_mib=redis_used_memory_load,
        redis_used_memory_final_mib=redis_used_memory_final,
        goblin_memory_load=goblin_memory_load,
        goblin_memory_final=goblin_memory_final,
        latency_min_us=None if latency_stats is None else latency_stats["min"],
        latency_p50_us=None if latency_stats is None else latency_stats["p50"],
        latency_p95_us=None if latency_stats is None else latency_stats["p95"],
        latency_p99_us=None if latency_stats is None else latency_stats["p99"],
        latency_max_us=None if latency_stats is None else latency_stats["max"],
    )


def append_timed_result(results: list[PostDeleteResult],
                        server: zbench.ServerProcess,
                        client: zbench.RespClient,
                        metric: str,
                        commands: Iterable[Sequence[object]],
                        args: argparse.Namespace,
                        members_after: int,
                        removed: int,
                        rss_baseline: float,
                        rss_after_load: float,
                        rss_after_remove: float,
                        redis_used_memory_load: float | None,
                        redis_used_memory_final: float | None,
                        goblin_memory_load: dict[str, int] | None,
                        goblin_memory_final: dict[str, int] | None) -> None:
    command_count, seconds = zbench.time_pipeline(client, commands, args.pipeline)
    count = command_count if metric.endswith("_commands") else args.ops
    if metric == "zrem_members":
        count = removed
    results.append(
        make_result(
            server, metric, count, seconds, args, members_after, removed,
            rss_baseline, rss_after_load, rss_after_remove,
            redis_used_memory_load, redis_used_memory_final,
            goblin_memory_load, goblin_memory_final,
        )
    )


def append_latency_result(results: list[PostDeleteResult],
                          server: zbench.ServerProcess,
                          client: zbench.RespClient,
                          metric: str,
                          commands: Iterable[Sequence[object]],
                          args: argparse.Namespace,
                          members_after: int,
                          removed: int,
                          rss_baseline: float,
                          rss_after_load: float,
                          rss_after_remove: float,
                          redis_used_memory_load: float | None,
                          redis_used_memory_final: float | None,
                          goblin_memory_load: dict[str, int] | None,
                          goblin_memory_final: dict[str, int] | None) -> None:
    count, seconds, latency_stats = zbench.time_latency_samples(
        client,
        commands,
        args.latency_warmup,
        args.latency_samples,
        args.latency_pipeline_depth,
    )
    results.append(
        make_result(
            server, metric, count, seconds, args, members_after, removed,
            rss_baseline, rss_after_load, rss_after_remove,
            redis_used_memory_load, redis_used_memory_final,
            goblin_memory_load, goblin_memory_final, latency_stats,
        )
    )


def run_target(server: zbench.ServerProcess,
               args: argparse.Namespace) -> list[PostDeleteResult]:
    client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
    try:
        key = f"postdelete:{os.getpid()}:{server.name}"
        load_ids = zbench.shuffled_ids(args.members, args.seed)
        removed = remove_count(args.members, args.remove_fraction)
        ids_to_remove = remove_ids(
            load_ids,
            removed,
            args.remove_order,
            args.seed + 17,
        )
        removed_ids = set(ids_to_remove)
        live_ids = [member_id for member_id in load_ids if member_id not in removed_ids]
        members_after = len(live_ids)

        lookup_ids = [live_ids[i % members_after] for i in range(args.ops)]
        random.Random(args.seed + 1).shuffle(lookup_ids)

        rss_baseline = zbench.process_rss_mib(server.process.pid)
        zbench.time_pipeline(
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
        redis_used_memory_load = (
            zbench.redis_used_memory_mib(client) if server.name == "redis" else None
        )
        goblin_memory_load = (
            zbench.goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        results: list[PostDeleteResult] = []
        zrem_commands, zrem_seconds = zbench.time_pipeline(
            client,
            zbench.zrem_commands(ids_to_remove, key, args.zrem_batch),
            args.pipeline,
        )
        time.sleep(args.settle_seconds)
        rss_after_remove = zbench.process_rss_mib(server.process.pid)
        redis_used_memory_final = (
            zbench.redis_used_memory_mib(client) if server.name == "redis" else None
        )
        goblin_memory_final = (
            zbench.goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        results.append(
            make_result(
                server, "zrem_members", removed, zrem_seconds, args,
                members_after, removed, rss_baseline, rss_after_load,
                rss_after_remove, redis_used_memory_load, redis_used_memory_final,
                goblin_memory_load, goblin_memory_final,
            )
        )
        results.append(
            make_result(
                server, "zrem_commands", zrem_commands, zrem_seconds, args,
                members_after, removed, rss_baseline, rss_after_load,
                rss_after_remove, redis_used_memory_load, redis_used_memory_final,
                goblin_memory_load, goblin_memory_final,
            )
        )

        read_context = (
            args, members_after, removed, rss_baseline, rss_after_load,
            rss_after_remove, redis_used_memory_load, redis_used_memory_final,
            goblin_memory_load, goblin_memory_final,
        )
        append_timed_result(
            results, server, client, "zscore_ops",
            zbench.one_member_commands("ZSCORE", lookup_ids, key),
            *read_context)
        append_timed_result(
            results, server, client, "zrank_ops",
            zbench.one_member_commands("ZRANK", lookup_ids, key),
            *read_context)
        append_timed_result(
            results, server, client, "zrevrank_ops",
            zbench.one_member_commands("ZREVRANK", lookup_ids, key),
            *read_context)
        append_timed_result(
            results, server, client, "zrange_ops",
            zbench.zrange_commands(args.ops, members_after, key, args.range_size,
                                   args.seed + 2),
            *read_context)
        append_timed_result(
            results, server, client, "zrange_withscores_ops",
            zbench.zrange_commands(args.ops, members_after, key, args.range_size,
                                   args.seed + 7, True),
            *read_context)
        append_timed_result(
            results, server, client, "zrevrange_ops",
            zbench.range_commands("ZREVRANGE", args.ops, members_after, key,
                                  args.range_size, args.seed + 3),
            *read_context)
        append_timed_result(
            results, server, client, "zrevrange_withscores_ops",
            zbench.range_commands("ZREVRANGE", args.ops, members_after, key,
                                  args.range_size, args.seed + 8, True),
            *read_context)

        if args.latency_samples > 0:
            latency_count = args.latency_samples + args.latency_warmup
            latency_ids = [live_ids[i % members_after] for i in range(latency_count)]
            random.Random(args.seed + 4).shuffle(latency_ids)
            append_latency_result(
                results, server, client, "zscore_latency",
                zbench.one_member_commands("ZSCORE", latency_ids, key),
                *read_context)
            append_latency_result(
                results, server, client, "zrank_latency",
                zbench.one_member_commands("ZRANK", latency_ids, key),
                *read_context)
            append_latency_result(
                results, server, client, "zrevrank_latency",
                zbench.one_member_commands("ZREVRANK", latency_ids, key),
                *read_context)
            append_latency_result(
                results, server, client, "zrange_latency",
                zbench.range_commands("ZRANGE", latency_count, members_after, key,
                                      args.range_size, args.seed + 5),
                *read_context)
            append_latency_result(
                results, server, client, "zrevrange_latency",
                zbench.range_commands("ZREVRANGE", latency_count, members_after, key,
                                      args.range_size, args.seed + 6),
                *read_context)

        return results
    finally:
        client.close()


def run_benchmark(args: argparse.Namespace) -> list[PostDeleteResult]:
    servers = start_targets(args)
    results: list[PostDeleteResult] = []
    try:
        for server in servers:
            results.extend(run_target(server, args))
    finally:
        for server in servers:
            server.stop()
    return results


def write_json(args: argparse.Namespace, results: Sequence[PostDeleteResult]) -> None:
    payload = {
        "config": {
            "target": args.target,
            "members": args.members,
            "ops": args.ops,
            "remove_fraction": args.remove_fraction,
            "remove_order": args.remove_order,
            "score_shape": args.score_shape,
            "range_size": args.range_size,
            "pipeline": args.pipeline,
            "zadd_batch": args.zadd_batch,
            "zrem_batch": args.zrem_batch,
            "latency_samples": args.latency_samples,
            "latency_warmup": args.latency_warmup,
            "latency_pipeline_depth": args.latency_pipeline_depth,
            "goblin_rank_cache": args.goblin_rank_cache,
            "goblin_score_string_cache": args.goblin_score_string_cache,
            "seed": args.seed,
        },
        "results": [asdict(row) for row in results],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")


def rows_by_key(results: Sequence[PostDeleteResult]) -> dict[tuple[str, str], PostDeleteResult]:
    return {(row.target, row.metric): row for row in results}


def comparison_table(results: Sequence[PostDeleteResult],
                     metrics: Sequence[tuple[str, str]]) -> list[str]:
    by_key = rows_by_key(results)
    lines = [
        "| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, metric in metrics:
        goblin = by_key.get(("goblin", metric))
        redis = by_key.get(("redis", metric))
        goblin_rate = None if goblin is None else goblin.per_second
        redis_rate = None if redis is None else redis.per_second
        ratio = None if goblin_rate is None or not redis_rate else goblin_rate / redis_rate
        lines.append(
            f"| `{label}` | {fmt_int(goblin_rate)} | "
            f"{fmt_int(redis_rate)} | {fmt_ratio(ratio)} |"
        )
    return lines


def latency_table(results: Sequence[PostDeleteResult]) -> list[str]:
    by_key = rows_by_key(results)
    if not any((target, metric) in by_key
               for _, metric in LATENCY_METRICS
               for target in ("goblin", "redis")):
        return ["Latency mode was disabled."]

    lines = [
        "| Metric | Target | p50 | p95 | p99 | max |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for label, metric in LATENCY_METRICS:
        for target in ("goblin", "redis"):
            row = by_key.get((target, metric))
            if row is None:
                continue
            lines.append(
                f"| `{label}` | `{target}` | "
                f"{fmt_unit(row.latency_p50_us, 'us')} | "
                f"{fmt_unit(row.latency_p95_us, 'us')} | "
                f"{fmt_unit(row.latency_p99_us, 'us')} | "
                f"{fmt_unit(row.latency_max_us, 'us')} |"
            )
    return lines


def memory_table(results: Sequence[PostDeleteResult]) -> list[str]:
    by_key = rows_by_key(results)
    lines = [
        "| Target | Load RSS MiB | Final RSS MiB | Redis load used MiB | Redis final used MiB |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for target in ("goblin", "redis"):
        row = by_key.get((target, "zrem_members"))
        if row is None:
            continue
        lines.append(
            f"| `{target}` | {fmt_float(row.rss_after_load_mib)} | "
            f"{fmt_float(row.rss_after_remove_mib)} | "
            f"{fmt_float(row.redis_used_memory_load_mib)} | "
            f"{fmt_float(row.redis_used_memory_final_mib)} |"
        )
    return lines


def goblin_internals_table(results: Sequence[PostDeleteResult]) -> list[str]:
    by_key = rows_by_key(results)
    row = by_key.get(("goblin", "zrem_members"))
    if row is None:
        return ["No Goblin Core run was recorded."]

    lines = [
        "| Phase | member_count | tombstones | score_entries | score_blocks | score_capacity | total alloc MiB | score index MiB | member index MiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for phase, stats in (
        ("load", row.goblin_memory_load),
        ("after remove", row.goblin_memory_final),
    ):
        lines.append(
            f"| `{phase}` | {fmt_int(stat(stats, 'member_count'))} | "
            f"{fmt_int(stat(stats, 'member_index_tombstones'))} | "
            f"{fmt_int(stat(stats, 'score_entry_count'))} | "
            f"{fmt_int(stat(stats, 'score_block_count'))} | "
            f"{fmt_int(stat(stats, 'score_block_capacity_sum'))} | "
            f"{fmt_float(mib(stats, 'total_allocated_bytes'))} | "
            f"{fmt_float(mib(stats, 'score_index_allocated_bytes'))} | "
            f"{fmt_float(mib(stats, 'member_index_allocated_bytes'))} |"
        )
    return lines


def write_report(args: argparse.Namespace, results: Sequence[PostDeleteResult]) -> None:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    removed = remove_count(args.members, args.remove_fraction)
    members_after = args.members - removed
    lines = [
        "# Goblin Core Post-Delete Read Benchmark",
        "",
        f"Generated: {generated_at}.",
        "",
        "This benchmark loads one zset, removes a configured fraction of members, "
        "then measures read and range throughput against the remaining members.",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- loaded members: `{args.members:,}`",
        f"- removed members: `{removed:,}`",
        f"- remaining members: `{members_after:,}`",
        f"- remove fraction: `{args.remove_fraction}`",
        f"- remove order: `{args.remove_order}`",
        f"- read/range ops: `{args.ops:,}`",
        f"- score shape: `{args.score_shape}`",
        f"- range size: `{args.range_size}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- zadd batch size: `{args.zadd_batch}`",
        f"- zrem batch size: `{args.zrem_batch}`",
        f"- latency samples: `{args.latency_samples:,}`",
        f"- latency warmup: `{args.latency_warmup:,}`",
        f"- latency pipeline depth: `{args.latency_pipeline_depth}`",
        f"- Goblin rank cache: `{args.goblin_rank_cache}`",
        f"- Goblin score-string cache: `{args.goblin_score_string_cache}`",
        f"- seed: `{args.seed}`",
        "",
        f"Source data: `{result_path(args.output_json, args.report)}`",
        "",
        "## Remove Throughput",
        "",
        *comparison_table(results, (("ZREM members", "zrem_members"),)),
        "",
        "## Post-Delete Read Throughput",
        "",
        *comparison_table(results, READ_METRICS),
        "",
        "## Post-Delete Read Latency",
        "",
        *latency_table(results),
        "",
        "## Process Memory",
        "",
        *memory_table(results),
        "",
        "## Goblin Internals",
        "",
        *goblin_internals_table(results),
        "",
    ]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("both", "goblin", "redis"), default="both")
    default_goblin = ROOT / "build-release" / "goblin-core"
    if not default_goblin.exists():
        default_goblin = ROOT / "build" / "goblin-core"
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--goblin-rank-cache", action="store_true")
    parser.add_argument("--goblin-score-string-cache", action="store_true")
    parser.add_argument("--goblin-max-output-buffer-mib", type=int)
    parser.add_argument("--redis-server", type=Path,
                        default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--members", type=int, default=1_000_000)
    parser.add_argument("--ops", type=int, default=1_000_000)
    parser.add_argument("--remove-fraction", type=float, default=0.5)
    parser.add_argument("--remove-order",
                        choices=("load-prefix", "load-suffix", "reshuffled"),
                        default="load-prefix")
    parser.add_argument("--score-shape", choices=zbench.SCORE_SHAPES, default="integer")
    parser.add_argument("--range-size", type=int, default=16)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--zrem-batch", type=int, default=128)
    parser.add_argument("--latency-samples", type=int, default=0)
    parser.add_argument("--latency-warmup", type=int, default=100)
    parser.add_argument("--latency-pipeline-depth", type=int, default=1)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "post-delete-read.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "post-delete-read.md")
    args = parser.parse_args(argv)

    if args.members <= 0:
        parser.error("--members must be positive")
    if args.ops < 0 or args.latency_samples < 0 or args.latency_warmup < 0:
        parser.error("--ops, --latency-samples, and --latency-warmup must be non-negative")
    if not 0.0 <= args.remove_fraction < 1.0:
        parser.error("--remove-fraction must be greater than or equal to 0 and less than 1")
    removed = remove_count(args.members, args.remove_fraction)
    if removed >= args.members:
        parser.error("remove fraction leaves no members to read")
    if args.goblin_max_output_buffer_mib is not None and args.goblin_max_output_buffer_mib < 0:
        parser.error("--goblin-max-output-buffer-mib must be non-negative")
    if min(args.range_size, args.pipeline, args.zadd_batch, args.zrem_batch,
           args.latency_pipeline_depth) <= 0:
        parser.error(
            "--range-size, --pipeline, --zadd-batch, --zrem-batch, "
            "and --latency-pipeline-depth must be positive"
        )
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    results = run_benchmark(args)
    write_json(args, results)
    write_report(args, results)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
