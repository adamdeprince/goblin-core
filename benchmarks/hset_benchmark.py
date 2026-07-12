#!/usr/bin/env python3
"""Benchmark HSET speed and trusted INFO RSS across Goblin and incumbents.

The runner reuses zset_benchmark.py's RESP client, server launchers, parity
configuration, pipelining, redis-benchmark driver, and GOBLIN.MEMORY reader.
Each scenario starts a fresh server and reads both `used_memory_rss` and
`used_memory` from `INFO memory` at every checkpoint.

Two workloads cover the hash representation boundary:

* one large hash: distinct bulk inserts, random existing-field HSET, a
  deterministic 16-byte-to-64-byte value growth pass, and Goblin compaction;
* many hashes: a fixed total field count swept across 8/32/64/128 fields per
  hash, with one multi-field HSET per hash and random-key middle-field updates.

Example (naamah):
  ./benchmarks/hset_benchmark.py \
    --engine goblin:goblin:./build-release/goblin-core \
    --engine redis-7.2.4:redis:$HOME/bench/redis-7.2.4/src/redis-server \
    --engine redis-8.8:redis:$HOME/bench/redis-8.8.0/src/redis-server \
    --engine valkey-9.1:redis:$HOME/bench/valkey-9.1.0/src/valkey-server \
    --engine dragonfly:dragonfly:$HOME/dragonfly/build-opt/dragonfly \
    --redis-benchmark $HOME/bench/redis-7.2.4/src/redis-benchmark
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
RANDOM_TOKEN_DIGITS = 12


@dataclass(frozen=True)
class EngineSpec:
    label: str
    kind: str
    binary: Path

    @property
    def is_goblin(self) -> bool:
        return self.kind == "goblin"


@dataclass
class MemorySample:
    rss_bytes: int
    used_memory_bytes: int | None
    key_memory_bytes: float | None
    goblin_memory: dict[str, int | str] | None


@dataclass
class OptimizeResult:
    seconds: float
    reclaimed_bytes: int | None


@dataclass
class LargeHashResult:
    engine: str
    kind: str
    binary: str
    fields: int
    field_bytes: int
    initial_value_bytes: int
    grown_value_bytes: int
    load_commands: int
    load_seconds: float
    load_fields_per_second: float
    update_hset_operations_per_second: float
    grow_commands: int
    grow_seconds: float
    grow_fields_per_second: float
    baseline: MemorySample
    loaded_before_optimize: MemorySample
    loaded_after_optimize: MemorySample
    after_same_width_updates: MemorySample
    grown_before_optimize: MemorySample
    grown_after_optimize: MemorySample
    initial_optimize: OptimizeResult
    grown_optimize: OptimizeResult


@dataclass
class SmallHashResult:
    engine: str
    kind: str
    binary: str
    hashes: int
    fields_per_hash: int
    total_fields: int
    key_bytes: int
    field_bytes: int
    value_bytes: int
    load_commands: int
    load_seconds: float
    load_hashes_per_second: float
    load_fields_per_second: float
    update_hset_operations_per_second: float
    baseline: MemorySample
    loaded: MemorySample
    after_updates: MemorySample


def parse_engine(spec: str) -> EngineSpec:
    parts = spec.split(":", 2)
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(
            f"--engine expects LABEL:KIND:PATH, got {spec!r}"
        )
    label, kind, path = parts
    if kind not in ("goblin", "redis", "dragonfly"):
        raise argparse.ArgumentTypeError(
            f"engine kind must be goblin|redis|dragonfly, got {kind!r}"
        )
    return EngineSpec(label, kind, Path(path).expanduser())


def start_engine(spec: EngineSpec) -> zbench.ServerProcess:
    if spec.kind == "goblin":
        return zbench.start_goblin(
            spec.binary, rank_cache=False, rank_cache_mode="off"
        )
    if spec.kind == "redis":
        return zbench.start_redis(spec.binary)
    return zbench.start_dragonfly(spec.binary)


def large_field(item_id: int) -> str:
    return f"field:{item_id:0{RANDOM_TOKEN_DIGITS}d}"


def small_key(hash_id: int) -> str:
    return f"h:{hash_id:0{RANDOM_TOKEN_DIGITS}d}"


def small_field(field_id: int) -> str:
    return f"f:{field_id:03d}"


def fixed_value(item_id: int, width: int, marker: str) -> str:
    digits = width - len(marker)
    if digits <= 0:
        raise ValueError("value width must exceed marker width")
    text = f"{item_id:0{digits}x}"
    if len(text) > digits:
        raise ValueError(f"item id {item_id} does not fit in {width} bytes")
    return marker + text


def large_hset_commands(
    count: int, key: str, batch: int, value_bytes: int, marker: str
) -> Iterable[list[object]]:
    for start in range(0, count, batch):
        command: list[object] = ["HSET", key]
        for item_id in range(start, min(start + batch, count)):
            command.append(large_field(item_id))
            command.append(fixed_value(item_id, value_bytes, marker))
        yield command


def small_hset_commands(
    hashes: int, fields_per_hash: int, value_bytes: int
) -> Iterable[list[object]]:
    ordinal = 0
    for hash_id in range(hashes):
        command: list[object] = ["HSET", small_key(hash_id)]
        for field_id in range(fields_per_hash):
            command.append(small_field(field_id))
            command.append(fixed_value(ordinal, value_bytes, "v"))
            ordinal += 1
        yield command


def memory_usage(client: zbench.RespClient, key: str) -> int | None:
    try:
        response = client.command("MEMORY", "USAGE", key)
    except Exception:
        return None
    return response if isinstance(response, int) else None


def goblin_total_allocated(
    client: zbench.RespClient, key: str
) -> tuple[float | None, dict[str, int | str] | None]:
    stats = zbench.goblin_memory_stats(client, key)
    if stats is None:
        return None, None
    total = stats.get("total_allocated_bytes")
    return (float(total) if isinstance(total, int) else None), stats


def memory_sample(
    client: zbench.RespClient,
    spec: EngineSpec,
    sample_keys: Sequence[str] = (),
) -> MemorySample:
    info = zbench.redis_info_fields(client, "memory")
    try:
        rss_bytes = int(info["used_memory_rss"])
    except (KeyError, ValueError) as error:
        available = ", ".join(sorted(info)) or "none"
        raise RuntimeError(
            f"{spec.label} INFO memory has no integer used_memory_rss "
            f"(available fields: {available})"
        ) from error

    used_memory: int | None
    try:
        used_memory = int(info["used_memory"])
    except (KeyError, ValueError):
        used_memory = None

    reported: list[float] = []
    goblin_stats: dict[str, int | str] | None = None
    for key in sample_keys:
        if spec.is_goblin:
            total, stats = goblin_total_allocated(client, key)
            if total is not None:
                reported.append(total)
            if goblin_stats is None:
                goblin_stats = stats
        else:
            value = memory_usage(client, key)
            if value is not None:
                reported.append(float(value))

    return MemorySample(
        rss_bytes=rss_bytes,
        used_memory_bytes=used_memory,
        key_memory_bytes=statistics.mean(reported) if reported else None,
        goblin_memory=goblin_stats,
    )


def optimize_hash(
    client: zbench.RespClient,
    spec: EngineSpec,
    key: str,
    density: float,
) -> OptimizeResult:
    if not spec.is_goblin:
        return OptimizeResult(0.0, None)
    started = time.perf_counter()
    response = client.command("GOBLIN.OPTIMIZE", key, str(density))
    seconds = time.perf_counter() - started
    return OptimizeResult(
        seconds=seconds,
        reclaimed_bytes=response if isinstance(response, int) else None,
    )


def benchmark_hset_update(
    args: argparse.Namespace,
    server: zbench.ServerProcess,
    command: Sequence[object],
    keyspace: int,
) -> float:
    rates = [
        zbench.redis_benchmark_rps(
            args.redis_benchmark,
            server.port,
            command,
            requests=args.requests,
            pipeline=args.pipeline,
            keyspace=keyspace,
            timeout=args.timeout,
        )
        for _ in range(args.rounds)
    ]
    return statistics.median(rates)


def verify_large_hash(
    client: zbench.RespClient,
    key: str,
    fields: int,
    value_bytes: int,
    marker: str,
) -> None:
    length = client.command("HLEN", key)
    if length != fields:
        raise RuntimeError(
            f"large hash has {length} fields, expected {fields}; "
            "redis-benchmark random-token formatting may not match the loader"
        )
    for item_id in (0, fields // 2, fields - 1):
        expected = fixed_value(item_id, value_bytes, marker).encode()
        actual = client.command("HGET", key, large_field(item_id))
        if actual != expected:
            raise RuntimeError(
                f"HGET correctness failure at field {item_id}: "
                f"expected {expected!r}, got {actual!r}"
            )


def run_large_hash(
    spec: EngineSpec, args: argparse.Namespace
) -> LargeHashResult:
    server = start_engine(spec)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            key = f"hsetbench:large:{os.getpid()}:{spec.label}"
            time.sleep(args.settle_seconds)
            baseline = memory_sample(client, spec)

            load_commands, load_seconds = zbench.time_pipeline(
                client,
                large_hset_commands(
                    args.large_fields,
                    key,
                    args.hset_batch,
                    args.value_bytes,
                    "v",
                ),
                args.pipeline,
            )
            verify_large_hash(
                client, key, args.large_fields, args.value_bytes, "v"
            )
            time.sleep(args.settle_seconds)
            loaded_before = memory_sample(client, spec, [key])

            initial_optimize = optimize_hash(
                client, spec, key, args.optimize_density
            )
            time.sleep(args.settle_seconds)
            loaded_after = memory_sample(client, spec, [key])

            update_rate = benchmark_hset_update(
                args,
                server,
                [
                    "HSET",
                    key,
                    "field:__rand_int__",
                    "u" * args.value_bytes,
                ],
                args.large_fields,
            )
            if client.command("HLEN", key) != args.large_fields:
                raise RuntimeError(
                    "random existing-field HSET created fields; expected "
                    f"{RANDOM_TOKEN_DIGITS}-digit __rand_int__ substitution"
                )
            time.sleep(args.settle_seconds)
            after_same_width = memory_sample(client, spec, [key])

            grow_commands, grow_seconds = zbench.time_pipeline(
                client,
                large_hset_commands(
                    args.large_fields,
                    key,
                    args.hset_batch,
                    args.grown_value_bytes,
                    "g",
                ),
                args.pipeline,
            )
            verify_large_hash(
                client, key, args.large_fields, args.grown_value_bytes, "g"
            )
            time.sleep(args.settle_seconds)
            grown_before = memory_sample(client, spec, [key])

            grown_optimize = optimize_hash(
                client, spec, key, args.optimize_density
            )
            time.sleep(args.settle_seconds)
            grown_after = memory_sample(client, spec, [key])

            return LargeHashResult(
                engine=spec.label,
                kind=spec.kind,
                binary=str(spec.binary),
                fields=args.large_fields,
                field_bytes=len(large_field(0)),
                initial_value_bytes=args.value_bytes,
                grown_value_bytes=args.grown_value_bytes,
                load_commands=load_commands,
                load_seconds=load_seconds,
                load_fields_per_second=args.large_fields / load_seconds,
                update_hset_operations_per_second=update_rate,
                grow_commands=grow_commands,
                grow_seconds=grow_seconds,
                grow_fields_per_second=args.large_fields / grow_seconds,
                baseline=baseline,
                loaded_before_optimize=loaded_before,
                loaded_after_optimize=loaded_after,
                after_same_width_updates=after_same_width,
                grown_before_optimize=grown_before,
                grown_after_optimize=grown_after,
                initial_optimize=initial_optimize,
                grown_optimize=grown_optimize,
            )
        finally:
            client.close()
    finally:
        server.stop()


def verify_small_hashes(
    client: zbench.RespClient,
    hashes: int,
    fields_per_hash: int,
    value_bytes: int,
) -> None:
    for hash_id in (0, hashes // 2, hashes - 1):
        key = small_key(hash_id)
        length = client.command("HLEN", key)
        if length != fields_per_hash:
            raise RuntimeError(
                f"{key} has {length} fields, expected {fields_per_hash}"
            )
        ordinal = hash_id * fields_per_hash + fields_per_hash - 1
        expected = fixed_value(ordinal, value_bytes, "v").encode()
        actual = client.command("HGET", key, small_field(fields_per_hash - 1))
        if actual != expected:
            raise RuntimeError(
                f"small-hash HGET failure for {key}: "
                f"expected {expected!r}, got {actual!r}"
            )


def run_small_hashes(
    spec: EngineSpec,
    args: argparse.Namespace,
    fields_per_hash: int,
) -> SmallHashResult:
    hashes = max(1, args.small_total_fields // fields_per_hash)
    total_fields = hashes * fields_per_hash
    server = start_engine(spec)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            time.sleep(args.settle_seconds)
            baseline = memory_sample(client, spec)
            load_commands, load_seconds = zbench.time_pipeline(
                client,
                small_hset_commands(hashes, fields_per_hash, args.value_bytes),
                args.pipeline,
            )
            verify_small_hashes(
                client, hashes, fields_per_hash, args.value_bytes
            )
            sample_keys = [small_key(0), small_key(hashes // 2), small_key(hashes - 1)]
            time.sleep(args.settle_seconds)
            loaded = memory_sample(client, spec, sample_keys)

            middle_field = small_field(fields_per_hash // 2)
            update_rate = benchmark_hset_update(
                args,
                server,
                [
                    "HSET",
                    "h:__rand_int__",
                    middle_field,
                    "u" * args.value_bytes,
                ],
                hashes,
            )
            verify_small_hashes(
                client, hashes, fields_per_hash, args.value_bytes
            )
            time.sleep(args.settle_seconds)
            after_updates = memory_sample(client, spec, sample_keys)

            return SmallHashResult(
                engine=spec.label,
                kind=spec.kind,
                binary=str(spec.binary),
                hashes=hashes,
                fields_per_hash=fields_per_hash,
                total_fields=total_fields,
                key_bytes=len(small_key(0)),
                field_bytes=len(small_field(0)),
                value_bytes=args.value_bytes,
                load_commands=load_commands,
                load_seconds=load_seconds,
                load_hashes_per_second=hashes / load_seconds,
                load_fields_per_second=total_fields / load_seconds,
                update_hset_operations_per_second=update_rate,
                baseline=baseline,
                loaded=loaded,
                after_updates=after_updates,
            )
        finally:
            client.close()
    finally:
        server.stop()


def delta(after: int | None, before: int | None) -> int | None:
    if after is None or before is None:
        return None
    return after - before


def per_unit(value: int | float | None, count: int) -> float | None:
    return None if value is None else float(value) / count


def fmt(value: int | float | None, digits: int = 2) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "n/a"
    return f"{float(value):,.{digits}f}"


def goblin_stat(sample: MemorySample, name: str) -> int | None:
    if sample.goblin_memory is None:
        return None
    value = sample.goblin_memory.get(name)
    return value if isinstance(value, int) else None


def report_lines(
    args: argparse.Namespace,
    large: Sequence[LargeHashResult],
    small: Sequence[SmallHashResult],
) -> list[str]:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [
        "# Goblin Core HSET Speed and Memory Benchmark",
        "",
        f"Generated on `{socket.gethostname()}` at {generated_at}.",
        "",
        "## Method",
        "",
        "- Every scenario starts a fresh server; engines run one at a time.",
        "- RSS is `INFO memory`'s `used_memory_rss` before and after the workload. "
        "The naamah builds read the same live `/proc` resident-set field and do "
        "not use a cached value.",
        "- Application memory is the independent `INFO used_memory` delta. "
        "Per-key memory is `MEMORY USAGE` on incumbents and "
        "`GOBLIN.MEMORY total_allocated_bytes` on Goblin Core. These are "
        "engine-specific corroborating counters; RSS is the cross-engine "
        "comparison.",
        f"- Bulk loads use multi-field `HSET` batches of `{args.hset_batch}` and "
        f"pipeline depth `{args.pipeline}` through the shared Python RESP "
        "harness; load timing includes client encoding and is reported as "
        "observed end-to-end throughput. Point-update rates use one "
        f"`redis-benchmark` client at pipeline depth `{args.pipeline}`, "
        f"`{args.requests:,}` requests, median of `{args.rounds}` runs.",
        "- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one "
        "proactor thread for single-core parity. The target is a modest single-core "
        "memory server; naamah is a quiet 64-core test machine.",
        "- Goblin uses the tested binary's configured compact-hash policy. The "
        "parity config keeps Redis and Valkey listpacked through 128 fields. "
        "The many-hash sweep therefore measures the engines' configured "
        "representation choices, not matched internal encodings.",
        "- Incumbents are exercised strictly as black-box RESP servers; their "
        "source code is not inspected.",
        "",
    ]

    if large:
        lines += [
            "## One Large Hash",
            "",
            f"`{args.large_fields:,}` distinct `{len(large_field(0))}`-byte fields "
            f"start with `{args.value_bytes}`-byte values. Existing-field HSET "
            f"uses the load generator's `{RANDOM_TOKEN_DIGITS}`-digit random token. "
            f"A deterministic pass then grows every value to "
            f"`{args.grown_value_bytes}` bytes.",
            "",
            "### Speed",
            "",
            "| Engine | bulk new fields/s | same-width HSET ops/s | "
            "bulk grow fields/s | initial optimize (s) | grown optimize (s) |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        for result in large:
            lines.append(
                f"| `{result.engine}` | {result.load_fields_per_second:,.0f} | "
                f"{result.update_hset_operations_per_second:,.0f} | "
                f"{result.grow_fields_per_second:,.0f} | "
                f"{fmt(result.initial_optimize.seconds, 4) if result.kind == 'goblin' else 'n/a'} | "
                f"{fmt(result.grown_optimize.seconds, 4) if result.kind == 'goblin' else 'n/a'} |"
            )

        lines += [
            "",
            f"### Memory With {args.value_bytes}-Byte Values",
            "",
            "Goblin's row is sampled after `GOBLIN.OPTIMIZE`; the pre-optimize "
            "RSS is retained in the raw JSON.",
            "",
            "| Engine | RSS MiB | RSS delta MiB | RSS B/field | used B/field | "
            "key B/field |",
            "| --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        for result in large:
            sample = result.loaded_after_optimize
            rss_delta = sample.rss_bytes - result.baseline.rss_bytes
            used_delta = delta(
                sample.used_memory_bytes, result.baseline.used_memory_bytes
            )
            lines.append(
                f"| `{result.engine}` | {sample.rss_bytes / 2**20:.2f} | "
                f"{rss_delta / 2**20:.2f} | "
                f"{rss_delta / result.fields:.2f} | "
                f"{fmt(per_unit(used_delta, result.fields))} | "
                f"{fmt(per_unit(sample.key_memory_bytes, result.fields))} |"
            )

        lines += [
            "",
            f"### Memory After Growing Every Value to {args.grown_value_bytes} Bytes",
            "",
            "Pre-optimize captures update fragmentation. Post-optimize is the "
            "equal-data steady state; non-Goblin engines perform no intervening "
            "maintenance operation.",
            "",
            "| Engine | pre-opt RSS B/field | post-opt RSS B/field | "
            "post-opt used B/field | post-opt key B/field |",
            "| --- | ---: | ---: | ---: | ---: |",
        ]
        for result in large:
            before = result.grown_before_optimize
            after = result.grown_after_optimize
            before_rss = before.rss_bytes - result.baseline.rss_bytes
            after_rss = after.rss_bytes - result.baseline.rss_bytes
            after_used = delta(
                after.used_memory_bytes, result.baseline.used_memory_bytes
            )
            lines.append(
                f"| `{result.engine}` | {before_rss / result.fields:.2f} | "
                f"{after_rss / result.fields:.2f} | "
                f"{fmt(per_unit(after_used, result.fields))} | "
                f"{fmt(per_unit(after.key_memory_bytes, result.fields))} |"
            )

        goblin = next((row for row in large if row.kind == "goblin"), None)
        if goblin is not None:
            lines += [
                "",
                "### Goblin Large-Hash Internals",
                "",
                "| Phase | fields | live MiB | dead MiB | arena MiB | "
                "index MiB | total MiB |",
                "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
            for phase, sample in (
                ("loaded before optimize", goblin.loaded_before_optimize),
                ("loaded after optimize", goblin.loaded_after_optimize),
                ("grown before optimize", goblin.grown_before_optimize),
                ("grown after optimize", goblin.grown_after_optimize),
            ):
                def mib(name: str) -> str:
                    value = goblin_stat(sample, name)
                    return fmt(None if value is None else value / 2**20)

                lines.append(
                    f"| `{phase}` | "
                    f"{goblin_stat(sample, 'field_count') or 0:,} | "
                    f"{mib('field_value_live_bytes')} | "
                    f"{mib('field_value_dead_bytes')} | "
                    f"{mib('field_value_allocated_bytes')} | "
                    f"{mib('field_index_allocated_bytes')} | "
                    f"{mib('total_allocated_bytes')} |"
                )

    if small:
        lines += [
            "",
            "## Many Hashes",
            "",
            f"Each row holds approximately `{args.small_total_fields:,}` total "
            f"fields while varying fields per hash. Keys are "
            f"`{len(small_key(0))}` bytes, fields are "
            f"`{len(small_field(0))}` bytes, and values are "
            f"`{args.value_bytes}` bytes. The update targets the middle field of "
            "random existing hashes.",
            "",
            "### Speed",
            "",
            "| fields/hash | hashes | Engine | load fields/s | load hashes/s | "
            "middle-field HSET ops/s |",
            "| ---: | ---: | --- | ---: | ---: | ---: |",
        ]
        for result in small:
            lines.append(
                f"| {result.fields_per_hash} | {result.hashes:,} | "
                f"`{result.engine}` | {result.load_fields_per_second:,.0f} | "
                f"{result.load_hashes_per_second:,.0f} | "
                f"{result.update_hset_operations_per_second:,.0f} |"
            )

        lines += [
            "",
            "### Memory",
            "",
            "Sampled key bytes are the mean of the first, middle, and last hash.",
            "",
            "| fields/hash | Engine | RSS MiB | RSS B/hash | RSS B/field | "
            "used B/hash | sampled key B/hash |",
            "| ---: | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        for result in small:
            rss_delta = result.loaded.rss_bytes - result.baseline.rss_bytes
            used_delta = delta(
                result.loaded.used_memory_bytes,
                result.baseline.used_memory_bytes,
            )
            lines.append(
                f"| {result.fields_per_hash} | `{result.engine}` | "
                f"{result.loaded.rss_bytes / 2**20:.2f} | "
                f"{rss_delta / result.hashes:.2f} | "
                f"{rss_delta / result.total_fields:.2f} | "
                f"{fmt(per_unit(used_delta, result.hashes))} | "
                f"{fmt(result.loaded.key_memory_bytes)} |"
            )

    lines += ["", "## Binaries", ""]
    for spec in args.engine:
        lines.append(f"- `{spec.label}`: `{spec.binary}`")
    lines.append("")
    return lines


def write_outputs(
    args: argparse.Namespace,
    large: Sequence[LargeHashResult],
    small: Sequence[SmallHashResult],
) -> None:
    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "host": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "logical_cpus": os.cpu_count(),
        },
        "config": {
            "large_fields": args.large_fields,
            "small_total_fields": args.small_total_fields,
            "small_shapes": args.small_shapes,
            "value_bytes": args.value_bytes,
            "grown_value_bytes": args.grown_value_bytes,
            "hset_batch": args.hset_batch,
            "pipeline": args.pipeline,
            "requests": args.requests,
            "rounds": args.rounds,
            "optimize_density": args.optimize_density,
            "settle_seconds": args.settle_seconds,
            "redis_benchmark": str(args.redis_benchmark),
        },
        "large_hash": [asdict(result) for result in large],
        "small_hashes": [asdict(result) for result in small],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")
    args.report.write_text("\n".join(report_lines(args, large, small)))


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
    parser.add_argument("--redis-benchmark", required=True, type=Path)
    parser.add_argument("--large-fields", type=int, default=1_000_000)
    parser.add_argument("--small-total-fields", type=int, default=500_000)
    parser.add_argument(
        "--small-shapes",
        default="8,32,64,128",
        help="comma-separated fields-per-hash values",
    )
    parser.add_argument("--value-bytes", type=int, default=16)
    parser.add_argument("--grown-value-bytes", type=int, default=64)
    parser.add_argument("--hset-batch", type=int, default=128)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--requests", type=int, default=500_000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--optimize-density", type=float, default=0.97)
    parser.add_argument("--settle-seconds", type=float, default=0.5)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--skip-large", action="store_true")
    parser.add_argument("--skip-small", action="store_true")
    parser.add_argument(
        "--output-json",
        type=Path,
        default=ROOT / "benchmark-results" / "hset.json",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=ROOT / "benchmark-results" / "hset.md",
    )
    args = parser.parse_args(argv)

    try:
        args.small_shapes = [
            int(value) for value in args.small_shapes.split(",") if value
        ]
    except ValueError as error:
        parser.error(f"--small-shapes must contain integers: {error}")
    if args.skip_large and args.skip_small:
        parser.error("--skip-large and --skip-small cannot both be set")
    if min(
        args.large_fields,
        args.small_total_fields,
        args.value_bytes,
        args.grown_value_bytes,
        args.hset_batch,
        args.pipeline,
        args.requests,
        args.rounds,
    ) <= 0 or not args.small_shapes or min(args.small_shapes) <= 0:
        parser.error("all counts, sizes, shapes, pipeline depths, and rounds must be positive")
    if args.grown_value_bytes <= args.value_bytes:
        parser.error("--grown-value-bytes must exceed --value-bytes")
    if args.value_bytes <= 1 or args.grown_value_bytes <= 1:
        parser.error("value sizes must leave room for a marker and payload")
    if args.value_bytes > 65535 or args.grown_value_bytes > 65535:
        parser.error("hash values cannot exceed 65,535 bytes")
    if args.large_fields > 10**RANDOM_TOKEN_DIGITS:
        parser.error(
            f"--large-fields cannot exceed the {RANDOM_TOKEN_DIGITS}-digit "
            "redis-benchmark random-token space"
        )
    largest_hash_count = args.small_total_fields // min(args.small_shapes)
    if largest_hash_count > 10**RANDOM_TOKEN_DIGITS:
        parser.error(
            f"the smallest --small-shapes value creates more than "
            f"10^{RANDOM_TOKEN_DIGITS} hashes"
        )
    if max(args.small_shapes) > 1000:
        parser.error(
            "--small-shapes values cannot exceed 1,000 fixed-width fields"
        )
    max_ordinal = max(args.large_fields, args.small_total_fields) - 1
    if len(f"{max_ordinal:x}") > args.value_bytes - 1:
        parser.error("--value-bytes is too small for distinct fixed-width values")
    if not (0.0 < args.optimize_density <= 1.0):
        parser.error("--optimize-density must be in (0, 1]")
    if args.settle_seconds < 0 or args.timeout <= 0:
        parser.error("settle time must be non-negative and timeout positive")

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
    large: list[LargeHashResult] = []
    small: list[SmallHashResult] = []

    if not args.skip_large:
        for spec in args.engine:
            print(f"benchmarking large hash on {spec.label}...", file=sys.stderr, flush=True)
            result = run_large_hash(spec, args)
            large.append(result)
            rss_delta = (
                result.loaded_after_optimize.rss_bytes - result.baseline.rss_bytes
            )
            print(
                f"  load {result.load_fields_per_second:,.0f} fields/s, "
                f"update {result.update_hset_operations_per_second:,.0f} ops/s, "
                f"RSS {rss_delta / result.fields:.1f} B/field",
                file=sys.stderr,
                flush=True,
            )

    if not args.skip_small:
        for fields_per_hash in args.small_shapes:
            for spec in args.engine:
                print(
                    f"benchmarking {fields_per_hash} fields/hash on {spec.label}...",
                    file=sys.stderr,
                    flush=True,
                )
                result = run_small_hashes(spec, args, fields_per_hash)
                small.append(result)
                rss_delta = result.loaded.rss_bytes - result.baseline.rss_bytes
                print(
                    f"  load {result.load_fields_per_second:,.0f} fields/s, "
                    f"update {result.update_hset_operations_per_second:,.0f} ops/s, "
                    f"RSS {rss_delta / result.hashes:.1f} B/hash",
                    file=sys.stderr,
                    flush=True,
                )

    write_outputs(args, large, small)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main(sys.argv[1:]))
