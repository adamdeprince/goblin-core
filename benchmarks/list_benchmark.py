#!/usr/bin/env python3
"""Benchmark a 100k-item Redis list across Goblin Core and incumbents.

The harness reuses zset_benchmark.py's RESP client, parity configuration,
server launchers, redis-benchmark driver, RSS probe, and INFO/GOBLIN.MEMORY
readers. It loads one fixed-width list through pipelined multi-value RPUSH,
measures rank-sensitive reads and writes at several positions, then exercises
stable-length endpoint and pivot churn. Every engine runs alone.

Example (naamah):
  ./benchmarks/list_benchmark.py \
    --engine goblin-pma:goblin-pma:./build-release/goblin-core \
    --engine redis-7.2.4:redis:$HOME/bench/redis-7.2.4/src/redis-server \
    --engine redis-8.8:redis:$HOME/bench/redis-8.8.0/src/redis-server \
    --engine valkey-9.1:redis:$HOME/bench/valkey-9.1.0/src/valkey-server \
    --engine dragonfly:dragonfly:$HOME/dragonfly/build-opt/dragonfly
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import signal
import socket
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zset_benchmark as zbench  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class EngineSpec:
    label: str
    kind: str
    binary: Path

    @property
    def is_goblin(self) -> bool:
        return self.kind == "goblin" or self.kind.startswith("goblin-")

    @property
    def list_prefix(self) -> str:
        if self.kind == "goblin":
            return ""
        if self.kind.startswith("goblin-"):
            implementation = self.kind.removeprefix("goblin-")
            return f"GOBLIN.{implementation.upper()}."
        return ""

    def list_command(self, name: str) -> str:
        return f"{self.list_prefix}{name}"


@dataclass
class MemorySample:
    rss_mib: float
    info_used_memory_mib: float | None
    key_memory_usage_bytes: int | None
    list_length: int | None
    goblin_memory: dict[str, int | str] | None


@dataclass
class OperationResult:
    name: str
    description: str
    harness: str
    logical_operations: int
    resp_commands: int
    seconds: float
    logical_operations_per_second: float
    resp_commands_per_second: float


@dataclass
class EngineResult:
    label: str
    kind: str
    binary: str
    load_commands: int
    load_seconds: float
    load_items_per_second: float
    memory_baseline: MemorySample
    memory_after_load: MemorySample
    memory_after_operations: MemorySample
    operations: list[OperationResult]


def parse_engine(spec: str) -> EngineSpec:
    parts = spec.split(":", 2)
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(
            f"--engine expects LABEL:KIND:PATH, got {spec!r}"
        )
    label, kind, path = parts
    goblin_implementation = kind.removeprefix("goblin-")
    valid_goblin_kind = kind == "goblin" or (
        kind.startswith("goblin-")
        and bool(goblin_implementation)
        and goblin_implementation.replace("-", "").isalnum()
    )
    if kind not in ("redis", "dragonfly") and not valid_goblin_kind:
        raise argparse.ArgumentTypeError(
            "engine kind must be redis, dragonfly, goblin, or "
            f"goblin-IMPLEMENTATION; got {kind!r}"
        )
    return EngineSpec(label=label, kind=kind, binary=Path(path).expanduser())


def item_value(item_id: int, width: int, prefix: str = "") -> str:
    if len(prefix) >= width:
        raise ValueError("value prefix must be shorter than --value-bytes")
    digits = width - len(prefix)
    text = f"{item_id:0{digits}x}"
    if len(text) > digits:
        raise ValueError("item id does not fit --value-bytes")
    return prefix + text


def rpush_commands(
    count: int, key: str, batch_size: int, value_bytes: int, command_name: str
) -> Iterable[list[object]]:
    for start in range(0, count, batch_size):
        stop = min(count, start + batch_size)
        yield [
            command_name,
            key,
            *(item_value(item_id, value_bytes) for item_id in range(start, stop)),
        ]


def paired_endpoint_commands(
    key: str, pairs: int, value_bytes: int, push: str, pop: str, prefix: str
) -> Iterable[list[object]]:
    for ordinal in range(pairs):
        yield [push, key, item_value(ordinal, value_bytes, prefix)]
        yield [pop, key]


def middle_insert_remove_commands(
    key: str,
    pivot: bytes,
    pairs: int,
    value_bytes: int,
    insert_command: str,
    remove_command: str,
) -> Iterable[list[object]]:
    for ordinal in range(pairs):
        value = item_value(ordinal, value_bytes, "m")
        yield [insert_command, key, "BEFORE", pivot, value]
        yield [remove_command, key, 1, value]


def counted_pop_commands(
    key: str,
    pairs: int,
    width: int,
    value_bytes: int,
    push_command: str,
    pop_command: str,
) -> Iterable[list[object]]:
    for ordinal in range(pairs):
        values = [
            item_value(ordinal * width + offset, value_bytes, "b")
            for offset in range(width)
        ]
        yield [push_command, key, *values]
        yield [pop_command, key, width]


def start_engine(spec: EngineSpec, args: argparse.Namespace) -> zbench.ServerProcess:
    if spec.is_goblin:
        extra_args = ["--list-implementation", args.standard_list_implementation]
        if args.goblin_max_density is not None:
            extra_args.extend(["--list-max-density", str(args.goblin_max_density)])
        return zbench.start_goblin(
            spec.binary,
            rank_cache=False,
            rank_cache_mode="off",
            extra_args=extra_args,
        )
    if spec.kind == "redis":
        return zbench.start_redis(spec.binary)
    return zbench.start_dragonfly(spec.binary)


def key_memory_usage(client: zbench.RespClient, key: str) -> int | None:
    try:
        response = client.command("MEMORY", "USAGE", key)
    except Exception:
        return None
    return response if isinstance(response, int) else None


def memory_sample(
    client: zbench.RespClient,
    server: zbench.ServerProcess,
    spec: EngineSpec,
    key: str,
) -> MemorySample:
    try:
        length = client.command(spec.list_command("LLEN"), key)
    except Exception:
        length = None
    return MemorySample(
        rss_mib=zbench.process_rss_mib(server.process.pid),
        info_used_memory_mib=zbench.redis_used_memory_mib(client),
        key_memory_usage_bytes=key_memory_usage(client, key),
        list_length=length if isinstance(length, int) else None,
        goblin_memory=(
            zbench.goblin_memory_stats(client, key)
            if spec.is_goblin
            else None
        ),
    )


def expect_bulk(client: zbench.RespClient, expected: str, *command: object) -> None:
    response = client.command(*command)
    if response != expected.encode():
        raise RuntimeError(
            f"correctness check failed for {command!r}: "
            f"expected {expected!r}, got {response!r}"
        )


def check_loaded_list(
    client: zbench.RespClient,
    spec: EngineSpec,
    key: str,
    members: int,
    value_bytes: int,
) -> None:
    length = client.command(spec.list_command("LLEN"), key)
    if length != members:
        raise RuntimeError(f"loaded list length is {length}, expected {members}")
    for index in (0, members // 2, members - 1):
        expect_bulk(
            client,
            item_value(index, value_bytes),
            spec.list_command("LINDEX"),
            key,
            index,
        )


def operation(
    name: str,
    description: str,
    harness: str,
    logical_operations: int,
    resp_commands: int,
    seconds: float,
) -> OperationResult:
    return OperationResult(
        name=name,
        description=description,
        harness=harness,
        logical_operations=logical_operations,
        resp_commands=resp_commands,
        seconds=seconds,
        logical_operations_per_second=(
            logical_operations / seconds if seconds > 0 else 0.0
        ),
        resp_commands_per_second=resp_commands / seconds if seconds > 0 else 0.0,
    )


def benchmark_fixed_command(
    args: argparse.Namespace,
    server: zbench.ServerProcess,
    name: str,
    description: str,
    command: Sequence[object],
) -> OperationResult:
    rates = [
        zbench.redis_benchmark_rps(
            args.redis_benchmark,
            server.port,
            command,
            requests=args.requests,
            pipeline=args.pipeline,
            keyspace=args.members,
            timeout=args.timeout,
        )
        for _ in range(args.rounds)
    ]
    rate = statistics.median(rates)
    return operation(
        name,
        description,
        f"redis-benchmark median of {args.rounds}",
        args.requests,
        args.requests,
        args.requests / rate,
    )


def benchmark_pipeline(
    client: zbench.RespClient,
    args: argparse.Namespace,
    name: str,
    description: str,
    logical_operations: int,
    commands: Iterable[Sequence[object]],
) -> OperationResult:
    command_count, seconds = zbench.time_pipeline(client, commands, args.pipeline)
    return operation(
        name,
        description,
        "existing Python RESP pipeline",
        logical_operations,
        command_count,
        seconds,
    )


def run_engine(spec: EngineSpec, args: argparse.Namespace) -> EngineResult:
    server = start_engine(spec, args)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            key = f"listbench:{os.getpid()}:{spec.label}"
            baseline = memory_sample(client, server, spec, key)

            load_commands, load_seconds = zbench.time_pipeline(
                client,
                rpush_commands(
                    args.members,
                    key,
                    args.load_batch,
                    args.value_bytes,
                    spec.list_command("RPUSH"),
                ),
                args.pipeline,
            )
            check_loaded_list(client, spec, key, args.members, args.value_bytes)
            if spec.is_goblin:
                client.command("GOBLIN.OPTIMIZE", key)
            time.sleep(args.settle_seconds)
            after_load = memory_sample(client, server, spec, key)

            middle = args.members // 2
            quarter = args.members // 4
            three_quarters = args.members * 3 // 4
            range_half = args.range_size // 2
            range_start = max(0, middle - range_half)
            range_stop = min(args.members - 1, range_start + args.range_size - 1)

            operations = [
                benchmark_fixed_command(
                    args,
                    server,
                    "llen",
                    "LLEN control",
                    [spec.list_command("LLEN"), key],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lindex_head",
                    "LINDEX 0",
                    [spec.list_command("LINDEX"), key, 0],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lindex_quarter",
                    f"LINDEX {quarter}",
                    [spec.list_command("LINDEX"), key, quarter],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lindex_middle",
                    f"LINDEX {middle}",
                    [spec.list_command("LINDEX"), key, middle],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lindex_three_quarters",
                    f"LINDEX {three_quarters}",
                    [spec.list_command("LINDEX"), key, three_quarters],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lindex_tail",
                    "LINDEX -1",
                    [spec.list_command("LINDEX"), key, -1],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lrange_middle",
                    f"LRANGE {range_start} {range_stop}",
                    [spec.list_command("LRANGE"), key, range_start, range_stop],
                ),
                benchmark_fixed_command(
                    args,
                    server,
                    "lset_middle",
                    f"LSET {middle}",
                    [
                        spec.list_command("LSET"),
                        key,
                        middle,
                        "s" * args.value_bytes,
                    ],
                ),
            ]

            pivot = client.command(spec.list_command("LINDEX"), key, middle)
            if not isinstance(pivot, bytes):
                raise RuntimeError("middle pivot disappeared before LINSERT phase")
            operations.append(
                benchmark_pipeline(
                    client,
                    args,
                    "middle_insert_remove",
                    "LINSERT before middle pivot + LREM inserted value",
                    args.middle_pairs,
                    middle_insert_remove_commands(
                        key,
                        pivot,
                        args.middle_pairs,
                        args.value_bytes,
                        spec.list_command("LINSERT"),
                        spec.list_command("LREM"),
                    ),
                )
            )
            operations.append(
                benchmark_pipeline(
                    client,
                    args,
                    "head_stack",
                    "LPUSH + LPOP",
                    args.endpoint_pairs,
                    paired_endpoint_commands(
                        key,
                        args.endpoint_pairs,
                        args.value_bytes,
                        spec.list_command("LPUSH"),
                        spec.list_command("LPOP"),
                        "h",
                    ),
                )
            )
            operations.append(
                benchmark_pipeline(
                    client,
                    args,
                    "tail_stack",
                    "RPUSH + RPOP",
                    args.endpoint_pairs,
                    paired_endpoint_commands(
                        key,
                        args.endpoint_pairs,
                        args.value_bytes,
                        spec.list_command("RPUSH"),
                        spec.list_command("RPOP"),
                        "t",
                    ),
                )
            )
            operations.append(
                benchmark_pipeline(
                    client,
                    args,
                    "counted_head_pop",
                    f"LPUSH {args.pop_width} values + LPOP count={args.pop_width}",
                    args.counted_pop_pairs,
                    counted_pop_commands(
                        key,
                        args.counted_pop_pairs,
                        args.pop_width,
                        args.value_bytes,
                        spec.list_command("LPUSH"),
                        spec.list_command("LPOP"),
                    ),
                )
            )
            operations.append(
                benchmark_pipeline(
                    client,
                    args,
                    "queue",
                    "RPUSH + LPOP",
                    args.endpoint_pairs,
                    paired_endpoint_commands(
                        key,
                        args.endpoint_pairs,
                        args.value_bytes,
                        spec.list_command("RPUSH"),
                        spec.list_command("LPOP"),
                        "q",
                    ),
                )
            )

            final_length = client.command(spec.list_command("LLEN"), key)
            if final_length != args.members:
                raise RuntimeError(
                    f"stable-length operations left {final_length} items, "
                    f"expected {args.members}"
                )
            time.sleep(args.settle_seconds)
            after_operations = memory_sample(client, server, spec, key)

            return EngineResult(
                label=spec.label,
                kind=spec.kind,
                binary=str(spec.binary),
                load_commands=load_commands,
                load_seconds=load_seconds,
                load_items_per_second=args.members / load_seconds,
                memory_baseline=baseline,
                memory_after_load=after_load,
                memory_after_operations=after_operations,
                operations=operations,
            )
        finally:
            client.close()
    finally:
        server.stop()


def fmt(value: float | int | None, digits: int = 2) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):,.{digits}f}"


def delta(after: float | None, before: float | None) -> float | None:
    if after is None or before is None:
        return None
    return after - before


def goblin_stat(sample: MemorySample, name: str) -> int | None:
    if sample.goblin_memory is None:
        return None
    value = sample.goblin_memory.get(name)
    return value if isinstance(value, int) else None


def report_lines(args: argparse.Namespace, results: Sequence[EngineResult]) -> list[str]:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    labels = [result.label for result in results]
    by_label = {result.label: result for result in results}
    operation_names = [op.name for op in results[0].operations]

    lines = [
        "# Goblin Core 100k List Benchmark",
        "",
        f"Generated on `{socket.gethostname()}` at {generated_at}.",
        "",
        "## Method",
        "",
        f"- `{args.members:,}` distinct `{args.value_bytes}`-byte values in one list.",
        f"- Load: multi-value `RPUSH` batches of `{args.load_batch}`, pipeline depth "
        f"`{args.pipeline}`.",
        f"- Fixed-command rates: `{args.requests:,}` requests, one client, pipeline "
        f"depth `{args.pipeline}`, median of `{args.rounds}` `redis-benchmark` runs.",
        "- Compound rates use the repository's existing RESP pipeline client and "
        "keep the list length constant.",
        "- Goblin implementation engines use their qualified command family "
        "(`goblin-pma` becomes `GOBLIN.PMA.*`); `goblin` exercises the selected "
        "standard aliases.",
        "- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one "
        "proactor thread for single-core parity. The intended target is a modest "
        "single-core memory server; the quiet benchmark host is a 64-core machine. "
        "Engines run one at a time.",
        "- RSS is external `ps` RSS. `INFO used_memory` and `MEMORY USAGE` are "
        "reported independently; RSS/INFO deltas subtract the empty-server baseline.",
        "- Per-key bytes are `MEMORY USAGE` on incumbents and "
        "`GOBLIN.MEMORY total_allocated_bytes` on Goblin Core.",
        "",
        "## Load",
        "",
        "| Engine | items/s | seconds | RPUSH commands |",
        "| --- | ---: | ---: | ---: |",
    ]
    for result in results:
        lines.append(
            f"| `{result.label}` | {result.load_items_per_second:,.0f} | "
            f"{result.load_seconds:.4f} | {result.load_commands:,} |"
        )

    lines += [
        "",
        "## Operations",
        "",
        "Logical operations per second. Fixed commands use the C load generator; "
        "compound rows count a two-command pair as one logical operation.",
        "",
        "| Operation | " + " | ".join(labels) + " |",
        "| --- | " + " | ".join("---:" for _ in labels) + " |",
    ]
    for name in operation_names:
        description = next(op.description for op in results[0].operations if op.name == name)
        cells = []
        for label in labels:
            op = next(op for op in by_label[label].operations if op.name == name)
            cells.append(f"{op.logical_operations_per_second:,.0f}")
        lines.append(f"| `{description}` | " + " | ".join(cells) + " |")

    lines += [
        "",
        "## Memory After Load",
        "",
        "| Engine | RSS MiB | RSS delta MiB | RSS delta B/item | INFO used MiB | "
        "INFO delta B/item | key-reported B/item |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        base = result.memory_baseline
        loaded = result.memory_after_load
        rss_delta = loaded.rss_mib - base.rss_mib
        used_delta_mib = delta(
            loaded.info_used_memory_mib, base.info_used_memory_mib
        )
        key_bytes = (
            loaded.key_memory_usage_bytes
            if loaded.key_memory_usage_bytes is not None
            else goblin_stat(loaded, "total_allocated_bytes")
        )
        lines.append(
            f"| `{result.label}` | {loaded.rss_mib:.2f} | {rss_delta:.2f} | "
            f"{rss_delta * 1024 * 1024 / args.members:.2f} | "
            f"{fmt(loaded.info_used_memory_mib)} | "
            f"{fmt(None if used_delta_mib is None else used_delta_mib * 1024 * 1024 / args.members)} | "
            f"{fmt(None if key_bytes is None else key_bytes / args.members)} |"
        )

    lines += [
        "",
        "## Memory After Operations",
        "",
        "| Engine | RSS MiB | INFO used MiB | key bytes/item | list length |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        sample = result.memory_after_operations
        key_bytes = (
            sample.key_memory_usage_bytes
            if sample.key_memory_usage_bytes is not None
            else goblin_stat(sample, "total_allocated_bytes")
        )
        lines.append(
            f"| `{result.label}` | {sample.rss_mib:.2f} | "
            f"{fmt(sample.info_used_memory_mib)} | "
            f"{fmt(None if key_bytes is None else key_bytes / args.members)} | "
            f"{sample.list_length if sample.list_length is not None else 'n/a'} |"
        )

    goblin = next(
        (result for result in results if result.kind.startswith("goblin")), None
    )
    if goblin is not None:
        lines += [
            "",
            "## Goblin List Internals",
            "",
            "| Phase | elements | live value MiB | dead value MiB | value alloc MiB | "
            "order capacity | front slack | back slack | order alloc MiB | "
            "total alloc MiB |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for phase, sample in (
            ("after load", goblin.memory_after_load),
            ("after operations", goblin.memory_after_operations),
        ):
            def mib_stat(name: str) -> str:
                value = goblin_stat(sample, name)
                return fmt(None if value is None else value / (1024 * 1024))

            lines.append(
                f"| `{phase}` | {goblin_stat(sample, 'element_count') or 0:,} | "
                f"{mib_stat('value_live_bytes')} | {mib_stat('value_dead_bytes')} | "
                f"{mib_stat('value_allocated_bytes')} | "
                f"{goblin_stat(sample, 'order_capacity') or 0:,} | "
                f"{goblin_stat(sample, 'order_front_slack') or 0:,} | "
                f"{goblin_stat(sample, 'order_back_slack') or 0:,} | "
                f"{mib_stat('order_allocated_bytes')} | "
                f"{mib_stat('total_allocated_bytes')} |"
            )

    lines += [
        "",
        "## Binaries",
        "",
    ]
    for result in results:
        lines.append(f"- `{result.label}`: `{result.binary}`")
    lines.append("")
    return lines


def write_outputs(args: argparse.Namespace, results: Sequence[EngineResult]) -> None:
    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "host": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "logical_cpus": os.cpu_count(),
        },
        "config": {
            "members": args.members,
            "value_bytes": args.value_bytes,
            "load_batch": args.load_batch,
            "pipeline": args.pipeline,
            "requests": args.requests,
            "rounds": args.rounds,
            "range_size": args.range_size,
            "endpoint_pairs": args.endpoint_pairs,
            "middle_pairs": args.middle_pairs,
            "counted_pop_pairs": args.counted_pop_pairs,
            "pop_width": args.pop_width,
            "settle_seconds": args.settle_seconds,
            "standard_list_implementation": args.standard_list_implementation,
            "goblin_max_density": args.goblin_max_density,
            "redis_benchmark": str(args.redis_benchmark),
        },
        "results": [asdict(result) for result in results],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")
    args.report.write_text("\n".join(report_lines(args, results)))


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--engine",
        action="append",
        type=parse_engine,
        required=True,
        metavar="LABEL:KIND:PATH",
        help="engine to test; repeatable",
    )
    parser.add_argument(
        "--redis-benchmark",
        type=Path,
        default=Path("redis-benchmark"),
        help="C load generator used for fixed-command throughput",
    )
    parser.add_argument(
        "--standard-list-implementation",
        default="pma",
        choices=["pma"],
        help="Target selected by standard list commands on Goblin Core.",
    )
    parser.add_argument(
        "--goblin-max-density",
        type=float,
        help="Override --list-max-density for Goblin engines during a sweep.",
    )
    parser.add_argument("--members", type=int, default=100_000)
    parser.add_argument("--value-bytes", type=int, default=16)
    parser.add_argument("--load-batch", type=int, default=128)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--requests", type=int, default=200_000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--range-size", type=int, default=16)
    parser.add_argument("--endpoint-pairs", type=int, default=50_000)
    parser.add_argument("--middle-pairs", type=int, default=250)
    parser.add_argument("--counted-pop-pairs", type=int, default=20_000)
    parser.add_argument("--pop-width", type=int, default=8)
    parser.add_argument("--settle-seconds", type=float, default=0.5)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument(
        "--output-json",
        type=Path,
        default=ROOT / "benchmark-results" / "list-100k.json",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=ROOT / "benchmark-results" / "list-100k.md",
    )
    args = parser.parse_args(argv)

    if min(
        args.members,
        args.value_bytes,
        args.load_batch,
        args.pipeline,
        args.requests,
        args.rounds,
        args.range_size,
        args.endpoint_pairs,
        args.middle_pairs,
        args.counted_pop_pairs,
        args.pop_width,
    ) <= 0:
        parser.error("all count, width, pipeline, request, and round values must be positive")
    if args.value_bytes < 8:
        parser.error("--value-bytes must be at least 8")
    if args.range_size > args.members:
        parser.error("--range-size cannot exceed --members")
    if args.settle_seconds < 0 or args.timeout <= 0:
        parser.error("--settle-seconds must be non-negative and --timeout positive")
    if args.goblin_max_density is not None and not (
        0.0 < args.goblin_max_density <= 1.0
    ):
        parser.error("--goblin-max-density must be in (0, 1]")

    try:
        args.redis_benchmark = zbench.resolve_executable(
            args.redis_benchmark, "redis-benchmark"
        )
        for spec in args.engine:
            zbench.resolve_executable(spec.binary, f"{spec.label} binary")
    except FileNotFoundError as error:
        parser.error(str(error))
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    results: list[EngineResult] = []
    for spec in args.engine:
        print(f"benchmarking {spec.label}...", file=sys.stderr, flush=True)
        result = run_engine(spec, args)
        results.append(result)
        middle = next(op for op in result.operations if op.name == "lindex_middle")
        rss_delta = result.memory_after_load.rss_mib - result.memory_baseline.rss_mib
        print(
            f"  load {result.load_items_per_second:,.0f} items/s, "
            f"middle LINDEX {middle.logical_operations_per_second:,.0f} ops/s, "
            f"RSS delta {rss_delta:.2f} MiB",
            file=sys.stderr,
            flush=True,
        )
    write_outputs(args, results)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main(sys.argv[1:]))
