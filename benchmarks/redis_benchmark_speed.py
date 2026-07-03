#!/usr/bin/env python3
"""Measure Goblin Core vs Redis throughput with redis-benchmark (a C client).

Single-member throughput MUST be driven by a C load generator. One Python
pipelined connection is client-bound near ~350K ops/sec, so a Python harness
measures the client, not the server, and reports both systems as roughly equal
(this is the origin of the misleading `ZSCORE 0.95x` in older reports). This
driver:

  * starts Goblin Core and Redis, pinning the server to one core and
    redis-benchmark to another (Linux `taskset`) so the numbers reflect server
    cost rather than client/server contention,
  * loads a sorted set, then measures ZSCORE / ZRANK / ZREVRANK / ZADD (score
    update) / ZRANGE / ZRANGE WITHSCORES throughput with redis-benchmark,
  * also reports resident-set (RSS) bytes per member, since memory is the
    primary story for Goblin Core,
  * interleaves the server order across rounds to cancel drift and reports the
    median of each metric plus the Goblin Core / Redis ratio.

Example:

    python3 benchmarks/redis_benchmark_speed.py \
      --goblin-bin build-release/goblin-core \
      --redis-server "$(command -v redis-server)" \
      --members 1000000 --requests 2000000 --rounds 3 \
      --server-cpu 0 --client-cpu 1
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path
from typing import Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402 - path set above.
    RespClient,
    free_port,
    process_rss_mib,
    redis_used_memory_mib,
    resolve_executable,
    wait_for_server,
)


# (label, redis-benchmark command template). `{K}` is the loaded key, `{HI}` the
# inclusive range stop. `__rand_int__` is expanded by redis-benchmark to a random
# id in [0, --members), so member lookups hit the loaded set and ZADD updates an
# existing member with a fresh score.
OPS = [
    ("ZSCORE", ["zscore", "{K}", "member:__rand_int__"]),
    ("ZRANK", ["zrank", "{K}", "member:__rand_int__"]),
    ("ZREVRANK", ["zrevrank", "{K}", "member:__rand_int__"]),
    ("ZADD (score update)", ["zadd", "{K}", "__rand_int__", "member:__rand_int__"]),
    ("ZRANGE", ["zrange", "{K}", "0", "{HI}"]),
    ("ZRANGE WITHSCORES", ["zrange", "{K}", "0", "{HI}", "WITHSCORES"]),
]


def taskset_prefix(cpu: int | None) -> list[str]:
    if cpu is None or shutil.which("taskset") is None:
        return []
    return ["taskset", "-c", str(cpu)]


def start_pinned(command: Sequence[str], name: str, cpu: int | None) -> subprocess.Popen:
    return subprocess.Popen(
        taskset_prefix(cpu) + [str(part) for part in command],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def redis_benchmark_rps(rb: Path,
                        port: int,
                        command: Sequence[str],
                        requests: int,
                        pipeline: int,
                        keyspace: int,
                        client_cpu: int | None,
                        timeout: float) -> float:
    cmd = taskset_prefix(client_cpu) + [
        str(rb), "-h", "127.0.0.1", "-p", str(port),
        "-n", str(requests), "-P", str(pipeline), "-c", "1", "-q",
        "-r", str(max(1, keyspace)),
    ] + list(command)
    out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout
    match = re.search(r"([0-9]+\.?[0-9]*)\s+requests per second", out)
    if match is None:
        raise RuntimeError(f"redis-benchmark produced no throughput line: {out!r}")
    return float(match.group(1))


def measure_target(name: str,
                   command: Sequence[str],
                   args: argparse.Namespace,
                   rb: Path) -> dict:
    port = free_port()
    proc = start_pinned(list(command) + ["--port", str(port)], name, args.server_cpu)
    try:
        wait_for_server(port, timeout=args.timeout)
        baseline_rss = process_rss_mib(proc.pid)

        # Load ~members distinct members with random integer scores. Over-issue
        # so coverage of the id space is high (~1 - e^-load_factor).
        key = "rbs"
        redis_benchmark_rps(
            rb, port, ["zadd", key, "__rand_int__", "member:__rand_int__"],
            requests=args.members * args.load_factor, pipeline=128,
            keyspace=args.members, client_cpu=args.client_cpu, timeout=args.timeout)

        client = RespClient("127.0.0.1", port, timeout=args.timeout)
        try:
            card = int(client.command("ZCARD", key))
            # Reflect the deployment path: load, then compact/repack before
            # serving. Goblin Core exposes this as GOBLIN.OPTIMIZE; Redis has no
            # equivalent step. Measured after this so RSS and read throughput
            # reflect the optimized set.
            if name == "goblin" and args.optimize_density is not None:
                client.command("GOBLIN.OPTIMIZE", key, str(args.optimize_density))
            used_memory = (
                redis_used_memory_mib(client) if name == "redis" else None)
        finally:
            client.close()
        loaded_rss = process_rss_mib(proc.pid)
        rss_delta_mib = loaded_rss - baseline_rss

        rps: dict[str, float] = {}
        hi = str(args.range_size - 1)
        for label, template in OPS:
            cmd = [part.format(K=key, HI=hi) for part in template]
            rps[label] = redis_benchmark_rps(
                rb, port, cmd, requests=args.requests, pipeline=args.pipeline,
                keyspace=args.members, client_cpu=args.client_cpu,
                timeout=args.timeout)
        return {
            "target": name,
            "loaded_members": card,
            "rss_delta_mib": rss_delta_mib,
            "rss_bytes_per_member": (
                rss_delta_mib * 1024.0 * 1024.0 / card if card else None),
            "used_memory_mib": used_memory,
            "ops_per_second": rps,
        }
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def run(args: argparse.Namespace, rb: Path) -> dict:
    goblin_bin = resolve_executable(args.goblin_bin, "Goblin Core binary")
    redis_bin = resolve_executable(args.redis_server, "Redis server")
    targets = {
        "goblin": [str(goblin_bin)],
        "redis": [
            str(redis_bin), "--bind", "127.0.0.1", "--save", "",
            "--appendonly", "no", "--protected-mode", "no"],
    }

    rounds: list[dict] = []
    for r in range(args.rounds):
        # Alternate order each round so warmup/thermal drift does not favor one.
        order = ["goblin", "redis"] if r % 2 == 0 else ["redis", "goblin"]
        for name in order:
            print(f"round {r + 1}: {name}", file=sys.stderr, flush=True)
            rounds.append(measure_target(name, targets[name], args, rb))

    def rows(name: str) -> list[dict]:
        return [row for row in rounds if row["target"] == name]

    summary = {"config": vars(args) | {"redis_benchmark": str(rb)}, "targets": {}}
    for name in ("goblin", "redis"):
        rs = rows(name)
        summary["targets"][name] = {
            "rss_bytes_per_member": statistics.median(
                [x["rss_bytes_per_member"] for x in rs if x["rss_bytes_per_member"]]),
            "loaded_members": statistics.median([x["loaded_members"] for x in rs]),
            "ops_per_second": {
                label: statistics.median([x["ops_per_second"][label] for x in rs])
                for label, _ in OPS
            },
        }
    summary["rounds"] = rounds
    return summary


def render_markdown(summary: dict) -> str:
    g = summary["targets"]["goblin"]
    r = summary["targets"]["redis"]
    lines = ["# Goblin Core vs Redis (redis-benchmark)", ""]
    gb, rb_ = g["rss_bytes_per_member"], r["rss_bytes_per_member"]
    lines += [
        "Memory (RSS delta over baseline; the primary story):",
        "",
        "| Target | RSS B/member | loaded members |",
        "| --- | ---: | ---: |",
        f"| Goblin Core | {gb:,.1f} | {int(g['loaded_members']):,} |",
        f"| Redis | {rb_:,.1f} | {int(r['loaded_members']):,} |",
        f"| Goblin Core / Redis | {gb / rb_ * 100:.1f}% | |",
        "",
        "Throughput (secondary), median requests/sec:",
        "",
        "| Operation | Goblin Core | Redis | Goblin Core / Redis |",
        "| --- | ---: | ---: | ---: |",
    ]
    for label, _ in OPS:
        gv = g["ops_per_second"][label]
        rv = r["ops_per_second"][label]
        ratio = f"{gv / rv:.2f}x" if rv else "n/a"
        lines.append(f"| `{label}` | {gv:,.0f} | {rv:,.0f} | {ratio} |")
    return "\n".join(lines) + "\n"


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    default_goblin = Path("build-release/goblin-core")
    if not default_goblin.exists():
        default_goblin = Path("build/goblin-core")
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--redis-server", type=Path,
                        default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--redis-benchmark", type=str,
                        default=shutil.which("redis-benchmark") or "redis-benchmark")
    parser.add_argument("--members", type=int, default=1_000_000)
    parser.add_argument("--requests", type=int, default=2_000_000,
                        help="redis-benchmark requests per throughput measurement.")
    parser.add_argument("--load-factor", type=int, default=3,
                        help="Insert members*load_factor rows so id-space coverage is high.")
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--range-size", type=int, default=16)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--optimize-density", type=str, default="0.97",
                        help="Run GOBLIN.OPTIMIZE at this member-index packing "
                             "density after loading (reflects load-then-serve "
                             "deployment). 'none' skips it. Avoid 1.0 unless the "
                             "set is read-only with no absent-member lookups.")
    parser.add_argument("--server-cpu", type=int, default=None,
                        help="Pin the server to this CPU (Linux taskset). Omit to not pin.")
    parser.add_argument("--client-cpu", type=int, default=None,
                        help="Pin redis-benchmark to this CPU (Linux taskset). Omit to not pin.")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--format", choices=["markdown", "json"], default="markdown")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if min(args.members, args.requests, args.pipeline, args.range_size,
           args.rounds, args.load_factor) <= 0:
        parser.error("--members/--requests/--pipeline/--range-size/--rounds/"
                     "--load-factor must be positive")
    if args.optimize_density in (None, "", "none", "None"):
        args.optimize_density = None
    else:
        density = float(args.optimize_density)
        if not (0.0 < density <= 1.0):
            parser.error("--optimize-density must be in (0, 1] or 'none'")
        args.optimize_density = density
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    rb = args.redis_benchmark
    resolved = rb if Path(rb).exists() else shutil.which(rb)
    if resolved is None:
        print(f"redis-benchmark not found: {rb}", file=sys.stderr)
        return 2
    summary = run(args, Path(resolved))
    text = json.dumps(summary, indent=2, default=str) if args.format == "json" \
        else render_markdown(summary)
    if args.output is None:
        print(text)
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text if text.endswith("\n") else text + "\n")
        print(f"wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
