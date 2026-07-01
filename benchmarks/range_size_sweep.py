#!/usr/bin/env python3
"""Run zset benchmarks across multiple ZRANGE sizes."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


ROOT = Path(__file__).resolve().parents[1]
ZSET_BENCHMARK = ROOT / "benchmarks" / "zset_benchmark.py"
SCORE_SHAPES = ("integer", "short-decimal", "long-decimal", "random-double")
RANGE_METRICS = (
    ("ZRANGE", "zrange_ops"),
    ("ZRANGE WITHSCORES", "zrange_withscores_ops"),
    ("ZREVRANGE", "zrevrange_ops"),
    ("ZREVRANGE WITHSCORES", "zrevrange_withscores_ops"),
)


def run(command: Sequence[object]) -> None:
    print("+ " + " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def format_int(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):,.0f}"


def format_ratio(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.2f}x"


def result_path(path: Path, report: Path) -> str:
    path = path.resolve()
    parent = report.resolve().parent
    try:
        return str(path.relative_to(parent))
    except ValueError:
        return str(path)


def load_rows(path: Path) -> list[dict[str, Any]]:
    rows = json.loads(path.read_text())
    if not isinstance(rows, list):
        raise ValueError(f"expected list result file: {path}")
    return rows


def indexed(rows: Sequence[dict[str, Any]]) -> dict[tuple[int, str, str], dict[str, Any]]:
    out: dict[tuple[int, str, str], dict[str, Any]] = {}
    for row in rows:
        out[(int(row["range_size"]), str(row["target"]), str(row["metric"]))] = row
    return out


def write_combined_json(args: argparse.Namespace,
                        run_files: Sequence[Path],
                        rows: Sequence[dict[str, Any]]) -> None:
    payload = {
        "config": {
            "target": args.target,
            "members": args.members,
            "ops": args.ops,
            "remove_members": args.remove_members,
            "range_sizes": args.range_sizes,
            "score_shape": args.score_shape,
            "pipeline": args.pipeline,
            "zadd_batch": args.zadd_batch,
            "zrem_batch": args.zrem_batch,
            "goblin_rank_cache": args.goblin_rank_cache,
            "goblin_score_string_cache": args.goblin_score_string_cache,
        },
        "run_files": [str(path) for path in run_files],
        "results": list(rows),
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")


def write_report(args: argparse.Namespace,
                 run_files: Sequence[Path],
                 rows: Sequence[dict[str, Any]]) -> None:
    rows_by_key = indexed(rows)
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [
        "# Goblin Core Range Size Sweep",
        "",
        f"Generated: {generated_at}.",
        "",
        "This benchmark runs the Redis/Goblin zset harness across multiple "
        "`ZRANGE` sizes using one fixed workload configuration.",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- members: `{args.members:,}`",
        f"- operations per metric: `{args.ops:,}`",
        f"- removed members: `{args.remove_members:,}`",
        f"- range sizes: `{', '.join(str(size) for size in args.range_sizes)}`",
        f"- score shape: `{args.score_shape}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- zadd batch size: `{args.zadd_batch}`",
        f"- zrem batch size: `{args.zrem_batch}`",
        f"- Goblin rank cache: `{args.goblin_rank_cache}`",
        f"- Goblin score string cache: `{args.goblin_score_string_cache}`",
        "",
        "Combined JSON:",
        "",
        f"- `{result_path(args.output_json, args.report)}`",
        "",
        "Per-size JSON:",
        "",
    ]
    lines.extend(f"- `{result_path(path, args.report)}`" for path in run_files)
    lines.extend([
        "",
        "## Range Throughput",
        "",
        "| Range size | Metric | Goblin ops/sec | Redis ops/sec | Goblin / Redis |",
        "| ---: | --- | ---: | ---: | ---: |",
    ])

    for size in args.range_sizes:
        for label, metric in RANGE_METRICS:
            goblin = rows_by_key.get((size, "goblin", metric))
            redis = rows_by_key.get((size, "redis", metric))
            goblin_rate = None if goblin is None else float(goblin["per_second"])
            redis_rate = None if redis is None else float(redis["per_second"])
            ratio = None
            if goblin_rate is not None and redis_rate:
                ratio = goblin_rate / redis_rate
            lines.append(
                f"| {size} | `{label}` | {format_int(goblin_rate)} | "
                f"{format_int(redis_rate)} | {format_ratio(ratio)} |"
            )

    lines.extend([
        "",
        "## Memory After Load",
        "",
        "| Range size | Target | RSS delta | RSS delta/member | Redis used_memory |",
        "| ---: | --- | ---: | ---: | ---: |",
    ])
    for size in args.range_sizes:
        for target in ("goblin", "redis"):
            row = rows_by_key.get((size, target, "zadd_members"))
            if row is None:
                continue
            used = row.get("redis_used_memory_mib")
            used_text = "n/a" if used is None else f"{float(used):.2f} MiB"
            lines.append(
                f"| {size} | `{target}` | {float(row['rss_delta_mib']):.2f} MiB | "
                f"{float(row['rss_delta_bytes_per_member']):.2f} B | {used_text} |"
            )

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n")


def benchmark_command(args: argparse.Namespace, range_size: int, output: Path) -> list[object]:
    command: list[object] = [
        sys.executable,
        ZSET_BENCHMARK,
        "--target",
        args.target,
        "--goblin-bin",
        args.goblin_bin,
        "--redis-server",
        args.redis_server,
        "--members",
        args.members,
        "--ops",
        args.ops,
        "--remove-members",
        args.remove_members,
        "--zadd-batch",
        args.zadd_batch,
        "--zrem-batch",
        args.zrem_batch,
        "--range-size",
        range_size,
        "--score-shape",
        args.score_shape,
        "--pipeline",
        args.pipeline,
        "--timeout",
        args.timeout,
        "--settle-seconds",
        args.settle_seconds,
        "--format",
        "json",
        "--output",
        output,
    ]
    if args.goblin_rank_cache:
        command.append("--goblin-rank-cache")
    if args.goblin_score_string_cache:
        command.append("--goblin-score-string-cache")
    return command


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
    parser.add_argument("--members", type=int, default=100_000)
    parser.add_argument("--ops", type=int, default=100_000)
    parser.add_argument("--remove-members", type=int, default=0)
    parser.add_argument("--range-sizes", type=int, nargs="+",
                        default=[1, 4, 16, 64, 256, 1024])
    parser.add_argument("--score-shape", choices=SCORE_SHAPES, default="integer")
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--zrem-batch", type=int, default=128)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "range-size-sweep.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "range-size-sweep.md")
    parser.add_argument("--run-dir", type=Path)
    args = parser.parse_args(argv)

    if min(args.members, args.pipeline, args.zadd_batch, args.zrem_batch) <= 0:
        parser.error("members, pipeline, zadd-batch, and zrem-batch must be positive")
    if args.ops < 0 or args.remove_members < 0:
        parser.error("ops and remove-members must be non-negative")
    if not args.range_sizes or min(args.range_sizes) <= 0:
        parser.error("range sizes must be positive")
    if max(args.range_sizes) > args.members:
        parser.error("range sizes must be less than or equal to members")
    if args.run_dir is None:
        args.run_dir = args.output_json.parent / f"{args.output_json.stem}-runs"
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    args.run_dir.mkdir(parents=True, exist_ok=True)

    combined_rows: list[dict[str, Any]] = []
    run_files: list[Path] = []
    for size in args.range_sizes:
        output = args.run_dir / f"range-size-{size}.json"
        run(benchmark_command(args, size, output))
        run_files.append(output)
        combined_rows.extend(load_rows(output))

    write_combined_json(args, run_files, combined_rows)
    write_report(args, run_files, combined_rows)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
