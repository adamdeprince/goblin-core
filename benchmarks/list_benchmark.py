#!/usr/bin/env python3
"""Benchmark a 100k-item Redis list across Goblin Core and incumbents.

The harness reuses zset_benchmark.py's RESP client, parity configuration,
server launchers, redis-benchmark driver, RSS probe, and INFO/GOBLIN.MEMORY
readers. It populates one fixed-width list through pipelined multi-value RPUSH,
measures rank-sensitive reads and writes at several positions, then exercises
stable-length endpoint and pivot churn. Every engine runs alone.

Example:
  ./benchmarks/list_benchmark.py \
    --engine goblin-resp-tcp:goblin:/path/to/goblin-core \
    --engine goblin-sbe-ring:goblin-sbe:/path/to/goblin-core \
    --sbe-benchmark /path/to/goblin_core_list_sbe_benchmark \
    --engine redis-7.2.4:redis:/path/to/redis-7.2.4/redis-server \
    --engine redis-8.8:redis:/path/to/redis-8.8/redis-server \
    --engine valkey-9.1:redis:/path/to/valkey-9.1/valkey-server \
    --engine dragonfly:dragonfly:/path/to/dragonfly
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import shutil
import signal
import socket
import statistics
import subprocess
import tempfile
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
    def is_sbe(self) -> bool:
        return self.kind == "goblin-sbe"

    @property
    def list_prefix(self) -> str:
        if self.kind in ("goblin", "goblin-sbe"):
            return ""
        if self.kind.startswith("goblin-"):
            implementation = self.kind.removeprefix("goblin-")
            return f"GOBLIN.{implementation.upper()}."
        return ""

    def list_command(self, name: str) -> str:
        return f"{self.list_prefix}{name}"

    def supports_list_command(self, name: str) -> bool:
        if self.kind != "mini-redis-go":
            return True
        return name in {"LLEN", "LRANGE", "LPUSH", "RPUSH", "LPOP", "RPOP"}


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
    if kind not in ("redis", "dragonfly", "mini-redis-go") and not valid_goblin_kind:
        raise argparse.ArgumentTypeError(
            "engine kind must be redis, dragonfly, mini-redis-go, goblin, or "
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
        ring_dir: Path | None = None
        if spec.is_sbe:
            ring_dir = Path(tempfile.mkdtemp(prefix="goblin-list-sbe-bench-"))
            ring_path = ring_dir / "requests.ring"
            extra_args.extend(["--ring", str(ring_path), args.ring_size])
        try:
            server = zbench.start_goblin(
                spec.binary,
                rank_cache=False,
                rank_cache_mode="off",
                extra_args=extra_args,
            )
        except Exception:
            if ring_dir is not None:
                shutil.rmtree(ring_dir, ignore_errors=True)
            raise
        if ring_dir is not None:
            server.temp_dir = ring_dir
            server.ring_path = ring_dir / "requests.ring"
        pin_process(server.process.pid, args.server_core)
        return server
    if spec.kind == "redis":
        server = zbench.start_redis(spec.binary)
    elif spec.kind == "mini-redis-go":
        server = zbench.start_mini_redis(spec.binary)
    else:
        server = zbench.start_dragonfly(spec.binary)
    pin_process(server.process.pid, args.server_core)
    return server


def pin_process(pid: int, core: int) -> None:
    if core < 0:
        return
    if not hasattr(os, "sched_setaffinity"):
        return
    os.sched_setaffinity(pid, {core})


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
        rss_mib=(
            zbench.process_ps_rss_mib(server.process.pid)
            if spec.kind == "mini-redis-go"
            else zbench.process_rss_mib(server.process.pid)
        ),
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
        expected = item_value(index, value_bytes).encode()
        if spec.supports_list_command("LINDEX"):
            response = client.command(spec.list_command("LINDEX"), key, index)
        else:
            response = client.command(spec.list_command("LRANGE"), key, index, index)
            response = response[0] if isinstance(response, list) and response else None
        if response != expected:
            raise RuntimeError(
                f"loaded list value at {index} is {response!r}, expected {expected!r}"
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


def benchmark_sbe_phase(
    args: argparse.Namespace,
    server: zbench.ServerProcess,
    action: str,
    key: str,
    **options: object,
) -> tuple[int, float]:
    if server.ring_path is None:
        raise RuntimeError("SBE list benchmark server has no ring path")
    command = [
        str(args.sbe_benchmark),
        "--ring",
        str(server.ring_path),
        "--action",
        action,
        "--key",
        key,
        "--pipeline",
        str(args.pipeline),
    ]
    for name, value in options.items():
        command.extend([f"--{name.replace('_', '-')}", str(value)])
    completed = subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=args.timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"SBE list phase {action!r} failed: {completed.stderr.strip()}"
        )
    fields: dict[str, str] = {}
    for line in completed.stdout.splitlines():
        name, separator, value = line.partition("=")
        if separator:
            fields[name] = value
    try:
        return int(fields["commands"]), float(fields["seconds"])
    except (KeyError, ValueError) as error:
        raise RuntimeError(
            f"SBE list phase {action!r} returned malformed output: "
            f"{completed.stdout!r}"
        ) from error


def run_engine(spec: EngineSpec, args: argparse.Namespace) -> EngineResult:
    server = start_engine(spec, args)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            key = f"listbench:{os.getpid()}:{spec.label}"
            baseline = memory_sample(client, server, spec, key)

            if spec.is_sbe:
                load_commands, load_seconds = benchmark_sbe_phase(
                    args,
                    server,
                    "load",
                    key,
                    members=args.members,
                    value_bytes=args.value_bytes,
                    batch=args.load_batch,
                    implementation="selected",
                )
            else:
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

            operations: list[OperationResult] = []

            def add_sbe_phase(
                name: str,
                description: str,
                logical_operations: int,
                action: str,
                **options: object,
            ) -> None:
                command_count, seconds = benchmark_sbe_phase(
                    args, server, action, key, **options
                )
                operations.append(
                    operation(
                        name,
                        description,
                        f"native C++ SBE over shared-memory ring; pipeline depth {args.pipeline}",
                        logical_operations,
                        command_count,
                        seconds,
                    )
                )

            def add_fixed(
                command_name: str,
                name: str,
                description: str,
                *arguments: object,
            ) -> None:
                if spec.supports_list_command(command_name):
                    if spec.is_sbe:
                        options: dict[str, object] = {
                            "operation": command_name,
                            "requests": args.requests,
                            "value_bytes": args.value_bytes,
                        }
                        if command_name in ("LINDEX", "LSET"):
                            options["index"] = arguments[0]
                        elif command_name == "LRANGE":
                            options["start"] = arguments[0]
                            options["stop"] = arguments[1]
                        rates: list[tuple[int, float]] = []
                        for _ in range(args.rounds):
                            rates.append(
                                benchmark_sbe_phase(
                                    args, server, "fixed", key, **options
                                )
                            )
                        rates.sort(key=lambda result: result[1])
                        command_count, seconds = rates[len(rates) // 2]
                        operations.append(
                            operation(
                                name,
                                description,
                                f"native C++ SBE/ring median of {args.rounds}; "
                                f"pipeline depth {args.pipeline}",
                                args.requests,
                                command_count,
                                seconds,
                            )
                        )
                    else:
                        operations.append(
                            benchmark_fixed_command(
                                args,
                                server,
                                name,
                                description,
                                [spec.list_command(command_name), key, *arguments],
                            )
                        )

            add_fixed("LLEN", "llen", "LLEN control")
            add_fixed("LINDEX", "lindex_head", "LINDEX 0", 0)
            add_fixed("LINDEX", "lindex_quarter", f"LINDEX {quarter}", quarter)
            add_fixed("LINDEX", "lindex_middle", f"LINDEX {middle}", middle)
            add_fixed(
                "LINDEX",
                "lindex_three_quarters",
                f"LINDEX {three_quarters}",
                three_quarters,
            )
            add_fixed("LINDEX", "lindex_tail", "LINDEX -1", -1)
            add_fixed(
                "LRANGE",
                "lrange_middle",
                f"LRANGE {range_start} {range_stop}",
                range_start,
                range_stop,
            )
            add_fixed(
                "LSET",
                "lset_middle",
                f"LSET {middle}",
                middle,
                "s" * args.value_bytes,
            )

            if spec.supports_list_command("LINSERT") and spec.supports_list_command(
                "LREM"
            ):
                if spec.is_sbe:
                    add_sbe_phase(
                        "middle_insert_remove",
                        "LINSERT before middle pivot + LREM inserted value",
                        args.middle_pairs,
                        "middle",
                        pairs=args.middle_pairs,
                        index=middle,
                        value_bytes=args.value_bytes,
                    )
                else:
                    pivot = client.command(spec.list_command("LINDEX"), key, middle)
                    if not isinstance(pivot, bytes):
                        raise RuntimeError(
                            "middle pivot disappeared before LINSERT phase"
                        )
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
            if spec.supports_list_command("LPUSH") and spec.supports_list_command(
                "LPOP"
            ):
                if spec.is_sbe:
                    add_sbe_phase(
                        "head_stack",
                        "LPUSH + LPOP",
                        args.endpoint_pairs,
                        "endpoint",
                        pairs=args.endpoint_pairs,
                        value_bytes=args.value_bytes,
                        push="LPUSH",
                        pop="LPOP",
                        prefix="h",
                    )
                else:
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
            if spec.supports_list_command("RPUSH") and spec.supports_list_command(
                "RPOP"
            ):
                if spec.is_sbe:
                    add_sbe_phase(
                        "tail_stack",
                        "RPUSH + RPOP",
                        args.endpoint_pairs,
                        "endpoint",
                        pairs=args.endpoint_pairs,
                        value_bytes=args.value_bytes,
                        push="RPUSH",
                        pop="RPOP",
                        prefix="t",
                    )
                else:
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
            if spec.kind != "mini-redis-go":
                if spec.is_sbe:
                    add_sbe_phase(
                        "counted_head_pop",
                        f"LPUSH {args.pop_width} values + LPOP count={args.pop_width}",
                        args.counted_pop_pairs,
                        "counted",
                        pairs=args.counted_pop_pairs,
                        width=args.pop_width,
                        value_bytes=args.value_bytes,
                    )
                else:
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
            if spec.supports_list_command("RPUSH") and spec.supports_list_command(
                "LPOP"
            ):
                if spec.is_sbe:
                    add_sbe_phase(
                        "queue",
                        "RPUSH + LPOP",
                        args.endpoint_pairs,
                        "endpoint",
                        pairs=args.endpoint_pairs,
                        value_bytes=args.value_bytes,
                        push="RPUSH",
                        pop="LPOP",
                        prefix="q",
                    )
                else:
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
                binary=spec.binary.name,
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
    operation_descriptions = {
        "llen": "LLEN control",
        "lindex_head": "LINDEX 0",
        "lindex_quarter": f"LINDEX {args.members // 4}",
        "lindex_middle": f"LINDEX {args.members // 2}",
        "lindex_three_quarters": f"LINDEX {args.members * 3 // 4}",
        "lindex_tail": "LINDEX -1",
        "lrange_middle": (
            f"LRANGE {max(0, args.members // 2 - args.range_size // 2)} "
            f"{min(args.members - 1, max(0, args.members // 2 - args.range_size // 2) + args.range_size - 1)}"
        ),
        "lset_middle": f"LSET {args.members // 2}",
        "middle_insert_remove": "LINSERT before middle pivot + LREM inserted value",
        "head_stack": "LPUSH + LPOP",
        "tail_stack": "RPUSH + RPOP",
        "counted_head_pop": (
            f"LPUSH {args.pop_width} values + LPOP count={args.pop_width}"
        ),
        "queue": "RPUSH + LPOP",
    }
    operation_names = list(operation_descriptions)

    def find_operation(
        result: EngineResult, name: str
    ) -> OperationResult | None:
        return next((op for op in result.operations if op.name == name), None)

    summary: list[str] = []
    goblins = [
        result for result in results if result.kind.startswith("goblin")
    ]
    incumbents = [
        result for result in results if not result.kind.startswith("goblin")
    ]
    if goblins and incumbents:
        fixed_names = operation_names[:8]
        incumbent_middle = max(
            op.logical_operations_per_second
            for result in incumbents
            if (op := find_operation(result, "lindex_middle")) is not None
        )
        best_incumbent_rss_per_item = min(
            (result.memory_after_load.rss_mib - result.memory_baseline.rss_mib)
            * 1024
            * 1024
            / args.members
            for result in incumbents
        )
        incumbent_key_per_item = [
            result.memory_after_load.key_memory_usage_bytes / args.members
            for result in incumbents
            if result.memory_after_load.key_memory_usage_bytes is not None
        ]
        goblin_internal = [
            (result, goblin_stat(result.memory_after_load,
                                 "total_allocated_bytes"))
            for result in goblins
        ]
        goblin_internal = [
            (result, allocated / args.members)
            for result, allocated in goblin_internal
            if allocated is not None
        ]
        fastest_load = max(result.load_items_per_second for result in incumbents)
        goblin_sentences = []
        for goblin in goblins:
            fixed_wins = 0
            for name in fixed_names:
                goblin_op = find_operation(goblin, name)
                incumbent_ops = [
                    op.logical_operations_per_second
                    for result in incumbents
                    if (op := find_operation(result, name)) is not None
                ]
                if (
                    goblin_op is not None
                    and incumbent_ops
                    and goblin_op.logical_operations_per_second > max(incumbent_ops)
                ):
                    fixed_wins += 1
            goblin_middle_op = find_operation(goblin, "lindex_middle")
            if goblin_middle_op is None:
                raise RuntimeError("Goblin result omitted middle-list LINDEX")
            goblin_middle = goblin_middle_op.logical_operations_per_second
            goblin_rss_per_item = (
                goblin.memory_after_load.rss_mib
                - goblin.memory_baseline.rss_mib
            ) * 1024 * 1024 / args.members
            load_delta = (
                goblin.load_items_per_second / fastest_load - 1.0
            ) * 100
            load_comparison = (
                f"`{load_delta:.1f}%` ahead of"
                if load_delta >= 0
                else f"`{-load_delta:.1f}%` behind"
            )
            goblin_sentences.append(
                f"`{goblin.label}` leads `{fixed_wins}` of "
                f"`{len(fixed_names)}` fixed-command rows, reaches "
                f"`{goblin_middle / incumbent_middle:.2f}x` the fastest "
                f"incumbent on middle-list `LINDEX`, population is "
                f"{load_comparison} "
                f"the fastest incumbent, and uses `{goblin_rss_per_item:.2f}` "
                "RSS-delta bytes/item."
            )
        allocation_sentence = ""
        if goblin_internal and incumbent_key_per_item:
            leanest_goblin, leanest_goblin_bytes = min(
                goblin_internal, key=lambda item: item[1]
            )
            allocation_sentence = (
                f" `{leanest_goblin.label}` is the leanest key representation at "
                f"`{leanest_goblin_bytes:.2f}` accounted bytes/item versus "
                f"`{min(incumbent_key_per_item):.2f}` from the leanest incumbent's "
                "key-level counter."
            )
        summary = [
            "## Summary",
            "",
            " ".join(goblin_sentences) + allocation_sentence +
            " The leanest incumbent uses "
            f"`{best_incumbent_rss_per_item:.2f}` RSS-delta bytes/item. TCP "
            "compound rows use the Python RESP pipeline; ring rows use the native "
            "C++ SBE client at the same pipeline depth. Fixed TCP rows use "
            "`redis-benchmark`.",
            "",
        ]

    lines = [
        "# Goblin Core 100k List Benchmark",
        "",
        f"Generated on a dedicated benchmark host at {generated_at}.",
        "",
        *summary,
        "## Method",
        "",
        f"- `{args.members:,}` distinct `{args.value_bytes}`-byte values in one list.",
        f"- Population: multi-value `RPUSH` batches of `{args.load_batch}`. "
        f"RESP/TCP and SBE/ring both use pipeline depth `{args.pipeline}`.",
        "- Population measures command ingestion, not Goblin Core native "
        "snapshot restoration; native restore reconstructs each list in one bulk "
        "operation and uses the compatible raw-copy accelerator when present.",
        f"- Fixed-command rates: `{args.requests:,}` requests and one client. "
        f"RESP/TCP uses pipeline depth `{args.pipeline}` and the median of "
        f"`{args.rounds}` `redis-benchmark` runs. SBE/ring uses the median of "
        f"`{args.rounds}` native C++ runs at the same pipeline depth.",
        "- Compound rates keep the list length constant. TCP engines use the "
        "existing RESP pipeline client; the ring row uses the native typed SBE client.",
        "- Transport matrix: Goblin Core is measured as `goblin-resp-tcp` and "
        "`goblin-sbe-ring`; every incumbent is measured over RESP/TCP. No UDS row "
        f"is included. The ring row uses a `{args.ring_size}` request and reply ring.",
        f"- Linux affinity: server core `{args.server_core}`, client/load-generator "
        f"core `{args.client_core}` (`-1` means unpinned). Ring client and server "
        "must not share a core.",
        "- Goblin implementation engines use their qualified command family "
        "(`goblin-pma` becomes `GOBLIN.PMA.*`); `goblin` exercises the selected "
        "standard aliases.",
        "- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one "
        "proactor thread for single-core parity. The intended target is a modest "
        "single-core memory server; the quiet benchmark host is a 64-core machine. "
        "Engines run one at a time.",
        "- mini-redis-go runs as an external TCP server with `GOMAXPROCS=1`, "
        "AOF disabled, and metrics disabled. Unsupported commands are shown as "
        "`n/a`, not timed error replies.",
        "- RSS is read from the launched server PID: mini-redis-go uses "
        "`ps -o rss=`, while the other engines use Linux `/proc/<pid>/status` "
        "as `VmRSS + HugetlbPages`. No server-reported RSS field is used. "
        "`INFO used_memory` and `MEMORY USAGE` are independent "
        "corroborating counters; RSS/INFO deltas subtract the empty-server baseline.",
        "- Per-key bytes are `MEMORY USAGE` where an incumbent exposes it and "
        "`GOBLIN.MEMORY total_allocated_bytes` on Goblin Core. mini-redis-go "
        "exposes neither `INFO memory` nor `MEMORY USAGE`, so its RSS is the "
        "cross-engine memory measurement.",
        "- Incumbents are exercised strictly as black-box RESP servers; their "
        "source code is not inspected.",
        "",
        "## Population",
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
        "Logical operations per second. Fixed TCP commands use `redis-benchmark`; "
        "ring commands use the native C++ SBE client. Compound rows count a "
        "two-command pair as one logical operation.",
        "",
        "| Operation | " + " | ".join(labels) + " |",
        "| --- | " + " | ".join("---:" for _ in labels) + " |",
    ]
    for name in operation_names:
        cells = []
        for label in labels:
            op = find_operation(by_label[label], name)
            cells.append(
                "n/a" if op is None else f"{op.logical_operations_per_second:,.0f}"
            )
        lines.append(
            f"| `{operation_descriptions[name]}` | " + " | ".join(cells) + " |"
        )

    lines += [
        "",
        "## Memory After Population",
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

    if goblins:
        lines += [
            "",
            "## Goblin List Internals",
            "",
            "| Engine | representation | phase | elements | live value MiB | dead value MiB | value alloc MiB | "
            "slots/leaves | front slack | back slack | order alloc MiB | "
            "total alloc MiB |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for goblin in goblins:
            for phase, sample in (
                ("after population", goblin.memory_after_load),
                ("after operations", goblin.memory_after_operations),
            ):
                def mib_stat(name: str) -> str:
                    value = goblin_stat(sample, name)
                    return fmt(None if value is None else value / (1024 * 1024))

                representation = (
                    sample.goblin_memory.get("implementation", "unknown")
                    if sample.goblin_memory is not None
                    else "unknown"
                )
                lines.append(
                    f"| `{goblin.label}` | `{representation}` | `{phase}` | "
                    f"{goblin_stat(sample, 'element_count') or 0:,} | "
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
        "## Tested Servers",
        "",
    ]
    for result in results:
        lines.append(f"- `{result.label}`")
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
            "ring_size": args.ring_size,
            "requests": args.requests,
            "rounds": args.rounds,
            "range_size": args.range_size,
            "endpoint_pairs": args.endpoint_pairs,
            "middle_pairs": args.middle_pairs,
            "counted_pop_pairs": args.counted_pop_pairs,
            "pop_width": args.pop_width,
            "settle_seconds": args.settle_seconds,
            "server_core": args.server_core,
            "client_core": args.client_core,
            "standard_list_implementation": args.standard_list_implementation,
            "goblin_max_density": args.goblin_max_density,
            "redis_benchmark": args.redis_benchmark.name,
            "sbe_benchmark": (
                args.sbe_benchmark.name
                if any(spec.is_sbe for spec in args.engine)
                else None
            ),
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
        "--sbe-benchmark",
        type=Path,
        default=Path("goblin_core_list_sbe_benchmark"),
        help="native workload runner used by goblin-sbe engines",
    )
    parser.add_argument(
        "--standard-list-implementation",
        default="segmented",
        choices=["pma", "segmented"],
        help=(
            "Target selected by standard list commands on Goblin Core "
            "(default: segmented)."
        ),
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
    parser.add_argument(
        "--ring-size",
        default="2mb",
        help="per-direction Goblin SBE ring size (default: 2mb on this x86 suite)",
    )
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
        "--server-core",
        type=int,
        default=-1,
        help="pin each launched server to this Linux CPU (-1 disables)",
    )
    parser.add_argument(
        "--client-core",
        type=int,
        default=-1,
        help="pin this harness and child load generators (-1 disables)",
    )
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
    if args.server_core < -1 or args.client_core < -1:
        parser.error("CPU cores must be non-negative or -1")
    if args.goblin_max_density is not None and not (
        0.0 < args.goblin_max_density <= 1.0
    ):
        parser.error("--goblin-max-density must be in (0, 1]")

    try:
        args.redis_benchmark = zbench.resolve_executable(
            args.redis_benchmark, "redis-benchmark"
        )
        if any(spec.is_sbe for spec in args.engine):
            args.sbe_benchmark = zbench.resolve_executable(
                args.sbe_benchmark, "Goblin SBE list benchmark"
            )
        for spec in args.engine:
            zbench.resolve_executable(spec.binary, f"{spec.label} binary")
    except FileNotFoundError as error:
        parser.error(str(error))
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    pin_process(0, args.client_core)
    results: list[EngineResult] = []
    for spec in args.engine:
        print(f"benchmarking {spec.label}...", file=sys.stderr, flush=True)
        result = run_engine(spec, args)
        results.append(result)
        middle = next(
            (op for op in result.operations if op.name == "lindex_middle"), None
        )
        rss_delta = result.memory_after_load.rss_mib - result.memory_baseline.rss_mib
        middle_rate = (
            "n/a"
            if middle is None
            else f"{middle.logical_operations_per_second:,.0f} ops/s"
        )
        transport = "SBE/ring" if spec.is_sbe else "RESP/TCP"
        print(
            f"  {transport} population {result.load_items_per_second:,.0f} items/s, "
            f"middle LINDEX {middle_rate}, "
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
