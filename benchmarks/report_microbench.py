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


def mode_name(data: dict[str, Any], fallback: str) -> str:
    config = data.get("config", {})
    return str(config.get("rank_cache_mode") or fallback)


def config_table(mode_data: dict[str, dict[str, Any]]) -> str:
    keys = ["members", "ops", "range_size", "warmups", "seed", "score_shape"]
    modes = list(mode_data)
    lines = [
        "| Setting | " + " | ".join(f"`{mode}`" for mode in modes) + " |",
        "| --- | " + " | ".join("---:" for _ in modes) + " |",
    ]
    for key in keys:
        values = []
        for data in mode_data.values():
            config = data["config"]
            values.append(config.get(key, "integer" if key == "score_shape" else None))
        lines.append("| `" + key + "` | " +
                     " | ".join(f"`{value}`" for value in values) + " |")
    lines.append("| `rank_cache_mode` | " +
                 " | ".join(f"`{mode_name(data, mode)}`"
                             for mode, data in mode_data.items()) + " |")
    lines.append(
        "| `score_string_cache` | " +
        " | ".join(
            f"`{str(data['config'].get('score_string_cache', False)).lower()}`"
            for data in mode_data.values()
        ) + " |"
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


def cache_table(mode_results: dict[str, dict[tuple[str, str], dict[str, Any]]]) -> str:
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
    modes = list(mode_results)
    lines = [
        "| Metric | " + " | ".join(f"{mode} ns/op" for mode in modes) +
        " | exact vs off | block-hint vs off |",
        "| --- | " + " | ".join("---:" for _ in modes) + " | ---: | ---: |",
    ]
    for label, category, metric in rows:
        values = {
            mode: ns(results, category, metric)
            for mode, results in mode_results.items()
        }
        off_ns = values.get("off")
        lines.append("| `" + label + "` | " +
                     " | ".join(fmt_ns(values[mode]) for mode in modes) +
                     f" | {fmt_percent(pct_change(off_ns, values.get('exact')))} | "
                     f"{fmt_percent(pct_change(off_ns, values.get('block-hint')))} |")
    return "\n".join(lines)


def range_breakdown_table(
    mode_results: dict[str, dict[tuple[str, str], dict[str, Any]]],
) -> str:
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
    modes = list(mode_results)
    lines = [
        "| Component | " + " | ".join(f"{mode} ns/op" for mode in modes) + " |",
        "| --- | " + " | ".join("---:" for _ in modes) + " |",
    ]
    for label, metric in rows:
        lines.append("| `" + label + "` | " +
                     " | ".join(
                         fmt_ns(ns(results, "zrange_breakdown", metric))
                         for results in mode_results.values()
                     ) + " |")
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


def write_report(off_json: Path,
                 exact_json: Path,
                 block_hint_json: Path | None,
                 output: Path) -> None:
    mode_paths = {"off": off_json, "exact": exact_json}
    if block_hint_json is not None:
        mode_paths["block-hint"] = block_hint_json
    mode_data = {mode: load(path) for mode, path in mode_paths.items()}
    mode_results = {
        mode: index_results(data)
        for mode, data in mode_data.items()
    }
    default_results = mode_results["off"]
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
        *[
            f"- `{mode}`: `{result_path(path, output)}`"
            for mode, path in mode_paths.items()
        ],
        "",
        "## Configuration",
        "",
        config_table(mode_data),
    ]

    for mode, results in mode_results.items():
        content.extend([
            "",
            f"## Rank Cache `{mode}`",
            "",
            split_table(results),
        ])

    content.extend([
        "",
        "## Rank Cache Effect",
        "",
        cache_table(mode_results),
        "",
        "## ZRANGE Serialization Breakdown",
        "",
        range_breakdown_table(mode_results),
        "",
        "## Read-Path Notes",
        "",
        *bottleneck_summary(default_results),
        "",
    ])
    output.write_text("\n".join(content))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--default-json",
        type=Path,
        default=ROOT / "benchmark-results" / "microbench-1m-rank-cache-off-v2.json",
    )
    parser.add_argument(
        "--rank-cache-json",
        type=Path,
        default=ROOT / "benchmark-results" / "microbench-1m-rank-cache-exact-v2.json",
        help="Exact rank-cache microbenchmark JSON.",
    )
    parser.add_argument(
        "--block-hint-json",
        type=Path,
        default=ROOT / "benchmark-results" / "microbench-1m-rank-cache-block-hint-v2.json",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "MICROBENCHMARKS.md")
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    write_report(args.default_json, args.rank_cache_json, args.block_hint_json, args.output)
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    import sys

    raise SystemExit(main(sys.argv[1:]))
