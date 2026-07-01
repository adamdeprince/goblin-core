#!/usr/bin/env python3
"""Benchmark an interleaved leaderboard-style zset workload."""

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

OP_LABELS = {
    "zscore": "ZSCORE",
    "zrank": "ZRANK",
    "zrevrank": "ZREVRANK",
    "zrange": "ZRANGE",
    "zrange_withscores": "ZRANGE WITHSCORES",
    "zadd_update": "ZADD score update",
    "zrem_add": "ZREM + replacement ZADD",
}


@dataclass
class MixedResult:
    target: str
    members: int
    ops: int
    resp_commands: int
    score_shape: str
    range_size: int
    pipeline_depth: int
    zadd_batch: int
    load_seconds: float
    load_members_per_second: float
    mixed_seconds: float
    logical_ops_per_second: float
    resp_commands_per_second: float
    op_counts: dict[str, int]
    rss_baseline_mib: float
    rss_after_load_mib: float
    rss_after_mixed_mib: float
    rss_final_mib: float
    redis_used_memory_load_mib: float | None
    redis_used_memory_mixed_mib: float | None
    redis_used_memory_final_mib: float | None
    goblin_memory_load: dict[str, int] | None
    goblin_memory_mixed: dict[str, int] | None
    goblin_memory_final: dict[str, int] | None
    latency_samples: int
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


def update_score(member_id: int, ordinal: int, score_shape: str, seed: int) -> float:
    mixed_id = member_id ^ ((ordinal + 1) * 0x9E3779B9)
    return zbench.score_for(mixed_id, score_shape, seed + 1_000_003)


def operation_weights(args: argparse.Namespace) -> list[tuple[str, int]]:
    weights = [
        ("zscore", args.zscore_weight),
        ("zrank", args.zrank_weight),
        ("zrevrank", args.zrevrank_weight),
        ("zrange", args.zrange_weight),
        ("zrange_withscores", args.zrange_withscores_weight),
        ("zadd_update", args.zadd_update_weight),
        ("zrem_add", args.zrem_add_weight),
    ]
    return [(name, weight) for name, weight in weights if weight > 0]


class MixedWorkload:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.rng = random.Random(args.seed + 91)
        self.live_ids = list(range(args.members))
        self.next_member_id = args.members
        self.weights = operation_weights(args)
        self.total_weight = sum(weight for _, weight in self.weights)
        self.counts = {name: 0 for name in OP_LABELS}
        self.event_index = 0

    def choose_operation(self) -> str:
        selected = self.rng.randrange(self.total_weight)
        running = 0
        for name, weight in self.weights:
            running += weight
            if selected < running:
                return name
        return self.weights[-1][0]

    def random_live_member(self) -> int:
        return self.live_ids[self.rng.randrange(len(self.live_ids))]

    def next_event(self, key: str) -> list[list[object]]:
        op = self.choose_operation()
        self.counts[op] += 1
        self.event_index += 1

        if op == "zscore":
            return [["ZSCORE", key, zbench.member_for(self.random_live_member())]]
        if op == "zrank":
            return [["ZRANK", key, zbench.member_for(self.random_live_member())]]
        if op == "zrevrank":
            return [["ZREVRANK", key, zbench.member_for(self.random_live_member())]]
        if op == "zrange":
            return [["ZRANGE", key, 0, self.args.range_size - 1]]
        if op == "zrange_withscores":
            return [["ZRANGE", key, 0, self.args.range_size - 1, "WITHSCORES"]]
        if op == "zadd_update":
            member_id = self.random_live_member()
            return [[
                "ZADD",
                key,
                update_score(member_id, self.event_index, self.args.score_shape, self.args.seed),
                zbench.member_for(member_id),
            ]]

        index = self.rng.randrange(len(self.live_ids))
        removed_id = self.live_ids[index]
        added_id = self.next_member_id
        self.next_member_id += 1
        self.live_ids[index] = added_id
        return [
            ["ZREM", key, zbench.member_for(removed_id)],
            [
                "ZADD",
                key,
                zbench.score_for(added_id, self.args.score_shape, self.args.seed),
                zbench.member_for(added_id),
            ],
        ]

    def commands(self, key: str, ops: int) -> Iterable[list[object]]:
        for _ in range(ops):
            for command in self.next_event(key):
                yield command

    def events(self, key: str, ops: int) -> Iterable[list[list[object]]]:
        for _ in range(ops):
            yield self.next_event(key)


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


