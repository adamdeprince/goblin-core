#!/usr/bin/env python3
"""Stress pipelined ZRANGE/ZREVRANGE response output.

This benchmark preloads one sorted set, then sends large bursts of range
commands before reading responses. It skips RESP payloads while validating their
framing, so the measurement focuses on server/network output rather than Python
object allocation.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import socket
import sys
import threading
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Sequence

import zset_benchmark as zbench


ROOT = Path(__file__).resolve().parents[1]

VARIANTS: dict[str, tuple[str, bool, int]] = {
    "zrange": ("ZRANGE", False, 10),
    "zrevrange": ("ZREVRANGE", False, 11),
    "zrange_withscores": ("ZRANGE", True, 12),
    "zrevrange_withscores": ("ZREVRANGE", True, 13),
}
DEFAULT_VARIANT_ORDER = tuple(VARIANTS)


@dataclass
class RangeOutputResult:
    target: str
    metric: str
    commands: int
    warmup_commands: int
    score_shape: str
    range_size: int
    pipeline_depth: int
    read_delay_ms: float
    goblin_max_output_buffer_mib: int | None
    goblin_initial_output_buffer_kib: int | None
    goblin_score_string_cache: bool | None
    seconds: float
    ops_per_second: float
    response_mib: float
    response_mib_per_second: float
    rss_after_load_mib: float
    rss_final_mib: float
    peak_rss_mib: float = 0.0


class RssSampler:
    def __init__(self, pid: int, interval_seconds: float, initial_mib: float) -> None:
        self.pid = pid
        self.interval_seconds = interval_seconds
        self.peak_mib = initial_mib
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "RssSampler":
        self.sample_once()
        self._thread.start()
        return self

    def __exit__(self, *_exc: object) -> None:
        self._stop.set()
        self._thread.join()
        self.sample_once()

    def sample_once(self) -> None:
        try:
            rss = zbench.process_rss_mib(self.pid)
        except Exception:  # noqa: BLE001 - process may exit during teardown.
            return
        with self._lock:
            self.peak_mib = max(self.peak_mib, rss)

    def peak(self) -> float:
        with self._lock:
            return self.peak_mib

    def _run(self) -> None:
        while not self._stop.wait(self.interval_seconds):
            self.sample_once()


class SkippingRespClient:
    def __init__(self, port: int, timeout: float) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()
        self.bytes_received = 0

    def close(self) -> None:
        self.sock.close()

    def reset_byte_count(self) -> None:
        self.bytes_received = 0

    def command(self, *parts: object) -> None:
        self.sock.sendall(zbench.encode_command(parts))
        self.read_response_skip()

    def pipeline_skip(self,
                      commands: Iterable[Sequence[object]],
                      pipeline_depth: int,
                      read_delay_seconds: float) -> int:
        pending = 0
        sent = 0
        batch = bytearray()
        for command in commands:
            batch.extend(zbench.encode_command(command))
            pending += 1
            sent += 1
            if pending >= pipeline_depth:
                self.sock.sendall(batch)
                batch.clear()
                if read_delay_seconds > 0:
                    time.sleep(read_delay_seconds)
                for _ in range(pending):
                    self.read_response_skip()
                pending = 0

        if pending:
            self.sock.sendall(batch)
            if read_delay_seconds > 0:
                time.sleep(read_delay_seconds)
            for _ in range(pending):
                self.read_response_skip()

        return sent

    def read_response_skip(self) -> None:
        prefix = self._read_exact(1)
        if prefix in (b"+", b"-", b":"):
            self._read_line()
            return
        if prefix == b"$":
            length = int(self._read_line())
            if length == -1:
                return
            payload = self._read_exact(length + 2)
            if payload[-2:] != b"\r\n":
                raise RuntimeError("invalid RESP bulk terminator")
            return
        if prefix == b"*":
            count = int(self._read_line())
            if count == -1:
                return
            for _ in range(count):
                self.read_response_skip()
            return
        raise RuntimeError(f"invalid RESP prefix: {prefix!r}")

    def _read_line(self) -> bytes:
        while True:
            pos = self.buffer.find(b"\r\n")
            if pos >= 0:
                line = bytes(self.buffer[:pos])
                del self.buffer[: pos + 2]
                return line
            self._recv_more()

    def _read_exact(self, size: int) -> bytes:
        while len(self.buffer) < size:
            self._recv_more()
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def _recv_more(self) -> None:
        data = self.sock.recv(1024 * 1024)
        if not data:
            raise RuntimeError("connection closed while reading RESP response")
        self.bytes_received += len(data)
        self.buffer.extend(data)


def range_commands(command_name: str,
                   count: int,
                   member_count: int,
                   key: str,
                   range_size: int,
                   seed: int,
                   with_scores: bool) -> Iterable[list[object]]:
    import random

    rng = random.Random(seed)
    max_start = max(0, member_count - range_size)
    for _ in range(count):
        start = rng.randint(0, max_start) if max_start > 0 else 0
        command: list[object] = [command_name, key, start, start + range_size - 1]
        if with_scores:
            command.append("WITHSCORES")
        yield command


def metric_name(command_name: str, with_scores: bool) -> str:
    name = command_name.lower()
    return f"{name}_withscores" if with_scores else name


def run_variant(client: SkippingRespClient,
                server: zbench.ServerProcess,
                key: str,
                args: argparse.Namespace,
                command_name: str,
                with_scores: bool,
                seed: int,
                rss_after_load: float) -> RangeOutputResult:
    if args.warmup_ops > 0:
        client.pipeline_skip(
            range_commands(
                command_name,
                args.warmup_ops,
                args.members,
                key,
                args.range_size,
                seed + 1_000_000,
                with_scores,
            ),
            args.pipeline,
            0.0,
        )

    client.reset_byte_count()
    with RssSampler(
        server.process.pid,
        args.rss_sample_interval_ms / 1000.0,
        rss_after_load,
    ) as rss_sampler:
        started = time.perf_counter()
        sent = client.pipeline_skip(
            range_commands(
                command_name,
                args.ops,
                args.members,
                key,
                args.range_size,
                seed,
                with_scores,
            ),
            args.pipeline,
            args.read_delay_ms / 1000.0,
        )
        seconds = time.perf_counter() - started
        rss_final = zbench.process_rss_mib(server.process.pid)

    peak_rss = max(rss_sampler.peak(), rss_final)

    response_mib = client.bytes_received / (1024.0 * 1024.0)
    return RangeOutputResult(
        target=server.name,
        metric=metric_name(command_name, with_scores),
        commands=sent,
        warmup_commands=args.warmup_ops,
        score_shape=args.score_shape,
        range_size=args.range_size,
        pipeline_depth=args.pipeline,
        read_delay_ms=args.read_delay_ms,
        goblin_max_output_buffer_mib=(
            args.goblin_max_output_buffer_mib if server.name == "goblin" else None
        ),
        goblin_initial_output_buffer_kib=(
            args.goblin_initial_output_buffer_kib if server.name == "goblin" else None
        ),
        goblin_score_string_cache=(
            args.goblin_score_string_cache if server.name == "goblin" else None
        ),
        seconds=seconds,
        ops_per_second=sent / seconds if seconds > 0 else 0.0,
        response_mib=response_mib,
        response_mib_per_second=response_mib / seconds if seconds > 0 else 0.0,
        rss_after_load_mib=rss_after_load,
        rss_final_mib=rss_final,
        peak_rss_mib=peak_rss,
    )


def load_fixture(client: SkippingRespClient,
                 server: zbench.ServerProcess,
                 args: argparse.Namespace,
                 key: str) -> float:
    load_ids = zbench.shuffled_ids(args.members, args.seed)
    client.pipeline_skip(
        zbench.zadd_commands(
            load_ids,
            key,
            args.zadd_batch,
            args.score_shape,
            args.seed,
        ),
        args.load_pipeline,
        0.0,
    )
    time.sleep(args.settle_seconds)
    return zbench.process_rss_mib(server.process.pid)


def run_target(server: zbench.ServerProcess,
               args: argparse.Namespace) -> list[RangeOutputResult]:
    client = SkippingRespClient(server.port, args.timeout)
    try:
        key = f"zout:{server.name}:{args.seed}"
        rss_after_load = load_fixture(client, server, args, key)
        variants = [
            (command_name, with_scores, args.seed + seed_offset)
            for command_name, with_scores, seed_offset in (
                VARIANTS[metric] for metric in args.variant_order
            )
        ]
        return [
            run_variant(
                client,
                server,
                key,
                args,
                command_name,
                with_scores,
                seed,
                rss_after_load,
            )
            for command_name, with_scores, seed in variants
        ]
    finally:
        client.close()


def start_targets(args: argparse.Namespace) -> list[zbench.ServerProcess]:
    servers: list[zbench.ServerProcess] = []
    if args.target in ("both", "goblin"):
        servers.append(
            zbench.start_goblin(
                args.goblin_bin,
                args.goblin_rank_cache,
                args.goblin_max_output_buffer_mib,
                args.goblin_score_string_cache,
                args.goblin_initial_output_buffer_kib,
            )
        )
    if args.target in ("both", "redis"):
        servers.append(zbench.start_redis(args.redis_server))
    return servers


def load_results(path: Path) -> list[RangeOutputResult]:
    rows = json.loads(path.read_text())
    for row in rows:
        row.setdefault("peak_rss_mib", row.get("rss_final_mib", 0.0))
        row.setdefault("goblin_score_string_cache", None)
        row.setdefault("goblin_initial_output_buffer_kib", None)
        row.setdefault("score_shape", "integer")
        row.setdefault("warmup_commands", 0)
    return [RangeOutputResult(**row) for row in rows]


def write_json(results: Sequence[RangeOutputResult], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps([asdict(row) for row in results], indent=2) + "\n")


def result_path(path: Path, report: Path) -> str:
    path = path.resolve()
    report_parent = report.resolve().parent
    try:
        return str(path.relative_to(report_parent))
    except ValueError:
        return str(path)


def fmt(value: float | int | None, digits: int = 2) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    if isinstance(value, int):
        return f"{value:,}"
    return f"{value:,.{digits}f}"


def write_report(results: Sequence[RangeOutputResult],
                 json_path: Path,
                 report: Path,
                 args: argparse.Namespace) -> None:
    by_metric: dict[str, dict[str, RangeOutputResult]] = {}
    for row in results:
        by_metric.setdefault(row.metric, {})[row.target] = row

    lines = [
        "# Goblin Core Range Output Benchmark",
        "",
        f"Generated: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%S UTC')}.",
        "",
        "This benchmark preloads one zset, sends each burst of range commands "
        "before reading responses, and skips RESP payloads while validating "
        "framing. It is meant to stress server output buffering.",
        "",
        f"Source data: `{result_path(json_path, report)}`",
        "",
        "## Workload",
        "",
        f"- target: `{args.target}`",
        f"- members: `{args.members:,}`",
        f"- range commands per metric: `{args.ops:,}`",
        f"- warmup commands per metric: `{args.warmup_ops:,}`",
        f"- score shape: `{args.score_shape}`",
        f"- range size: `{args.range_size}`",
        f"- pipeline depth: `{args.pipeline}`",
        f"- read delay per burst: `{args.read_delay_ms}` ms",
        f"- Goblin max output buffer: `{args.goblin_max_output_buffer_mib}` MiB",
        f"- Goblin initial output buffer: `{args.goblin_initial_output_buffer_kib}` KiB",
        f"- Goblin score string cache: `{args.goblin_score_string_cache}`",
        f"- RSS sample interval: `{args.rss_sample_interval_ms}` ms",
        f"- zadd batch size: `{args.zadd_batch}`",
        f"- load pipeline depth: `{args.load_pipeline}`",
        f"- seed: `{args.seed}`",
        f"- variant order: `{', '.join(args.variant_order)}`",
        "",
        "## Results",
        "",
        "| Metric | Goblin ops/sec | Redis ops/sec | Goblin / Redis | "
        "Goblin MiB/sec | Redis MiB/sec | Goblin peak RSS MiB | Redis peak RSS MiB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for metric in ("zrange", "zrevrange", "zrange_withscores", "zrevrange_withscores"):
        goblin = by_metric.get(metric, {}).get("goblin")
        redis = by_metric.get(metric, {}).get("redis")
        goblin_rate = None if goblin is None else goblin.ops_per_second
        redis_rate = None if redis is None else redis.ops_per_second
        ratio = (
            None if goblin_rate is None or not redis_rate
            else goblin_rate / redis_rate
        )
        ratio_text = "n/a" if ratio is None else f"{ratio:.2f}x"
        lines.append(
            f"| `{metric}` | {fmt(goblin_rate, 0)} | {fmt(redis_rate, 0)} | "
            f"{ratio_text} | "
            f"{fmt(None if goblin is None else goblin.response_mib_per_second, 2)} | "
            f"{fmt(None if redis is None else redis.response_mib_per_second, 2)} | "
            f"{fmt(None if goblin is None else goblin.peak_rss_mib, 2)} | "
            f"{fmt(None if redis is None else redis.peak_rss_mib, 2)} |"
        )

    lines.extend([
        "",
        "## Per Target",
        "",
        "| Target | Metric | Seconds | Ops/sec | Response MiB | Response MiB/sec | RSS load MiB | Peak RSS MiB | RSS final MiB |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    for row in results:
        lines.append(
            f"| `{row.target}` | `{row.metric}` | {fmt(row.seconds, 3)} | "
            f"{fmt(row.ops_per_second, 0)} | {fmt(row.response_mib, 2)} | "
            f"{fmt(row.response_mib_per_second, 2)} | "
            f"{fmt(row.rss_after_load_mib, 2)} | "
            f"{fmt(row.peak_rss_mib, 2)} | {fmt(row.rss_final_mib, 2)} |"
        )

    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(lines) + "\n")


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=["both", "goblin", "redis"], default="both")
    default_goblin = ROOT / "build-release" / "goblin-core"
    if not default_goblin.exists():
        default_goblin = ROOT / "build" / "goblin-core"
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--goblin-rank-cache", action="store_true")
    parser.add_argument("--goblin-score-string-cache", action="store_true")
    parser.add_argument("--goblin-max-output-buffer-mib", type=int, default=1)
    parser.add_argument("--goblin-initial-output-buffer-kib", type=int, default=0)
    parser.add_argument("--redis-server", type=Path,
                        default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--members", type=int, default=100_000)
    parser.add_argument("--ops", type=int, default=10_000)
    parser.add_argument("--warmup-ops", type=int, default=0)
    parser.add_argument("--score-shape", choices=zbench.SCORE_SHAPES, default="integer")
    parser.add_argument("--range-size", type=int, default=256)
    parser.add_argument("--pipeline", type=int, default=2048)
    parser.add_argument("--read-delay-ms", type=float, default=2.0)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--load-pipeline", type=int, default=256)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument(
        "--variant-order",
        nargs="+",
        choices=tuple(VARIANTS),
        default=list(DEFAULT_VARIANT_ORDER),
        help=(
            "metric execution order; useful for separating path cost from "
            "cache-warming/order effects"
        ),
    )
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--rss-sample-interval-ms", type=float, default=10.0)
    parser.add_argument("--output-json", type=Path,
                        default=ROOT / "benchmark-results" / "range-output.json")
    parser.add_argument("--report", type=Path,
                        default=ROOT / "benchmark-results" / "range-output.md")
    args = parser.parse_args(argv)

    if min(args.members, args.ops, args.range_size, args.pipeline,
           args.zadd_batch, args.load_pipeline) <= 0:
        parser.error("members, ops, range-size, pipeline, zadd-batch, and load-pipeline must be positive")
    if args.warmup_ops < 0:
        parser.error("--warmup-ops must be non-negative")
    if args.range_size > args.members:
        parser.error("--range-size must be less than or equal to --members")
    if args.goblin_max_output_buffer_mib < 0:
        parser.error("--goblin-max-output-buffer-mib must be non-negative")
    if args.goblin_initial_output_buffer_kib < 0:
        parser.error("--goblin-initial-output-buffer-kib must be non-negative")
    if args.read_delay_ms < 0 or args.settle_seconds < 0 or args.rss_sample_interval_ms <= 0:
        parser.error(
            "--read-delay-ms and --settle-seconds must be non-negative; "
            "--rss-sample-interval-ms must be positive"
        )
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    servers = start_targets(args)
    results: list[RangeOutputResult] = []
    try:
        for server in servers:
            results.extend(run_target(server, args))
    finally:
        for server in servers:
            server.stop()

    write_json(results, args.output_json)
    write_report(results, args.output_json, args.report, args)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
