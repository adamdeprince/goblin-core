#!/usr/bin/env python3
"""Build Goblin Core, run scripted benchmarks, and write a Markdown report."""

from __future__ import annotations

import argparse
import json
import math
import platform
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


ROOT = Path(__file__).resolve().parents[1]
ZSET_BENCHMARK = ROOT / "benchmarks" / "zset_benchmark.py"
RANK_CACHE_MODES = ("off", "exact", "block-hint")


def run(command: Sequence[object], cwd: Path = ROOT) -> None:
    print("+ " + " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], cwd=cwd, check=True)


def format_int(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):,.0f}"


def format_float(value: float | int | None, digits: int = 2) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):,.{digits}f}"


def format_ratio(value: float | int | None) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):.2f}x"


def format_percent(value: float | int | None, digits: int = 1) -> str:
    if value is None:
        return "n/a"
    return f"{float(value):,.{digits}f}%"


def format_unit(value: float | int | None, unit: str, digits: int = 2) -> str:
    if value is None:
        return "n/a"
    return f"{format_float(value, digits)} {unit}"


def load_results(path: Path) -> dict[tuple[str, str], dict[str, Any]]:
    rows = json.loads(path.read_text())
    return {(row["target"], row["metric"]): row for row in rows}


def load_payload(path: Path) -> dict[str, Any] | None:
    if not path.exists():
        return None
    return json.loads(path.read_text())


def result_path(path: Path, report: Path) -> str:
    path = path.resolve()
    report_parent = report.resolve().parent
    try:
        return str(path.relative_to(report_parent))
    except ValueError:
        return str(path)


