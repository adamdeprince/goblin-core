#!/usr/bin/env python3
"""Write a Markdown report from Goblin Core microbenchmark JSON files."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


ROOT = Path(__file__).resolve().parents[1]


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text())


def index_results(data: dict[str, Any]) -> dict[tuple[str, str], dict[str, Any]]:
    return {
        (str(row["category"]), str(row["metric"])): row
        for row in data["results"]
    }


def fmt_ns(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:,.2f}"


def fmt_rate(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:,.0f}"


def fmt_percent(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:,.1f}%"


def ns(results: dict[tuple[str, str], dict[str, Any]],
       category: str,
       metric: str) -> float | None:
    row = results.get((category, metric))
    return None if row is None else float(row["ns_per_op"])


def rate(results: dict[tuple[str, str], dict[str, Any]],
         category: str,
         metric: str) -> float | None:
    row = results.get((category, metric))
    return None if row is None else float(row["ops_per_second"])


def pct_change(old: float | None, new: float | None) -> float | None:
    if old is None or new is None or old == 0:
        return None
    return (new / old - 1.0) * 100.0


def result_path(path: Path, report: Path) -> str:
    path = path.resolve()
    report_parent = report.resolve().parent
    try:
        return str(path.relative_to(report_parent))
    except ValueError:
        return str(path)


def config_table(default_data: dict[str, Any], cache_data: dict[str, Any]) -> str:
    default_config = default_data["config"]
    cache_config = cache_data["config"]
    keys = ["members", "ops", "range_size", "warmups", "seed", "score_shape"]
    lines = ["| Setting | Rank cache off | Rank cache on |", "| --- | ---: | ---: |"]
    for key in keys:
        default_value = default_config.get(key, "integer" if key == "score_shape" else None)
        cache_value = cache_config.get(key, "integer" if key == "score_shape" else None)
        lines.append(f"| `{key}` | `{default_value}` | `{cache_value}` |")
    lines.append("| `rank_cache` | `false` | `true` |")
    lines.append(
        f"| `score_string_cache` | "
        f"`{str(default_config.get('score_string_cache', False)).lower()}` | "
        f"`{str(cache_config.get('score_string_cache', False)).lower()}` |"
    )
    return "\n".join(lines)


def split_table(results: dict[tuple[str, str], dict[str, Any]]) -> str:
    rows = [
        ("ZSCORE", "zscore", "zscore_bulk", "zscore", "zscore"),
        ("ZRANK", "zrank", "zrank_integer", "zrank", "zrank"),
        ("ZREVRANK", "zrevrank", "zrank_integer", "zrevrank", "zrevrank"),
        ("ZRANGE", "zrange_iter", "zrange", "zrange", "zrange"),
        ("ZRANGE WITHSCORES", "zrange_iter", "zrange_withscores",
         "zrange_withscores", "zrange_withscores"),
    ]
    lines = [
        "| Metric | Raw ZSet ns/op | RESP ns/op | Command into ns/op | Command string ns/op | Parse into ns/op |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for label, raw_metric, resp_metric, command_metric, parse_metric in rows:
        lines.append(
            f"| `{label}` | {fmt_ns(ns(results, 'raw_zset', raw_metric))} | "
            f"{fmt_ns(ns(results, 'resp', resp_metric))} | "
            f"{fmt_ns(ns(results, 'command_into', command_metric))} | "
            f"{fmt_ns(ns(results, 'command_string', command_metric))} | "
            f"{fmt_ns(ns(results, 'parse_command_into', parse_metric))} |"
        )
    return "\n".join(lines)


def cache_table(default: dict[tuple[str, str], dict[str, Any]],
                cached: dict[tuple[str, str], dict[str, Any]]) -> str:
    rows = [
        ("Raw ZSCORE", "raw_zset", "zscore"),
        ("Raw ZRANK", "raw_zset", "zrank"),
        ("Raw ZREVRANK", "raw_zset", "zrevrank"),
        ("Raw ZRANGE iter", "raw_zset", "zrange_iter"),
        ("Command-into ZSCORE", "command_into", "zscore"),
        ("Command-into ZRANK", "command_into", "zrank"),
        ("Command-into ZREVRANK", "command_into", "zrevrank"),
        ("Command-into ZRANGE", "command_into", "zrange"),
    ]
    lines = [
        "| Metric | Off ns/op | On ns/op | Change | Off ops/sec | On ops/sec |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for label, category, metric in rows:
        off_ns = ns(default, category, metric)
        on_ns = ns(cached, category, metric)
        lines.append(
            f"| `{label}` | {fmt_ns(off_ns)} | {fmt_ns(on_ns)} | "
            f"{fmt_percent(pct_change(off_ns, on_ns))} | "
            f"{fmt_rate(rate(default, category, metric))} | "
            f"{fmt_rate(rate(cached, category, metric))} |"
        )
    return "\n".join(lines)


def range_breakdown_table(default: dict[tuple[str, str], dict[str, Any]],
                          cached: dict[tuple[str, str], dict[str, Any]]) -> str:
    rows = [
        ("Score-index traversal", "score_index_traversal"),
        ("Member lookup", "member_lookup"),
        ("Score formatting only", "score_format_only"),
        ("Member RESP append only", "member_resp_append_only"),
        ("Score RESP append preformatted", "score_resp_append_preformatted"),
        ("Score RESP append formatting", "score_resp_append_formatting"),
        ("Direct RESP append WITHSCORES", "direct_resp_append_withscores"),
        ("RESP append", "resp_append"),
        ("RESP append WITHSCORES", "resp_append_withscores"),
        ("Full command into", "execute_command_into"),
        ("Full command into WITHSCORES", "execute_command_into_withscores"),
    ]
    lines = [
        "| Component | Off ns/op | On ns/op | Off ops/sec | On ops/sec |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for label, metric in rows:
        lines.append(
            f"| `{label}` | "
            f"{fmt_ns(ns(default, 'zrange_breakdown', metric))} | "
            f"{fmt_ns(ns(cached, 'zrange_breakdown', metric))} | "
            f"{fmt_rate(rate(default, 'zrange_breakdown', metric))} | "
            f"{fmt_rate(rate(cached, 'zrange_breakdown', metric))} |"
        )
    return "\n".join(lines)


def bottleneck_summary(results: dict[tuple[str, str], dict[str, Any]]) -> list[str]:
    command_zrange = ns(results, "command_into", "zrange")
    resp_zrange = ns(results, "resp", "zrange")
    raw_zrange = ns(results, "raw_zset", "zrange_iter")
    traversal_zrange = ns(results, "zrange_breakdown", "score_index_traversal")
    member_lookup_zrange = ns(results, "zrange_breakdown", "member_lookup")
    score_format_zrange = ns(results, "zrange_breakdown", "score_format_only")
    score_append_zrange = ns(
        results, "zrange_breakdown", "score_resp_append_formatting"
    )
    append_zrange = ns(results, "zrange_breakdown", "resp_append")
    append_scores_zrange = ns(results, "zrange_breakdown", "resp_append_withscores")
    command_zscore = ns(results, "command_into", "zscore")
    raw_zscore = ns(results, "raw_zset", "zscore")
    command_zrank = ns(results, "command_into", "zrank")
    raw_zrank = ns(results, "raw_zset", "zrank")
    command_zrange_string = ns(results, "command_string", "zrange")

    lines: list[str] = []
    if command_zrange and resp_zrange and raw_zrange:
        lines.append(
            "- `ZRANGE` response construction is the visible in-process cost: "
            f"RESP-only serialization is {fmt_ns(resp_zrange)} ns/op versus "
            f"{fmt_ns(raw_zrange)} ns/op for raw range iteration."
        )
    if traversal_zrange and member_lookup_zrange and append_zrange:
        lines.append(
            "- `ZRANGE` breakdown separates score-index traversal, member lookup, "
            "and RESP append costs: "
            f"{fmt_ns(traversal_zrange)} ns/op traversal, "
            f"{fmt_ns(member_lookup_zrange)} ns/op member lookup, and "
            f"{fmt_ns(append_zrange)} ns/op RESP append."
        )
    if append_scores_zrange and append_zrange:
        lines.append(
            "- `WITHSCORES` adds double formatting work in the RESP append path: "
            f"{fmt_ns(append_scores_zrange)} ns/op versus "
            f"{fmt_ns(append_zrange)} ns/op without scores."
        )
    if score_format_zrange and score_append_zrange:
        lines.append(
            "- Score formatting and score bulk append are now separated: "
            f"{fmt_ns(score_format_zrange)} ns/op for formatting only and "
            f"{fmt_ns(score_append_zrange)} ns/op for score bulk append with "
            "formatting."
        )
    if command_zrange and command_zrange_string:
        lines.append(
            "- `ZRANGE` command-into versus string-wrapper cost should be read "
            "as a server-path check, not a guaranteed microbench win: "
            "command-into is "
            f"{fmt_ns(command_zrange)} ns/op versus "
            f"{fmt_ns(command_zrange_string)} ns/op for the string wrapper."
        )
    if command_zscore and raw_zscore:
        lines.append(
            "- `ZSCORE` command overhead over raw lookup is about "
            f"{fmt_ns(command_zscore - raw_zscore)} ns/op."
        )
    if command_zrank and raw_zrank:
        lines.append(
            "- `ZRANK` remains mostly data-structure work in-process: raw rank is "
            f"{fmt_ns(raw_zrank)} ns/op versus {fmt_ns(command_zrank)} ns/op "
            "for command execution."
        )
    return lines


def write_report(default_json: Path, cache_json: Path, output: Path) -> None:
    default_data = load(default_json)
    cache_data = load(cache_json)
    default_results = index_results(default_data)
    cache_results = index_results(cache_data)
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    content = [
        "# Goblin Core Microbenchmarks",
        "",
        f"Generated: {generated_at}.",
        "",
        "These measurements isolate in-process read-path costs. They do not "
        "include sockets, polling, Redis comparison, or process RSS accounting.",
        "",
        "Source data:",
        "",
        f"- `{result_path(default_json, output)}`",
        f"- `{result_path(cache_json, output)}`",
        "",
        "## Configuration",
        "",
        config_table(default_data, cache_data),
        "",
        "## Rank Cache Off",
        "",
        split_table(default_results),
        "",
        "## Rank Cache On",
        "",
        split_table(cache_results),
        "",
        "## Rank Cache Effect",
        "",
        cache_table(default_results, cache_results),
        "",
        "## ZRANGE Serialization Breakdown",
        "",
        range_breakdown_table(default_results, cache_results),
        "",
        "## Read-Path Notes",
        "",
        *bottleneck_summary(default_results),
        "",
    ]
    output.write_text("\n".join(content))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--default-json",
        type=Path,
        default=ROOT / "benchmark-results" / "microbench-1m-rank-cache-off.json",
    )
    parser.add_argument(
        "--rank-cache-json",
        type=Path,
        default=ROOT / "benchmark-results" / "microbench-1m-rank-cache-on.json",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "MICROBENCHMARKS.md")
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    write_report(args.default_json, args.rank_cache_json, args.output)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    import sys

    raise SystemExit(main(sys.argv[1:]))