def send_event(client: zbench.RespClient, event: Sequence[Sequence[object]]) -> None:
    payload = bytearray()
    for command in event:
        payload.extend(zbench.encode_command(command))
    client.sock.sendall(payload)
    for _ in event:
        client.read_response()


def time_mixed_latency(client: zbench.RespClient,
                       workload: MixedWorkload,
                       key: str,
                       warmup: int,
                       samples: int) -> tuple[float, dict[str, float]]:
    for event in workload.events(key, warmup):
        send_event(client, event)

    latencies: list[float] = []
    started = time.perf_counter()
    for event in workload.events(key, samples):
        event_started = time.perf_counter()
        send_event(client, event)
        latencies.append(time.perf_counter() - event_started)
    elapsed = time.perf_counter() - started
    return elapsed, zbench.latency_stats_us(latencies)


def make_result(server: zbench.ServerProcess,
                args: argparse.Namespace,
                ops: int,
                resp_commands: int,
                load_seconds: float,
                mixed_seconds: float,
                op_counts: dict[str, int],
                rss_baseline: float,
                rss_after_load: float,
                rss_after_mixed: float,
                rss_final: float,
                redis_used_memory_load: float | None,
                redis_used_memory_mixed: float | None,
                redis_used_memory_final: float | None,
                goblin_memory_load: dict[str, int] | None,
                goblin_memory_mixed: dict[str, int] | None,
                goblin_memory_final: dict[str, int] | None,
                latency_stats: dict[str, float] | None) -> MixedResult:
    return MixedResult(
        target=server.name,
        members=args.members,
        ops=ops,
        resp_commands=resp_commands,
        score_shape=args.score_shape,
        range_size=args.range_size,
        pipeline_depth=args.pipeline,
        zadd_batch=args.zadd_batch,
        load_seconds=load_seconds,
        load_members_per_second=args.members / load_seconds if load_seconds > 0 else 0.0,
        mixed_seconds=mixed_seconds,
        logical_ops_per_second=ops / mixed_seconds if mixed_seconds > 0 else 0.0,
        resp_commands_per_second=(
            resp_commands / mixed_seconds if mixed_seconds > 0 else 0.0
        ),
        op_counts=op_counts,
        rss_baseline_mib=rss_baseline,
        rss_after_load_mib=rss_after_load,
        rss_after_mixed_mib=rss_after_mixed,
        rss_final_mib=rss_final,
        redis_used_memory_load_mib=redis_used_memory_load,
        redis_used_memory_mixed_mib=redis_used_memory_mixed,
        redis_used_memory_final_mib=redis_used_memory_final,
        goblin_memory_load=goblin_memory_load,
        goblin_memory_mixed=goblin_memory_mixed,
        goblin_memory_final=goblin_memory_final,
        latency_samples=args.latency_samples,
        latency_min_us=None if latency_stats is None else latency_stats["min"],
        latency_p50_us=None if latency_stats is None else latency_stats["p50"],
        latency_p95_us=None if latency_stats is None else latency_stats["p95"],
        latency_p99_us=None if latency_stats is None else latency_stats["p99"],
        latency_max_us=None if latency_stats is None else latency_stats["max"],
    )


