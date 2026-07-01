#!/usr/bin/env python3
"""Run skip-parser range-output benchmarks across multiple range sizes."""

from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

import zset_benchmark as zbench


ROOT = Path(__file__).resolve().parents[1]
RANGE_OUTPUT_BENCHMARK = ROOT / "benchmarks" / "range_output_benchmark.py"
RANGE_METRICS = (
    ("ZRANGE", "zrange"),
    ("ZREVRANGE", "zrevrange"),
    ("ZRANGE WITHSCORES", "zrange_withscores"),
    ("ZREVRANGE WITHSCORES", "zrevrange_withscores"),
)
VARIANT_METRICS = tuple(metric for _, metric in RANGE_METRICS)


def run(command: Sequence[object]) -> None:
    print("+ " + " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def format_number(value: float | int | None, digits: int = 0) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):,.{digits}f}"


def format_ratio(value: float | int | None) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
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
            "warmup_ops": args.warmup_ops,
            "range_sizes": args.range_sizes,
            "score_shape": args.score_shape,
            "pipeline": args.pipeline,
            "read_delay_ms": args.read_delay_ms,
            "goblin_max_output_buffer_mib": args.goblin_max_output_buffer_mib,
            "goblin_initial_output_buffer_kib": args.goblin_initial_output_buffer_kib,
            "goblin_rank_cache": args.goblin_rank_cache,
            "goblin_rank_cache_mode": args.goblin_rank_cache_mode,
            "goblin_score_string_cache": args.goblin_score_string_cache,
            "variant_order": args.variant_order,
            "zadd_batch": args.zadd_batch,
            "load_pipeline": args.load_pipeline,
            "rss_sample_interval_ms": args.rss_sample_interval_ms,
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
        "# Goblin Core Range Output Sweep",
        "",
        f"Generated: {generated_at}.",
        "",
        "This benchmark runs the range-output stress test across multiple "
        "`ZRANGE` sizes. The client validates RESP framing but skips payload "
        "materialization, so the result is less sensitive to Python object "
        "allocation than the normal zset benchmark.",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- members: `{args.members:,}`",
        f"- range commands per metric: `{args.ops:,}`",
        f"- warmup commands per metric: `{args.warmup_ops:,}`",
        f"- range sizes: `{', '.join(str(size) for size in args.range_sizes)}`",
        f"- score shape: `{args.score_shape}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- read delay per burst: `{args.read_delay_ms}` ms",
        f"- Goblin max output buffer: `{args.goblin_max_output_buffer_mib}` MiB",
        f"- Goblin initial output buffer: `{args.goblin_initial_output_buffer_kib}` KiB",
        f"- Goblin rank cache: `{args.goblin_rank_cache}`",
        f"- Goblin rank cache mode: `{args.goblin_rank_cache_mode}`",
        f"- Goblin score string cache: `{args.goblin_score_string_cache}`",
        f"- variant order: `{', '.join(args.variant_order)}`",
        f"- RSS sample interval: `{args.rss_sample_interval_ms}` ms",
        f"- zadd batch size: `{args.zadd_batch}`",
        f"- load pipeline depth: `{args.load_pipeline}`",
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
        "## Output Throughput",
        "",
        "| Range size | Metric | Goblin ops/sec | Redis ops/sec | Goblin / Redis | Goblin MiB/sec | Redis MiB/sec | Goblin / Redis MiB/sec |",
        "| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])

    for size in args.range_sizes:
        for label, metric in RANGE_METRICS:
            goblin = rows_by_key.get((size, "goblin", metric))
            redis = rows_by_key.get((size, "redis", metric))
            goblin_ops = None if goblin is None else float(goblin["ops_per_second"])
            redis_ops = None if redis is None else float(redis["ops_per_second"])
            goblin_mib = None if goblin is None else float(goblin["response_mib_per_second"])
            redis_mib = None if redis is None else float(redis["response_mib_per_second"])
            ops_ratio = None if goblin_ops is None or not redis_ops else goblin_ops / redis_ops
            mib_ratio = None if goblin_mib is None or not redis_mib else goblin_mib / redis_mib
            lines.append(
                f"| {size} | `{label}` | {format_number(goblin_ops)} | "
                f"{format_number(redis_ops)} | {format_ratio(ops_ratio)} | "
                f"{format_number(goblin_mib, 2)} | {format_number(redis_mib, 2)} | "
                f"{format_ratio(mib_ratio)} |"
            )

    lines.extend([
        "",
        "## Peak RSS During Output",
        "",
        "| Range size | Target | Load RSS MiB | Peak RSS MiB | Peak - load MiB | Final RSS MiB |",
        "| ---: | --- | ---: | ---: | ---: | ---: |",
    ])
    for size in args.range_sizes:
        for target in ("goblin", "redis"):
            rows_for_target = [
                rows_by_key.get((size, target, metric))
                for _, metric in RANGE_METRICS
            ]
            present = [row for row in rows_for_target if row is not None]
            if not present:
                continue
            load_rss = float(present[0]["rss_after_load_mib"])
            peak_rss = max(float(row["peak_rss_mib"]) for row in present)
            final_rss = float(present[-1]["rss_final_mib"])
            lines.append(
                f"| {size} | `{target}` | {format_number(load_rss, 2)} | "
                f"{format_number(peak_rss, 2)} | {format_number(peak_rss - load_rss, 2)} | "
                f"{format_number(final_rss, 2)} |"
            )

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n")


def benchmark_command(args: argparse.Namespace, range_size: int, output: Path) -> list[object]:
    command: list[object] = [
        sys.executable,
        RANGE_OUTPUT_BENCHMARK,
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
        "--warmup-ops",
        args.warmup_ops,
        "--score-shape",
        args.score_shape,
        "--range-size",
        range_size,
        "--pipeline",
        args.pipeline,
        "--read-delay-ms",
        args.read_delay_ms,
        "--goblin-max-output-buffer-mib",
        args.goblin_max_output_buffer_mib,
        "--goblin-initial-output-buffer-kib",
        args.goblin_initial_output_buffer_kib,
        "--zadd-batch",
        args.zadd_batch,
        "--load-pipeline",
        args.load_pipeline,
        "--timeout",
        args.timeout,
        "--settle-seconds",
        args.settle_seconds,
        "--rss-sample-interval-ms",
        args.rss_sample_interval_ms,
        "--variant-order",
        *args.variant_order,
        "--output-json",
        output,
        "--report",
        output.with_suffix(".md"),
    ]
    if args.goblin_rank_cache:
        command.append("--goblin-rank-cache")
    command.extend(["--goblin-rank-cache-mode", args.goblin_rank_cache_mode])
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
    parser.add_argument("--goblin-rank-cache-mode",
                        choices=("off", "exact", "block-hint"))
    parser.add_argument("--goblin-score-string-cache", action="store_true")
    parser.add_argument("--goblin-max-output-buffer-mib", type=int, default=1)
    parser.add_argument("--goblin-initial-output-buffer-kib", type=int, default=0)
    parser.add_argument("--members", type=int, default=100_000)
    parser.add_argument("--ops", type=int, default=10_000)
    parser.add_argument("--warmup-ops", type=int, default=0)
    parser.add_argument("--range-sizes", type=int, nargs="+",
                        default=[16, 64, 256, 1024])
    parser.add_argument("--score-shape", choices=zbench.SCORE_SHAPES, default="integer")
    parser.add_argument("--pipeline", type=int, default=2048)
    parser.add_argument("--read-delay-ms", type=float, default=2.0)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--load-pipeline", type=int, default=256)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--rss-sample-interval-ms", type=float, default=10.0)
    parser.add_argument(
        "--variant-order",
        nargs="+",
        choices=VARIANT_METRICS,
        default=list(VARIANT_METRICS),
        help="metric execution order passed to range_output_benchmark.py",
    )
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "range-output-sweep.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "range-output-sweep.md")
    parser.add_argument("--run-dir", type=Path)
    args = parser.parse_args(argv)

    if min(args.members, args.ops, args.pipeline, args.zadd_batch,
           args.load_pipeline) <= 0:
        parser.error(
            "members, ops, pipeline, zadd-batch, and load-pipeline must be positive"
        )
    if args.warmup_ops < 0:
        parser.error("warmup-ops must be non-negative")
    if args.goblin_max_output_buffer_mib < 0:
        parser.error("goblin-max-output-buffer-mib must be non-negative")
    if args.goblin_initial_output_buffer_kib < 0:
        parser.error("goblin-initial-output-buffer-kib must be non-negative")
    if not args.range_sizes or min(args.range_sizes) <= 0:
        parser.error("range sizes must be positive")
    if max(args.range_sizes) > args.members:
        parser.error("range sizes must be less than or equal to members")
    if args.read_delay_ms < 0 or args.settle_seconds < 0 or args.rss_sample_interval_ms <= 0:
        parser.error(
            "read-delay-ms and settle-seconds must be non-negative; "
            "rss-sample-interval-ms must be positive"
        )
    if args.run_dir is None:
        args.run_dir = args.output_json.parent / f"{args.output_json.stem}-runs"
    if args.goblin_rank_cache_mode is None:
        args.goblin_rank_cache_mode = "exact" if args.goblin_rank_cache else "off"
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    args.run_dir.mkdir(parents=True, exist_ok=True)

    combined_rows: list[dict[str, Any]] = []
    run_files: list[Path] = []
    for size in args.range_sizes:
        output = args.run_dir / f"range-output-size-{size}.json"
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
