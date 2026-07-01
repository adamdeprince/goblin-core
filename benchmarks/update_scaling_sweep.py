#!/usr/bin/env python3
"""Run ZADD/ZREM scaling benchmarks across multiple set sizes."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

import zset_benchmark as zbench


ROOT = Path(__file__).resolve().parents[1]
ZSET_BENCHMARK = ROOT / "benchmarks" / "zset_benchmark.py"
UPDATE_METRICS = (
    ("ZADD members", "zadd_members"),
    ("ZADD commands", "zadd_commands"),
    ("ZREM members", "zrem_members"),
    ("ZREM commands", "zrem_commands"),
)


def run(command: Sequence[object]) -> None:
    print("+ " + " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=ROOT, check=True)


def fmt_rate(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):,.0f}"


def fmt_ratio(value: float | int | None) -> str:
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


def configured_remove_members(args: argparse.Namespace, members: int) -> int:
    if args.remove_members is not None:
        return min(args.remove_members, members)
    return min(members, int(round(members * args.remove_fraction)))


def load_rows(path: Path, members: int, remove_members: int) -> list[dict[str, Any]]:
    rows = json.loads(path.read_text())
    if not isinstance(rows, list):
        raise ValueError(f"expected list result file: {path}")
    for row in rows:
        row["configured_members"] = members
        row["configured_remove_members"] = remove_members
    return rows


def indexed(rows: Sequence[dict[str, Any]]) -> dict[tuple[int, str, str], dict[str, Any]]:
    out: dict[tuple[int, str, str], dict[str, Any]] = {}
    for row in rows:
        out[
            (int(row["configured_members"]), str(row["target"]), str(row["metric"]))
        ] = row
    return out


def write_combined_json(args: argparse.Namespace,
                        run_files: Sequence[Path],
                        rows: Sequence[dict[str, Any]]) -> None:
    payload = {
        "config": {
            "target": args.target,
            "member_counts": args.member_counts,
            "remove_fraction": args.remove_fraction,
            "remove_members": args.remove_members,
            "read_ops": args.read_ops,
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
        "# Goblin Core Update Scaling Sweep",
        "",
        f"Generated: {generated_at}.",
        "",
        "This benchmark runs the Redis/Goblin zset harness across multiple set "
        "sizes with read operations disabled by default, so the report focuses "
        "on `ZADD` load throughput and `ZREM` removal throughput.",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- member counts: `{', '.join(f'{count:,}' for count in args.member_counts)}`",
        f"- remove fraction: `{args.remove_fraction}`",
        f"- absolute remove members: `{args.remove_members}`",
        f"- read operations per read metric: `{args.read_ops:,}`",
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
        "## Update Throughput",
        "",
        "| Members | Removed | Metric | Goblin/sec | Redis/sec | Goblin / Redis |",
        "| ---: | ---: | --- | ---: | ---: | ---: |",
    ])

    for members in args.member_counts:
        removed = configured_remove_members(args, members)
        for label, metric in UPDATE_METRICS:
            goblin = rows_by_key.get((members, "goblin", metric))
            redis = rows_by_key.get((members, "redis", metric))
            goblin_rate = None if goblin is None else float(goblin["per_second"])
            redis_rate = None if redis is None else float(redis["per_second"])
            ratio = None
            if goblin_rate is not None and redis_rate:
                ratio = goblin_rate / redis_rate
            lines.append(
                f"| {members:,} | {removed:,} | `{label}` | "
                f"{fmt_rate(goblin_rate)} | {fmt_rate(redis_rate)} | "
                f"{fmt_ratio(ratio)} |"
            )

    lines.extend([
        "",
        "## Memory",
        "",
        "| Members | Target | RSS delta MiB | RSS delta/member | Final RSS MiB | Redis used_memory MiB |",
        "| ---: | --- | ---: | ---: | ---: | ---: |",
    ])
    for members in args.member_counts:
        for target in ("goblin", "redis"):
            row = rows_by_key.get((members, target, "zadd_members"))
            final_row = rows_by_key.get((members, target, "zrem_members"))
            if row is None:
                continue
            used = row.get("redis_used_memory_mib")
            used_text = "n/a" if used is None else f"{float(used):.2f}"
            final_rss = row["rss_final_mib"] if final_row is None else final_row["rss_final_mib"]
            lines.append(
                f"| {members:,} | `{target}` | {float(row['rss_delta_mib']):.2f} | "
                f"{float(row['rss_delta_bytes_per_member']):.2f} B | "
                f"{float(final_rss):.2f} | {used_text} |"
            )

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(lines) + "\n")


def benchmark_command(args: argparse.Namespace,
                      members: int,
                      remove_members: int,
                      output: Path) -> list[object]:
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
        members,
        "--ops",
        args.read_ops,
        "--remove-members",
        remove_members,
        "--zadd-batch",
        args.zadd_batch,
        "--zrem-batch",
        args.zrem_batch,
        "--range-size",
        1,
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
    parser.add_argument("--member-counts", type=int, nargs="+",
                        default=[10_000, 100_000, 1_000_000])
    parser.add_argument("--remove-fraction", type=float, default=0.5)
    parser.add_argument("--remove-members", type=int)
    parser.add_argument("--read-ops", type=int, default=0)
    parser.add_argument("--score-shape", choices=zbench.SCORE_SHAPES, default="integer")
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--zrem-batch", type=int, default=128)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "update-scaling-sweep.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "update-scaling-sweep.md")
    parser.add_argument("--run-dir", type=Path)
    args = parser.parse_args(argv)

    if not args.member_counts or min(args.member_counts) <= 0:
        parser.error("member counts must be positive")
    if args.remove_members is not None and args.remove_members < 0:
        parser.error("remove-members must be non-negative")
    if not 0.0 <= args.remove_fraction <= 1.0:
        parser.error("remove-fraction must be between 0 and 1")
    if args.read_ops < 0:
        parser.error("read-ops must be non-negative")
    if min(args.pipeline, args.zadd_batch, args.zrem_batch) <= 0:
        parser.error("pipeline, zadd-batch, and zrem-batch must be positive")
    if args.run_dir is None:
        args.run_dir = args.output_json.parent / f"{args.output_json.stem}-runs"
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    args.run_dir.mkdir(parents=True, exist_ok=True)

    combined_rows: list[dict[str, Any]] = []
    run_files: list[Path] = []
    for members in args.member_counts:
        remove_members = configured_remove_members(args, members)
        output = args.run_dir / f"updates-{members}.json"
        run(benchmark_command(args, members, remove_members, output))
        run_files.append(output)
        combined_rows.extend(load_rows(output, members, remove_members))

    write_combined_json(args, run_files, combined_rows)
    write_report(args, run_files, combined_rows)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