def run_target(server: zbench.ServerProcess, args: argparse.Namespace) -> MixedResult:
    client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
    try:
        key = f"mixed:{os.getpid()}:{server.name}"
        load_ids = zbench.shuffled_ids(args.members, args.seed)
        rss_baseline = zbench.process_rss_mib(server.process.pid)

        _, load_seconds = zbench.time_pipeline(
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

        workload = MixedWorkload(args)
        started = time.perf_counter()
        resp_commands = client.pipeline(workload.commands(key, args.ops), args.pipeline)
        mixed_seconds = time.perf_counter() - started
        mixed_counts = dict(workload.counts)

        time.sleep(args.settle_seconds)
        rss_after_mixed = zbench.process_rss_mib(server.process.pid)
        redis_used_memory_mixed = (
            zbench.redis_used_memory_mib(client) if server.name == "redis" else None
        )
        goblin_memory_mixed = (
            zbench.goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        latency_stats = None
        if args.latency_samples > 0:
            _, latency_stats = time_mixed_latency(
                client,
                workload,
                key,
                args.latency_warmup,
                args.latency_samples,
            )

        time.sleep(args.settle_seconds)
        rss_final = zbench.process_rss_mib(server.process.pid)
        redis_used_memory_final = (
            zbench.redis_used_memory_mib(client) if server.name == "redis" else None
        )
        goblin_memory_final = (
            zbench.goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        return make_result(
            server,
            args,
            args.ops,
            resp_commands,
            load_seconds,
            mixed_seconds,
            mixed_counts,
            rss_baseline,
            rss_after_load,
            rss_after_mixed,
            rss_final,
            redis_used_memory_load,
            redis_used_memory_mixed,
            redis_used_memory_final,
            goblin_memory_load,
            goblin_memory_mixed,
            goblin_memory_final,
            latency_stats,
        )
    finally:
        client.close()


def run_benchmark(args: argparse.Namespace) -> list[MixedResult]:
    servers = start_targets(args)
    results: list[MixedResult] = []
    try:
        for server in servers:
            results.append(run_target(server, args))
    finally:
        for server in servers:
            server.stop()
    return results


def write_json(args: argparse.Namespace, results: Sequence[MixedResult]) -> None:
    payload = {
        "config": {
            "target": args.target,
            "members": args.members,
            "ops": args.ops,
            "score_shape": args.score_shape,
            "range_size": args.range_size,
            "pipeline": args.pipeline,
            "zadd_batch": args.zadd_batch,
            "latency_samples": args.latency_samples,
            "latency_warmup": args.latency_warmup,
            "goblin_rank_cache": args.goblin_rank_cache,
            "goblin_score_string_cache": args.goblin_score_string_cache,
            "seed": args.seed,
            "weights": {name: weight for name, weight in operation_weights(args)},
        },
        "results": [asdict(row) for row in results],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")


def by_target(results: Sequence[MixedResult]) -> dict[str, MixedResult]:
    return {row.target: row for row in results}


def comparison_lines(results: Sequence[MixedResult]) -> list[str]:
    rows = by_target(results)
    goblin = rows.get("goblin")
    redis = rows.get("redis")

    def ratio(goblin_value: float | None, redis_value: float | None) -> float | None:
        if goblin_value is None or not redis_value:
            return None
        return goblin_value / redis_value

    metrics = [
        ("Load members/sec", "load_members_per_second"),
        ("Mixed logical ops/sec", "logical_ops_per_second"),
        ("Mixed RESP commands/sec", "resp_commands_per_second"),
    ]
    lines = [
        "| Metric | Goblin Core | Redis | Goblin Core / Redis |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, attr in metrics:
        goblin_value = None if goblin is None else getattr(goblin, attr)
        redis_value = None if redis is None else getattr(redis, attr)
        lines.append(
            f"| `{label}` | {fmt_int(goblin_value)} | {fmt_int(redis_value)} | "
            f"{fmt_ratio(ratio(goblin_value, redis_value))} |"
        )
    return lines


def operation_mix_lines(results: Sequence[MixedResult]) -> list[str]:
    rows = by_target(results)
    row = rows.get("goblin") or rows.get("redis")
    if row is None:
        return ["No operation counts were recorded."]
    lines = [
        "| Operation | Count | Share |",
        "| --- | ---: | ---: |",
    ]
    for name, label in OP_LABELS.items():
        count = row.op_counts.get(name, 0)
        share = (count / row.ops * 100.0) if row.ops > 0 else 0.0
        lines.append(f"| `{label}` | {fmt_int(count)} | {fmt_float(share, 1)}% |")
    return lines


def latency_lines(results: Sequence[MixedResult]) -> list[str]:
    if not any(row.latency_p50_us is not None for row in results):
        return ["Latency mode was disabled."]
    lines = [
        "| Target | p50 | p95 | p99 | max |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for row in results:
        lines.append(
            f"| `{row.target}` | {fmt_unit(row.latency_p50_us, 'us')} | "
            f"{fmt_unit(row.latency_p95_us, 'us')} | "
            f"{fmt_unit(row.latency_p99_us, 'us')} | "
            f"{fmt_unit(row.latency_max_us, 'us')} |"
        )
    return lines


def memory_lines(results: Sequence[MixedResult]) -> list[str]:
    lines = [
        "| Target | Load RSS MiB | Mixed RSS MiB | Final RSS MiB | Redis load used MiB | Redis final used MiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in results:
        lines.append(
            f"| `{row.target}` | {fmt_float(row.rss_after_load_mib)} | "
            f"{fmt_float(row.rss_after_mixed_mib)} | "
            f"{fmt_float(row.rss_final_mib)} | "
            f"{fmt_float(row.redis_used_memory_load_mib)} | "
            f"{fmt_float(row.redis_used_memory_final_mib)} |"
        )
    return lines


def goblin_internals_lines(results: Sequence[MixedResult]) -> list[str]:
    row = by_target(results).get("goblin")
    if row is None:
        return ["No Goblin Core run was recorded."]
    lines = [
        "| Phase | member_count | tombstones | score_entries | score_blocks | score_capacity | total alloc MiB | score index MiB | member index MiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for phase, stats in (
        ("load", row.goblin_memory_load),
        ("after mixed", row.goblin_memory_mixed),
        ("final", row.goblin_memory_final),
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


def write_report(args: argparse.Namespace, results: Sequence[MixedResult]) -> None:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    weights = ", ".join(
        f"{OP_LABELS[name]}={weight}" for name, weight in operation_weights(args)
    )
    lines = [
        "# Goblin Core Mixed Leaderboard Benchmark",
        "",
        f"Generated: {generated_at}.",
        "",
        "This benchmark preloads one zset and runs an interleaved leaderboard "
        "workload with reads, small top ranges, score updates, and remove/add churn.",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- loaded members: `{args.members:,}`",
        f"- logical mixed operations: `{args.ops:,}`",
        f"- score shape: `{args.score_shape}`",
        f"- range: `0..{args.range_size - 1}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- zadd batch size: `{args.zadd_batch}`",
        f"- latency samples: `{args.latency_samples:,}`",
        f"- latency warmup: `{args.latency_warmup:,}`",
        f"- Goblin rank cache: `{args.goblin_rank_cache}`",
        f"- Goblin score-string cache: `{args.goblin_score_string_cache}`",
        f"- seed: `{args.seed}`",
        f"- weights: `{weights}`",
        "",
        f"Source data: `{result_path(args.output_json, args.report)}`",
        "",
        "## Throughput",
        "",
        *comparison_lines(results),
        "",
        "## Operation Mix",
        "",
        *operation_mix_lines(results),
        "",
        "## Mixed Latency",
        "",
        *latency_lines(results),
        "",
        "## Process Memory",
        "",
        *memory_lines(results),
        "",
        "## Goblin Internals",
        "",
        *goblin_internals_lines(results),
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
    parser.add_argument("--score-shape", choices=zbench.SCORE_SHAPES, default="integer")
    parser.add_argument("--range-size", type=int, default=100)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--latency-samples", type=int, default=0)
    parser.add_argument("--latency-warmup", type=int, default=100)
    parser.add_argument("--zscore-weight", type=int, default=40)
    parser.add_argument("--zrank-weight", type=int, default=15)
    parser.add_argument("--zrevrank-weight", type=int, default=10)
    parser.add_argument("--zrange-weight", type=int, default=10)
    parser.add_argument("--zrange-withscores-weight", type=int, default=10)
    parser.add_argument("--zadd-update-weight", type=int, default=10)
    parser.add_argument("--zrem-add-weight", type=int, default=5)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "mixed-leaderboard.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "mixed-leaderboard.md")
    args = parser.parse_args(argv)

    if args.members <= 0 or args.ops <= 0:
        parser.error("--members and --ops must be positive")
    if args.latency_samples < 0 or args.latency_warmup < 0:
        parser.error("--latency-samples and --latency-warmup must be non-negative")
    if args.goblin_max_output_buffer_mib is not None and args.goblin_max_output_buffer_mib < 0:
        parser.error("--goblin-max-output-buffer-mib must be non-negative")
    if min(args.range_size, args.pipeline, args.zadd_batch) <= 0:
        parser.error("--range-size, --pipeline, and --zadd-batch must be positive")
    if any(weight < 0 for _, weight in [
        ("zscore", args.zscore_weight),
        ("zrank", args.zrank_weight),
        ("zrevrank", args.zrevrank_weight),
        ("zrange", args.zrange_weight),
        ("zrange_withscores", args.zrange_withscores_weight),
        ("zadd_update", args.zadd_update_weight),
        ("zrem_add", args.zrem_add_weight),
    ]):
        parser.error("operation weights must be non-negative")
    weights = operation_weights(args)
    if not weights:
        parser.error("at least one operation weight must be positive")
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