def metric_table(results: dict[tuple[str, str], dict[str, Any]]) -> str:
    metrics = [
        ("ZADD members", "zadd_members"),
        ("ZSCORE ops", "zscore_ops"),
        ("ZRANK ops", "zrank_ops"),
        ("ZREVRANK ops", "zrevrank_ops"),
        ("ZRANGE ops", "zrange_ops"),
        ("ZRANGE WITHSCORES ops", "zrange_withscores_ops"),
        ("ZREVRANGE ops", "zrevrange_ops"),
        ("ZREVRANGE WITHSCORES ops", "zrevrange_withscores_ops"),
        ("ZREM members", "zrem_members"),
    ]
    lines = [
        "| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, metric in metrics:
        goblin = results.get(("goblin", metric))
        redis = results.get(("redis", metric))
        goblin_rate = None if goblin is None else goblin["per_second"]
        redis_rate = None if redis is None else redis["per_second"]
        ratio = None
        if goblin_rate is not None and redis_rate:
            ratio = goblin_rate / redis_rate
        lines.append(
            f"| `{label}` | {format_int(goblin_rate)} | "
            f"{format_int(redis_rate)} | {format_ratio(ratio)} |"
        )
    return "\n".join(lines)


def memory_table(results: dict[tuple[str, str], dict[str, Any]]) -> str:
    goblin = results.get(("goblin", "zadd_members"))
    redis = results.get(("redis", "zadd_members"))
    lines = [
        "| Memory metric | Goblin Core | Redis |",
        "| --- | ---: | ---: |",
        "| RSS delta | "
        f"{format_unit(None if goblin is None else goblin['rss_delta_mib'], 'MiB')} | "
        f"{format_unit(None if redis is None else redis['rss_delta_mib'], 'MiB')} |",
        "| RSS delta per loaded member | "
        f"{format_unit(None if goblin is None else goblin['rss_delta_bytes_per_member'], 'B')} | "
        f"{format_unit(None if redis is None else redis['rss_delta_bytes_per_member'], 'B')} |",
        "| Redis `used_memory` | n/a | "
        f"{format_unit(None if redis is None else redis['redis_used_memory_mib'], 'MiB')} |",
        "| Redis `used_memory` per member | n/a | "
        f"{format_unit(None if redis is None else redis['redis_used_bytes_per_member'], 'B')} |",
    ]
    return "\n".join(lines)


def latency_table(results: dict[tuple[str, str], dict[str, Any]]) -> str:
    metrics = [
        ("ZSCORE", "zscore_latency"),
        ("ZRANK", "zrank_latency"),
        ("ZREVRANK", "zrevrank_latency"),
        ("ZRANGE", "zrange_latency"),
        ("ZREVRANGE", "zrevrange_latency"),
    ]
    if not any(("goblin", metric) in results or ("redis", metric) in results
               for _, metric in metrics):
        return ""

    lines = [
        "| Metric | Target | p50 | p95 | p99 | max |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    for label, metric in metrics:
        for target in ("goblin", "redis"):
            row = results.get((target, metric))
            if row is None:
                continue
            lines.append(
                f"| `{label}` | {target} | "
                f"{format_unit(row.get('latency_p50_us'), 'us')} | "
                f"{format_unit(row.get('latency_p95_us'), 'us')} | "
                f"{format_unit(row.get('latency_p99_us'), 'us')} | "
                f"{format_unit(row.get('latency_max_us'), 'us')} |"
            )
    return "\n".join(lines)


def goblin_allocation_table(result: dict[str, Any] | None) -> str:
    rows = [
        ("member storage", "goblin_member_storage_allocated_mib"),
        ("member index", "goblin_member_index_allocated_mib"),
        ("score index", "goblin_score_index_allocated_mib"),
        ("score-string cache", "goblin_score_string_cache_allocated_mib"),
        ("rank-location cache", "goblin_rank_location_cache_allocated_mib"),
        ("total tracked zset allocation", "goblin_total_allocated_mib"),
    ]
    lines = ["| Component | Allocated |", "| --- | ---: |"]
    for label, key in rows:
        value = None if result is None else result.get(key)
        lines.append(f"| {label} | {format_unit(value, 'MiB')} |")
    return "\n".join(lines)


def rank_cache_mode_comparison_table(
    mode_results: dict[str, dict[tuple[str, str], dict[str, Any]]],
) -> str:
    metrics = [
        ("ZADD members", "zadd_members"),
        ("ZSCORE ops", "zscore_ops"),
        ("ZRANK ops", "zrank_ops"),
        ("ZREVRANK ops", "zrevrank_ops"),
        ("ZRANGE ops", "zrange_ops"),
        ("ZRANGE WITHSCORES ops", "zrange_withscores_ops"),
        ("ZREVRANGE ops", "zrevrange_ops"),
        ("ZREVRANGE WITHSCORES ops", "zrevrange_withscores_ops"),
        ("ZREM members", "zrem_members"),
    ]
    modes = [mode for mode in RANK_CACHE_MODES if mode in mode_results]
    lines = [
        "| Metric | " + " | ".join(f"{mode} ops/sec" for mode in modes) +
        " | exact vs off | block-hint vs off |",
        "| --- | " + " | ".join("---:" for _ in modes) + " | ---: | ---: |",
    ]
    for label, metric in metrics:
        rates: dict[str, float | None] = {}
        for mode in modes:
            row = mode_results[mode].get(("goblin", metric))
            rates[mode] = None if row is None else row["per_second"]
        off_rate = rates.get("off")

        def change(mode: str) -> float | None:
            rate = rates.get(mode)
            if off_rate and rate is not None:
                return (rate / off_rate - 1.0) * 100.0
            return None

        lines.append("| `" + label + "` | " +
                     " | ".join(format_int(rates[mode]) for mode in modes) +
                     f" | {format_percent(change('exact'))} | "
                     f"{format_percent(change('block-hint'))} |")
    return "\n".join(lines)


def rank_cache_mode_allocation_table(
    mode_results: dict[str, dict[tuple[str, str], dict[str, Any]]],
    members: int,
) -> str:
    lines = [
        "| Mode | Rank cache MiB | Rank cache B/member | RSS delta B/member |",
        "| --- | ---: | ---: | ---: |",
    ]
    for mode in RANK_CACHE_MODES:
        results = mode_results.get(mode)
        row = None if results is None else results.get(("goblin", "zadd_members"))
        cache_mib = None if row is None else row.get(
            "goblin_rank_location_cache_allocated_mib"
        )
        cache_bpm = None
        if cache_mib is not None and members > 0:
            cache_bpm = cache_mib * 1024.0 * 1024.0 / members
        rss_bpm = None if row is None else row.get("rss_delta_bytes_per_member")
        lines.append(
            f"| `{mode}` | {format_float(cache_mib)} | "
            f"{format_float(cache_bpm)} | {format_float(rss_bpm)} |"
        )
    return "\n".join(lines)


def post_delete_section(json_path: Path, report_path: Path, output: Path) -> list[str]:
    payload = load_payload(json_path)
    if payload is None:
        return []
    results = {
        (row["target"], row["metric"]): row
        for row in payload.get("results", [])
    }
    config = payload.get("config", {})
    removed_members = config.get("remove_members")
    if removed_members is None:
        remove_row = results.get(("goblin", "zrem_members")) or results.get(
            ("redis", "zrem_members")
        )
        removed_members = None if remove_row is None else remove_row.get("removed_members")
    metrics = [
        ("ZREM members", "zrem_members"),
        ("ZSCORE ops", "zscore_ops"),
        ("ZRANK ops", "zrank_ops"),
        ("ZREVRANK ops", "zrevrank_ops"),
        ("ZRANGE ops", "zrange_ops"),
        ("ZRANGE WITHSCORES ops", "zrange_withscores_ops"),
        ("ZREVRANGE ops", "zrevrange_ops"),
        ("ZREVRANGE WITHSCORES ops", "zrevrange_withscores_ops"),
    ]

    lines = [
        "",
        "## Post-Delete Reads",
        "",
        "This run loads members, removes a configured fraction, then measures "
        "reads against the remaining members.",
        "",
        f"Source data: `{result_path(json_path, output)}`",
        "",
        f"Detailed report: `{result_path(report_path, output)}`",
        "",
        f"- loaded members: `{int(config.get('members', 0)):,}`",
        f"- removed members: `{int(removed_members or 0):,}`",
        f"- remove order: `{config.get('remove_order', 'n/a')}`",
        f"- range size: `{config.get('range_size', 'n/a')}`",
        "",
        "| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, metric in metrics:
        goblin = results.get(("goblin", metric))
        redis = results.get(("redis", metric))
        goblin_rate = None if goblin is None else goblin["per_second"]
        redis_rate = None if redis is None else redis["per_second"]
        ratio = None if goblin_rate is None or not redis_rate else goblin_rate / redis_rate
        lines.append(
            f"| `{label}` | {format_int(goblin_rate)} | "
            f"{format_int(redis_rate)} | {format_ratio(ratio)} |"
        )

    goblin_remove = results.get(("goblin", "zrem_members"))
    tombstones = None
    if goblin_remove is not None:
        final_memory = goblin_remove.get("goblin_memory_final") or {}
        tombstones = final_memory.get("member_index_tombstones")
    if tombstones is not None:
        lines.extend([
            "",
            f"Goblin Core member-index tombstones after removal: `{tombstones:,}`.",
        ])
    return lines


def mixed_leaderboard_section(json_path: Path,
                              report_path: Path,
                              output: Path) -> list[str]:
    payload = load_payload(json_path)
    if payload is None:
        return []
    rows = {row["target"]: row for row in payload.get("results", [])}
    config = payload.get("config", {})
    goblin = rows.get("goblin")
    redis = rows.get("redis")

    def ratio(key: str) -> float | None:
        if goblin is None or redis is None or not redis.get(key):
            return None
        return float(goblin[key]) / float(redis[key])

    lines = [
        "",
        "## Mixed Leaderboard Workload",
        "",
        "This run preloads one zset and runs an interleaved leaderboard workload "
        "with reads, top ranges, score updates, and remove/add churn.",
        "",
        f"Source data: `{result_path(json_path, output)}`",
        "",
        f"Detailed report: `{result_path(report_path, output)}`",
        "",
        f"- loaded members: `{int(config.get('members', 0)):,}`",
        f"- logical mixed operations: `{int(config.get('ops', 0)):,}`",
        f"- range size: `{config.get('range_size', 'n/a')}`",
        "",
        "| Metric | Goblin Core | Redis | Goblin Core / Redis |",
        "| --- | ---: | ---: | ---: |",
        f"| `Load members/sec` | "
        f"{format_int(None if goblin is None else goblin.get('load_members_per_second'))} | "
        f"{format_int(None if redis is None else redis.get('load_members_per_second'))} | "
        f"{format_ratio(ratio('load_members_per_second'))} |",
        f"| `Mixed logical ops/sec` | "
        f"{format_int(None if goblin is None else goblin.get('logical_ops_per_second'))} | "
        f"{format_int(None if redis is None else redis.get('logical_ops_per_second'))} | "
        f"{format_ratio(ratio('logical_ops_per_second'))} |",
        f"| `Mixed RESP commands/sec` | "
        f"{format_int(None if goblin is None else goblin.get('resp_commands_per_second'))} | "
        f"{format_int(None if redis is None else redis.get('resp_commands_per_second'))} | "
        f"{format_ratio(ratio('resp_commands_per_second'))} |",
    ]

    if goblin is not None and redis is not None:
        lines.extend([
            "",
            "Mixed workload latency:",
            "",
            "| Target | p50 | p95 | p99 | max |",
            "| --- | ---: | ---: | ---: | ---: |",
        ])
        for row in (goblin, redis):
            lines.append(
                f"| `{row['target']}` | "
                f"{format_unit(row.get('latency_p50_us'), 'us')} | "
                f"{format_unit(row.get('latency_p95_us'), 'us')} | "
                f"{format_unit(row.get('latency_p99_us'), 'us')} | "
                f"{format_unit(row.get('latency_max_us'), 'us')} |"
            )
        final_memory = goblin.get("goblin_memory_final") or {}
        tombstones = final_memory.get("member_index_tombstones")
        if tombstones is not None:
            lines.extend([
                "",
                f"Goblin Core member-index tombstones after the mixed run: "
                f"`{tombstones:,}`.",
            ])
    return lines


def range_output_section(json_path: Path,
                         report_path: Path,
                         output: Path) -> list[str]:
    payload = load_payload(json_path)
    if payload is None:
        return []
    rows = {
        (int(row["range_size"]), row["target"], row["metric"]): row
        for row in payload.get("results", [])
    }
    config = payload.get("config", {})
    sizes = sorted({key[0] for key in rows})
    metrics = [
        ("ZRANGE", "zrange"),
        ("ZREVRANGE", "zrevrange"),
        ("ZRANGE WITHSCORES", "zrange_withscores"),
        ("ZREVRANGE WITHSCORES", "zrevrange_withscores"),
    ]

    lines = [
        "",
        "## Cold Range Output",
        "",
        "This no-warmup range-output sweep isolates first-burst response-buffer "
        "growth after geometric range-output reserve. Goblin Core uses the "
        "default `0` KiB initial output buffer.",
        "",
        f"Source data: `{result_path(json_path, output)}`",
        "",
        f"Detailed report: `{result_path(report_path, output)}`",
        "",
        f"- range commands per metric: `{int(config.get('ops', 0)):,}`",
        f"- warmup commands per metric: `{int(config.get('warmup_ops', 0)):,}`",
        f"- pipeline depth: `{config.get('pipeline', 'n/a')}`",
        f"- read delay per burst: `{config.get('read_delay_ms', 'n/a')}` ms",
        f"- Goblin initial output buffer: "
        f"`{config.get('goblin_initial_output_buffer_kib', 'n/a')}` KiB",
        "",
        "| Range size | Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |",
        "| ---: | --- | ---: | ---: | ---: |",
    ]
    for size in sizes:
        for label, metric in metrics:
            goblin = rows.get((size, "goblin", metric))
            redis = rows.get((size, "redis", metric))
            goblin_rate = None if goblin is None else goblin["ops_per_second"]
            redis_rate = None if redis is None else redis["ops_per_second"]
            ratio = None if goblin_rate is None or not redis_rate else goblin_rate / redis_rate
            lines.append(
                f"| {size} | `{label}` | {format_int(goblin_rate)} | "
                f"{format_int(redis_rate)} | {format_ratio(ratio)} |"
            )
    return lines


def prefixed_rank_cache_paths(prefix: Path) -> dict[str, Path]:
    return {
        "off": prefix.parent / f"{prefix.name}-rank-cache-off.json",
        "exact": prefix.parent / f"{prefix.name}-rank-cache-exact.json",
        "block-hint": prefix.parent / f"{prefix.name}-rank-cache-block-hint.json",
    }


def geomean_speedup(results: dict[tuple[str, str], dict[str, Any]]) -> float | None:
    metrics = [
        "zadd_members",
        "zscore_ops",
        "zrank_ops",
        "zrevrank_ops",
        "zrange_ops",
        "zrange_withscores_ops",
        "zrevrange_ops",
        "zrevrange_withscores_ops",
        "zrem_members",
    ]
    ratios = []
    for metric in metrics:
        goblin = results.get(("goblin", metric))
        redis = results.get(("redis", metric))
        if goblin is None or redis is None or not redis.get("per_second"):
            return None
        ratios.append(float(goblin["per_second"]) / float(redis["per_second"]))
    return math.prod(ratios) ** (1.0 / len(ratios))


def linux_benchmark_section(prefix: Path,
                            label: str,
                            report: Path) -> list[str]:
    paths = prefixed_rank_cache_paths(prefix)
    if not paths["off"].exists():
        return []
    mode_results = {
        mode: load_results(path)
        for mode, path in paths.items()
        if path.exists()
    }
    default_results = mode_results["off"]
    default_goblin = default_results.get(("goblin", "zadd_members"))
    default_redis = default_results.get(("redis", "zadd_members"))
    members = 0 if default_goblin is None else int(default_goblin.get("count", 0))

    rss_ratio = None
    if default_goblin and default_redis and default_redis["rss_delta_bytes_per_member"]:
        rss_ratio = (
            default_goblin["rss_delta_bytes_per_member"] /
            default_redis["rss_delta_bytes_per_member"]
        )
    geomean = geomean_speedup(default_results)
    summary = []
    if geomean is not None:
        summary.append(f"`{format_ratio(geomean)}` geomean throughput vs Redis")
    if rss_ratio is not None:
        summary.append(f"`{format_percent(rss_ratio * 100.0)}` of Redis process RSS")

    lines = [
        "",
        "## Linux Results",
        "",
        f"Deployment-oriented run: `{label}` — the primary benchmark context "
        "for Linux users. The current-host section below is a local development "
        "baseline.",
    ]
    if summary:
        lines.extend([
            "",
            "Headline: Goblin Core is " + " and ".join(summary) + ".",
        ])
    lines.extend([
        "",
        "Source data:",
        "",
        *[
            f"- `{mode}`: `{result_path(paths[mode], report)}`"
            for mode in RANK_CACHE_MODES
            if mode in mode_results
        ],
        "",
        "Linux default configuration: rank cache mode `off`; score-string cache "
        "`False`.",
        "",
        metric_table(default_results),
        "",
        memory_table(default_results),
        "",
        "Latency percentiles:",
        "",
        latency_table(default_results) or "Latency mode was disabled.",
    ])
    if len(mode_results) > 1:
        lines.extend([
            "",
            "Linux rank-cache mode comparison:",
            "",
            rank_cache_mode_comparison_table(mode_results),
            "",
            "Linux rank-cache allocation by mode:",
            "",
            rank_cache_mode_allocation_table(mode_results, members),
        ])
    return lines


def benchmark_instructions_section() -> list[str]:
    return [
        "",
        "## Running These Benchmarks",
        "",
        "Redis is required for comparison runs. The top-level benchmark workflow "
        "configures a release build, runs the test suite, starts Goblin Core and "
        "Redis on temporary localhost ports, drives both over RESP, and writes "
        "JSON artifacts plus this Markdown report.",
        "",
        "Full benchmark report:",
        "",
        "```sh",
        "python3 benchmarks/run_benchmarks.py \\",
        "  --redis-server /path/to/redis-server \\",
        "  --report BENCHMARKS.md \\",
        "  --name redis-goblin-core-1m-modes \\",
        "  --latency-samples 10000",
        "```",
        "",
        "`--latency-samples` enables per-command latency rows for `ZSCORE`, "
        "`ZRANK`, `ZREVRANK`, `ZRANGE`, and `ZREVRANGE`. Latency sampling "
        "defaults to single-command round trips with `--latency-pipeline-depth "
        "1` so percentiles are not hidden by the throughput pipeline.",
        "",
        "CI-style smoke benchmark:",
        "",
        "```sh",
        "scripts/benchmark_smoke.sh",
        "```",
        "",
        "The smoke script writes `benchmark-results/ci-smoke.md`, regenerates "
        "the static HTML docs, and accepts environment overrides such as "
        "`BENCHMARK_MEMBERS`, `BENCHMARK_OPS`, `BENCHMARK_LATENCY_SAMPLES`, "
        "`REDIS_SERVER`, and `SKIP_BUILD=1`.",
        "",
        "Quick Goblin-only validation:",
        "",
        "```sh",
        "python3 benchmarks/run_benchmarks.py \\",
        "  --target goblin \\",
        "  --members 10000 \\",
        "  --ops 10000 \\",
        "  --remove-members 5000 \\",
        "  --latency-samples 1000 \\",
        "  --rank-cache-modes off",
        "```",
        "",
        "Targeted server harness:",
        "",
        "```sh",
        "python3 benchmarks/zset_benchmark.py \\",
        "  --target goblin \\",
        "  --goblin-rank-cache-mode off",
        "```",
        "",
        "Use `--goblin-rank-cache-mode off|exact|block-hint` to benchmark a "
        "specific rank-cache mode. Add `--goblin-score-string-cache` to measure "
        "the optional cached score-output path. Add `--score-shape "
        "integer|short-decimal|long-decimal|random-double` to vary score "
        "serialization behavior.",
        "",
        "In-process microbenchmark:",
        "",
        "```sh",
        "cmake --build build-release --target goblin_core_microbench",
        "./build-release/goblin_core_microbench \\",
        "  --members 1000000 \\",
        "  --ops 1000000 \\",
        "  --score-shape integer \\",
        "  --format json \\",
        "  --output benchmark-results/microbench.json",
        "```",
        "",
        "Regenerate the microbenchmark summary after running the rank-cache "
        "modes:",
        "",
        "```sh",
        "python3 benchmarks/report_microbench.py --output MICROBENCHMARKS.md",
        "```",
        "",
        "Server-bound throughput and RSS-per-member via `redis-benchmark` (C "
        "client; the Python harness above is client-bound for fast reads):",
        "",
        "```sh",
        "python3 benchmarks/redis_benchmark_speed.py \\",
        "  --goblin-bin build-release/goblin-core \\",
        "  --redis-server \"$(command -v redis-server)\" \\",
        "  --members 1000000 --requests 2000000 --rounds 3 \\",
        "  --server-cpu 0 --client-cpu 1",
        "```",
        "",
        "Supplemental benchmark scripts:",
        "",
        "```sh",
        "python3 benchmarks/range_output_benchmark.py \\",
        "  --output-json benchmark-results/range-output.json \\",
        "  --report benchmark-results/range-output.md",
        "",
        "python3 benchmarks/range_size_sweep.py \\",
        "  --score-shape integer \\",
        "  --range-sizes 1 4 16 64 256 1024 \\",
        "  --output-json benchmark-results/range-size-sweep.json \\",
        "  --report benchmark-results/range-size-sweep.md",
        "",
        "python3 benchmarks/range_output_sweep.py \\",
        "  --score-shape integer \\",
        "  --range-sizes 16 64 256 1024 \\",
        "  --warmup-ops 4096 \\",
        "  --output-json benchmark-results/range-output-sweep.json \\",
        "  --report benchmark-results/range-output-sweep.md",
        "",
        "python3 benchmarks/update_scaling_sweep.py \\",
        "  --member-counts 10000 100000 1000000 \\",
        "  --remove-fraction 0.5 \\",
        "  --output-json benchmark-results/update-scaling-sweep.json \\",
        "  --report benchmark-results/update-scaling-sweep.md",
        "",
        "python3 benchmarks/zrem_shape_sweep.py \\",
        "  --member-counts 50000 100000 200000 \\",
        "  --remove-fractions 0.01 0.1 0.5 0.9 \\",
        "  --remove-orders load-prefix load-suffix reshuffled \\",
        "  --output-json benchmark-results/zrem-shape-sweep.json \\",
        "  --report benchmark-results/zrem-shape-sweep.md",
        "",
        "python3 benchmarks/post_delete_read_benchmark.py \\",
        "  --members 1000000 \\",
        "  --ops 1000000 \\",
        "  --remove-fraction 0.5 \\",
        "  --latency-samples 10000 \\",
        "  --output-json benchmark-results/post-delete-read.json \\",
        "  --report benchmark-results/post-delete-read.md",
        "",
        "python3 benchmarks/mixed_leaderboard_benchmark.py \\",
        "  --members 1000000 \\",
        "  --ops 1000000 \\",
        "  --range-size 100 \\",
        "  --latency-samples 10000 \\",
        "  --output-json benchmark-results/mixed-leaderboard.json \\",
        "  --report benchmark-results/mixed-leaderboard.md",
        "```",
    ]


def write_report(mode_jsons: dict[str, Path],
                 report: Path,
                 args: argparse.Namespace) -> None:
    if "off" not in mode_jsons:
        raise ValueError("rank-cache mode report requires an off-mode artifact")
    mode_results = {
        mode: load_results(path)
        for mode, path in mode_jsons.items()
        if path.exists()
    }
    if "off" not in mode_results:
        raise FileNotFoundError(mode_jsons["off"])
    default_json = mode_jsons["off"]
    default_results = load_results(default_json)
    default_goblin = default_results.get(("goblin", "zadd_members"))
    default_redis = default_results.get(("redis", "zadd_members"))
    rss_ratio = None
    if default_goblin and default_redis and default_redis["rss_delta_bytes_per_member"]:
        rss_ratio = (
            default_goblin["rss_delta_bytes_per_member"] /
            default_redis["rss_delta_bytes_per_member"]
        )

    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    report.parent.mkdir(parents=True, exist_ok=True)

    content = [
        "# Goblin Core Benchmarks",
        "",
        f"Generated: {generated_at}.",
        "",
        "These results compare Goblin Core against Redis for the current "
        "sorted-set-focused implementation. Goblin Core's optional rank-location "
        "cache and score-string cache are off by default.",
        "",
        "## Methodology",
        "",
        "The benchmark starts each server on a temporary localhost port, drives "
        "both over RESP, and records throughput plus process RSS. Redis also "
        "reports `INFO memory used_memory`; Goblin Core reports internal zset "
        "allocation and the active `rank_cache_mode` through `GOBLIN.MEMORY`.",
        "",
        "Both servers are measured after a load-then-serve sequence: Goblin Core "
        "runs `GOBLIN.OPTIMIZE` at a high packing density (default `0.97`) after "
        "loading, before memory and reads are measured, reflecting how a "
        "read-mostly deployment would compact before serving. Redis has no "
        "equivalent step. Pass `--goblin-optimize-density none` to measure the "
        "un-optimized (as-loaded) state instead.",
        "",
        "Memory is the primary comparison; throughput is secondary. Single-member "
        "read throughput (`ZSCORE`, `ZRANK`, `ZREVRANK`) is driven by "
        "`redis-benchmark`, a C load generator, because one Python pipelined "
        "connection is client-bound near ~350K ops/sec and would measure the "
        "client, not the server. Batched/range throughput rows remain "
        "Python-driven and are similarly client-bound; process RSS and "
        "`used_memory` are unaffected by client speed. Pass `--redis-benchmark "
        "none` to force the client-bound Python read path.",
        "",
        "Host:",
        "",
        f"- {platform.platform()} {platform.machine()}",
        f"- Python {platform.python_version()}",
        "- Release build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`",
        "",
        "Workload:",
        "",
        f"- loaded members: `{args.members:,}`",
        f"- read/range ops: `{args.ops:,}`",
        f"- removed members: `{args.remove_members:,}`",
        f"- `ZADD` batch size: `{args.zadd_batch}`",
        f"- `ZREM` batch size: `{args.zrem_batch}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- latency samples: `{args.latency_samples:,}`",
        f"- latency warmup per metric: `{args.latency_warmup:,}`",
        f"- latency pipeline depth: `{args.latency_pipeline_depth}`",
        f"- `ZRANGE` size: `{args.range_size}`",
        f"- score shape: `{args.score_shape}`",
        f"- Goblin score-string cache: `{args.goblin_score_string_cache}`",
        f"- seed: `{args.seed}`",
    ]

    if not args.skip_supplemental:
        content.extend(linux_benchmark_section(
            args.linux_benchmark_prefix,
            args.linux_benchmark_label,
            report,
        ))

    content.extend([
        "",
        "## Current Host Default Configuration",
        "",
        "Goblin Core default run: rank cache mode `off`; "
        f"score-string cache `{args.goblin_score_string_cache}`.",
        "",
        f"Source data: `{result_path(default_json, report)}`",
        "",
        metric_table(default_results),
        "",
        memory_table(default_results),
        "",
        "Latency percentiles:",
        "",
        latency_table(default_results) or "Latency mode was disabled.",
        "",
        "Goblin Core tracked zset allocation:",
        "",
        goblin_allocation_table(default_goblin),
        "",
        "RSS includes allocator retained pages, executable/runtime mappings, "
        "and other process overhead, so it is expected to exceed tracked zset "
        "allocation.",
    ])

    if len(mode_results) > 1:
        content.extend([
            "",
            "## Rank Cache Modes",
            "",
            "`--rank-cache-mode exact` enables the packed member-id-to-score-location "
            "cache. `--rank-cache-mode block-hint` stores only member-to-score-block "
            "hints, reducing write maintenance while retaining a smaller `ZRANK` "
            "read assist. Block hints start as 16-bit ids and promote to 32-bit "
            "ids automatically if the block-id space grows beyond the narrow "
            "encoding.",
            "",
            "Source data:",
            "",
            *[
                f"- `{mode}`: `{result_path(mode_jsons[mode], report)}`"
                for mode in RANK_CACHE_MODES
                if mode in mode_results
            ],
            "",
            rank_cache_mode_comparison_table(mode_results),
            "",
            "Rank-cache allocation by mode:",
            "",
            rank_cache_mode_allocation_table(mode_results, args.members),
            "",
            "Exact mode is the read-heavy `ZRANK` option; block-hint mode is the "
            "churn-heavy leaderboard option.",
        ])

    for mode in ("exact", "block-hint"):
        results = mode_results.get(mode)
        path = mode_jsons.get(mode)
        if results is None or path is None:
            continue
        content.extend([
            "",
            f"### Rank Cache Mode: `{mode}`",
            "",
            f"Source data: `{result_path(path, report)}`",
            "",
            metric_table(results),
            "",
            "Latency percentiles:",
            "",
            latency_table(results) or "Latency mode was disabled.",
        ])

    if rss_ratio is None:
        interpretation = (
            "This run did not include enough Redis and Goblin Core memory data to "
            "compute a process RSS ratio."
        )
    else:
        interpretation = (
            "For the default configuration, Goblin Core uses "
            f"about {format_percent(rss_ratio * 100.0)} of Redis process RSS "
            "on this workload."
        )
    if not args.skip_supplemental:
        content.extend(post_delete_section(
            args.post_delete_json,
            args.post_delete_report,
            report,
        ))
        content.extend(mixed_leaderboard_section(
            args.mixed_json,
            args.mixed_report,
            report,
        ))
        content.extend(range_output_section(
            args.range_output_json,
            args.range_output_report,
            report,
        ))
    content.extend([
        "",
        "## Interpretation",
        "",
        interpretation,
        "",
        "The rank cache is intentionally a command-line choice, not the default "
        "policy. Enable it only after measuring a workload where `ZRANK` "
        "latency dominates write throughput.",
        "",
    ])
    content.extend(benchmark_instructions_section())

    report.write_text("\n".join(content))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-release")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "benchmark-results")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "benchmark-report.md")
    parser.add_argument("--redis-server", type=Path,
                        default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--redis-benchmark", type=str,
                        default=shutil.which("redis-benchmark") or "none",
                        help="Path to redis-benchmark (C load generator) used for "
                             "single-member read throughput. 'none' forces the "
                             "client-bound Python read path.")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--report-only", action="store_true",
                        help="Regenerate the Markdown report from existing JSON artifacts.")
    parser.add_argument(
        "--skip-supplemental",
        action="store_true",
        help="Omit post-delete, mixed-workload, and range-output sections from the report.",
    )
    parser.add_argument(
        "--linux-benchmark-prefix",
        type=Path,
        default=ROOT / "benchmark-results" / "linux-1m",
        help="Prefix for optional Linux deployment benchmark artifacts.",
    )
    parser.add_argument(
        "--linux-benchmark-label",
        default="Ubuntu 26.04, Intel Xeon 6975P-C, Redis 8.0.5, GCC 16.1.0",
        help="Human-readable label for the optional Linux benchmark section.",
    )
    parser.add_argument(
        "--skip-rank-cache",
        action="store_true",
        help="Compatibility alias for --rank-cache-modes off.",
    )
    parser.add_argument(
        "--rank-cache-modes",
        choices=RANK_CACHE_MODES,
        nargs="+",
        default=list(RANK_CACHE_MODES),
        help="Goblin rank-cache modes to benchmark and report.",
    )
    parser.add_argument("--goblin-optimize-density", type=str, default="0.97",
                        help="GOBLIN.OPTIMIZE member-index packing density applied "
                             "after loading, before measuring. 'none' skips it.")
    parser.add_argument("--goblin-score-string-cache", action="store_true",
                        help="Start Goblin Core with cached RESP score strings.")
    parser.add_argument("--target", choices=["both", "goblin", "redis"], default="both")
    parser.add_argument("--members", type=int, default=1_000_000)
    parser.add_argument("--ops", type=int, default=1_000_000)
    parser.add_argument("--remove-members", type=int, default=500_000)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--zrem-batch", type=int, default=128)
    parser.add_argument("--range-size", type=int, default=16)
    parser.add_argument(
        "--score-shape",
        choices=("integer", "short-decimal", "long-decimal", "random-double"),
        default="integer",
    )
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--latency-samples", type=int, default=0)
    parser.add_argument("--latency-warmup", type=int, default=100)
    parser.add_argument("--latency-pipeline-depth", type=int, default=1)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--name", default=None,
                        help="Output file prefix. Defaults to a UTC timestamp.")
    parser.add_argument(
        "--post-delete-json",
        type=Path,
        default=ROOT / "benchmark-results" / "post-delete-read-1m.json",
    )
    parser.add_argument(
        "--post-delete-report",
        type=Path,
        default=ROOT / "benchmark-results" / "post-delete-read-1m.md",
    )
    parser.add_argument(
        "--mixed-json",
        type=Path,
        default=ROOT / "benchmark-results" / "mixed-leaderboard-1m-r100.json",
    )
    parser.add_argument(
        "--mixed-report",
        type=Path,
        default=ROOT / "benchmark-results" / "mixed-leaderboard-1m-r100.md",
    )
    parser.add_argument(
        "--range-output-json",
        type=Path,
        default=ROOT / "benchmark-results" / "range-output-geometric-reserve-r4-r16.json",
    )
    parser.add_argument(
        "--range-output-report",
        type=Path,
        default=ROOT / "benchmark-results" / "range-output-geometric-reserve-r4-r16.md",
    )
    args = parser.parse_args(argv)
    if args.skip_rank_cache:
        args.rank_cache_modes = ["off"]
    if "off" not in args.rank_cache_modes:
        args.rank_cache_modes.insert(0, "off")
    args.rank_cache_modes = [
        mode for mode in RANK_CACHE_MODES if mode in set(args.rank_cache_modes)
    ]
    return args


def benchmark_command(args: argparse.Namespace,
                      goblin_bin: Path,
                      output: Path,
                      rank_cache_mode: str) -> list[object]:
    command: list[object] = [
        sys.executable,
        ZSET_BENCHMARK,
        "--target",
        args.target,
        "--goblin-bin",
        goblin_bin,
        "--redis-server",
        args.redis_server,
        "--redis-benchmark",
        args.redis_benchmark,
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
        args.range_size,
        "--score-shape",
        args.score_shape,
        "--pipeline",
        args.pipeline,
        "--latency-samples",
        args.latency_samples,
        "--latency-warmup",
        args.latency_warmup,
        "--latency-pipeline-depth",
        args.latency_pipeline_depth,
        "--seed",
        args.seed,
        "--timeout",
        args.timeout,
        "--settle-seconds",
        args.settle_seconds,
        "--output",
        output,
        "--format",
        "json",
    ]
    command.extend(["--goblin-rank-cache-mode", rank_cache_mode])
    command.extend(["--goblin-optimize-density", str(args.goblin_optimize_density)])
    if args.goblin_score_string_cache:
        command.append("--goblin-score-string-cache")
    return command


def mode_output_paths(args: argparse.Namespace, output_dir: Path) -> dict[str, Path]:
    prefix = args.name or datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
    paths: dict[str, Path] = {}
    for mode in args.rank_cache_modes:
        if mode == "off":
            paths[mode] = output_dir / f"{prefix}-rank-cache-off.json"
        else:
            paths[mode] = output_dir / f"{prefix}-rank-cache-{mode}.json"
    return paths


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    build_dir = args.build_dir
    goblin_bin = build_dir / "goblin-core"
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    mode_jsons = mode_output_paths(args, output_dir)

    if args.report_only:
        write_report(mode_jsons, args.report, args)
        print(f"wrote {args.report}")
        return 0

    if not args.skip_build:
        run([
            "cmake",
            "-S",
            ROOT,
            "-B",
            build_dir,
            "-DCMAKE_BUILD_TYPE=Release",
            "-DGOBLIN_CORE_BUILD_TESTS=ON",
        ])
        run(["cmake", "--build", build_dir])
        run(["ctest", "--test-dir", build_dir, "--output-on-failure"])

    for mode, output in mode_jsons.items():
        run(benchmark_command(args, goblin_bin, output, rank_cache_mode=mode))

    write_report(mode_jsons, args.report, args)
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
