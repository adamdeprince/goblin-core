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

Two focused workloads cover update behavior:

* Goblin full hashes: random and clustered value relocation at 0%, 0.1%, 1%,
  10%, 50%, and 100%, including first-growth latency, HGET, memory, and compact;
* mixed hashes: seeded inserts, same-width updates, deletes, and value growth,
  measured as depth-one RESP latency through p99.9.

Example:
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
import random
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
    construction_rounds: int = 1


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
    construction_rounds: int = 1


@dataclass
class RelocationResult:
    engine: str
    binary: str
    pattern: str
    density: float
    fields: int
    grown_fields: int
    load_seconds: float
    load_fields_per_second: float
    first_growth_latency_us: float | None
    remaining_growth_seconds: float
    hget_operations_per_second: float
    baseline: MemorySample
    loaded: MemorySample
    grown_before_optimize: MemorySample
    grown_after_optimize: MemorySample
    optimize: OptimizeResult
    rounds: int = 1


@dataclass
class MixedHashResult:
    engine: str
    kind: str
    binary: str
    initial_fields: int
    samples: int
    warmup: int
    operations: dict[str, int]
    operation_latency_us: dict[str, dict[str, float]]
    load_seconds: float
    operations_per_second: float
    latency_min_us: float
    latency_p50_us: float
    latency_p95_us: float
    latency_p99_us: float
    latency_p999_us: float
    latency_max_us: float
    baseline: MemorySample
    loaded: MemorySample
    after_mixed: MemorySample
    final_fields: int
    rounds: int = 1


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


def start_engine(
    spec: EngineSpec, extra_goblin_args: Sequence[str] = ()
) -> zbench.ServerProcess:
    if spec.kind == "goblin":
        return zbench.start_goblin(
            spec.binary,
            rank_cache=False,
            rank_cache_mode="off",
            extra_args=extra_goblin_args,
        )
    if spec.kind == "redis":
        return zbench.start_redis(spec.binary)
    return zbench.start_dragonfly(spec.binary)


def warm_baseline(
    client: zbench.RespClient, spec: EngineSpec, args: argparse.Namespace
) -> MemorySample:
    """Warm the hash allocator/index path, settle, then sample an empty server."""
    for _ in range(args.baseline_warmup_commands):
        response = client.command("PING")
        if response not in ("PONG", b"PONG"):
            raise RuntimeError(f"{spec.label} returned an invalid PING response")
    warm_key = f"hsetbench:baseline-warm:{os.getpid()}:{spec.label}"
    zbench.time_pipeline(
        client,
        large_hset_commands(
            args.baseline_warmup_fields,
            warm_key,
            args.hset_batch,
            args.value_bytes,
            "w",
        ),
        args.pipeline,
    )
    if client.command("HLEN", warm_key) != args.baseline_warmup_fields:
        raise RuntimeError(f"{spec.label} baseline hash warmup was incomplete")
    if client.command("DEL", warm_key) != 1:
        raise RuntimeError(f"{spec.label} baseline hash warmup cleanup failed")
    # Touch INFO before the measured call so first-response encoding is also
    # outside the baseline sample.
    zbench.redis_info_fields(client, "memory")
    time.sleep(args.settle_seconds)
    return memory_sample(client, spec)


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
    if args.warmup_requests:
        zbench.redis_benchmark_rps(
            args.redis_benchmark,
            server.port,
            command,
            requests=args.warmup_requests,
            pipeline=args.pipeline,
            keyspace=keyspace,
            timeout=args.timeout,
        )
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


def benchmark_hget(
    args: argparse.Namespace,
    server: zbench.ServerProcess,
    key: str,
    keyspace: int,
) -> float:
    command = ["HGET", key, "field:__rand_int__"]
    if args.warmup_requests:
        zbench.redis_benchmark_rps(
            args.redis_benchmark,
            server.port,
            command,
            requests=args.warmup_requests,
            pipeline=args.pipeline,
            keyspace=keyspace,
            timeout=args.timeout,
        )
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


def median_optional_int(values: Sequence[int | None]) -> int | None:
    present = [value for value in values if value is not None]
    return None if not present else int(round(statistics.median(present)))


def median_optional_float(values: Sequence[float | None]) -> float | None:
    present = [value for value in values if value is not None]
    return None if not present else float(statistics.median(present))


def median_memory(samples: Sequence[MemorySample]) -> MemorySample:
    keys = {
        key
        for sample in samples
        for key in (sample.goblin_memory or {}).keys()
    }
    goblin_memory: dict[str, int | str] | None = None
    if keys:
        goblin_memory = {}
        for key in sorted(keys):
            values = [
                sample.goblin_memory[key]
                for sample in samples
                if sample.goblin_memory is not None
                and key in sample.goblin_memory
            ]
            integers = [value for value in values if isinstance(value, int)]
            if len(integers) == len(values):
                goblin_memory[key] = int(round(statistics.median(integers)))
            elif values:
                goblin_memory[key] = values[0]
    return MemorySample(
        rss_bytes=int(round(statistics.median(s.rss_bytes for s in samples))),
        used_memory_bytes=median_optional_int(
            [s.used_memory_bytes for s in samples]
        ),
        key_memory_bytes=median_optional_float(
            [s.key_memory_bytes for s in samples]
        ),
        goblin_memory=goblin_memory,
    )


def median_memory_checkpoint(
    samples: Sequence[MemorySample],
    baselines: Sequence[MemorySample],
    median_baseline: MemorySample,
) -> MemorySample:
    """Aggregate a checkpoint while preserving paired per-run deltas."""
    if len(samples) != len(baselines):
        raise ValueError("memory checkpoints and baselines must be paired")
    absolute = median_memory(samples)
    rss_delta = int(
        round(
            statistics.median(
                sample.rss_bytes - baseline.rss_bytes
                for sample, baseline in zip(samples, baselines)
            )
        )
    )
    used_deltas = [
        sample.used_memory_bytes - baseline.used_memory_bytes
        for sample, baseline in zip(samples, baselines)
        if sample.used_memory_bytes is not None
        and baseline.used_memory_bytes is not None
    ]
    used_memory = absolute.used_memory_bytes
    if median_baseline.used_memory_bytes is not None and used_deltas:
        used_memory = median_baseline.used_memory_bytes + int(
            round(statistics.median(used_deltas))
        )
    return MemorySample(
        rss_bytes=median_baseline.rss_bytes + rss_delta,
        used_memory_bytes=used_memory,
        key_memory_bytes=absolute.key_memory_bytes,
        goblin_memory=absolute.goblin_memory,
    )


def median_optimize(results: Sequence[OptimizeResult]) -> OptimizeResult:
    return OptimizeResult(
        seconds=float(statistics.median(result.seconds for result in results)),
        reclaimed_bytes=median_optional_int(
            [result.reclaimed_bytes for result in results]
        ),
    )


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


def run_large_hash_once(
    spec: EngineSpec, args: argparse.Namespace
) -> LargeHashResult:
    server = start_engine(spec)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            key = f"hsetbench:large:{os.getpid()}:{spec.label}"
            baseline = warm_baseline(client, spec, args)

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


def median_large_hash(results: Sequence[LargeHashResult]) -> LargeHashResult:
    first = results[0]
    baselines = [r.baseline for r in results]
    baseline = median_memory(baselines)
    return LargeHashResult(
        engine=first.engine,
        kind=first.kind,
        binary=first.binary,
        fields=first.fields,
        field_bytes=first.field_bytes,
        initial_value_bytes=first.initial_value_bytes,
        grown_value_bytes=first.grown_value_bytes,
        load_commands=int(round(statistics.median(r.load_commands for r in results))),
        load_seconds=float(statistics.median(r.load_seconds for r in results)),
        load_fields_per_second=float(
            statistics.median(r.load_fields_per_second for r in results)
        ),
        update_hset_operations_per_second=float(
            statistics.median(
                r.update_hset_operations_per_second for r in results
            )
        ),
        grow_commands=int(round(statistics.median(r.grow_commands for r in results))),
        grow_seconds=float(statistics.median(r.grow_seconds for r in results)),
        grow_fields_per_second=float(
            statistics.median(r.grow_fields_per_second for r in results)
        ),
        baseline=baseline,
        loaded_before_optimize=median_memory_checkpoint(
            [r.loaded_before_optimize for r in results], baselines, baseline
        ),
        loaded_after_optimize=median_memory_checkpoint(
            [r.loaded_after_optimize for r in results], baselines, baseline
        ),
        after_same_width_updates=median_memory_checkpoint(
            [r.after_same_width_updates for r in results], baselines, baseline
        ),
        grown_before_optimize=median_memory_checkpoint(
            [r.grown_before_optimize for r in results], baselines, baseline
        ),
        grown_after_optimize=median_memory_checkpoint(
            [r.grown_after_optimize for r in results], baselines, baseline
        ),
        initial_optimize=median_optimize([r.initial_optimize for r in results]),
        grown_optimize=median_optimize([r.grown_optimize for r in results]),
        construction_rounds=len(results),
    )


def run_large_hash(
    spec: EngineSpec, args: argparse.Namespace
) -> LargeHashResult:
    return median_large_hash(
        [run_large_hash_once(spec, args) for _ in range(args.construction_rounds)]
    )


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


def run_small_hashes_once(
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
            baseline = warm_baseline(client, spec, args)
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


def median_small_hash(results: Sequence[SmallHashResult]) -> SmallHashResult:
    first = results[0]
    baselines = [r.baseline for r in results]
    baseline = median_memory(baselines)
    return SmallHashResult(
        engine=first.engine,
        kind=first.kind,
        binary=first.binary,
        hashes=first.hashes,
        fields_per_hash=first.fields_per_hash,
        total_fields=first.total_fields,
        key_bytes=first.key_bytes,
        field_bytes=first.field_bytes,
        value_bytes=first.value_bytes,
        load_commands=int(round(statistics.median(r.load_commands for r in results))),
        load_seconds=float(statistics.median(r.load_seconds for r in results)),
        load_hashes_per_second=float(
            statistics.median(r.load_hashes_per_second for r in results)
        ),
        load_fields_per_second=float(
            statistics.median(r.load_fields_per_second for r in results)
        ),
        update_hset_operations_per_second=float(
            statistics.median(
                r.update_hset_operations_per_second for r in results
            )
        ),
        baseline=baseline,
        loaded=median_memory_checkpoint(
            [r.loaded for r in results], baselines, baseline
        ),
        after_updates=median_memory_checkpoint(
            [r.after_updates for r in results], baselines, baseline
        ),
        construction_rounds=len(results),
    )


def run_small_hashes(
    spec: EngineSpec,
    args: argparse.Namespace,
    fields_per_hash: int,
) -> SmallHashResult:
    return median_small_hash(
        [
            run_small_hashes_once(spec, args, fields_per_hash)
            for _ in range(args.construction_rounds)
        ]
    )


def relocation_field_ids(
    fields: int, count: int, pattern: str, seed: int
) -> list[int]:
    if count == 0:
        return []
    if pattern == "clustered":
        start = (fields - count) // 2
        return list(range(start, start + count))
    ids = random.Random(seed).sample(range(fields), count)
    return ids


def verify_relocation_values(
    client: zbench.RespClient,
    key: str,
    fields: int,
    grown_ids: Sequence[int],
    initial_value_bytes: int,
    grown_value_bytes: int,
) -> None:
    if client.command("HLEN", key) != fields:
        raise RuntimeError("relocation workload changed the hash field count")
    selected = set(grown_ids)
    selected_samples = (
        []
        if not grown_ids
        else [grown_ids[0], grown_ids[len(grown_ids) // 2], grown_ids[-1]]
    )
    for item_id in dict.fromkeys(selected_samples):
        expected = fixed_value(item_id, grown_value_bytes, "g").encode()
        actual = client.command("HGET", key, large_field(item_id))
        if actual != expected:
            raise RuntimeError(
                f"relocated value {item_id} mismatch: {actual!r} != {expected!r}"
            )

    unselected_samples: list[int] = []
    if len(selected) < fields:
        candidates = [0, fields // 4, fields // 2, 3 * fields // 4, fields - 1]
        for candidate in candidates:
            if candidate not in selected and candidate not in unselected_samples:
                unselected_samples.append(candidate)
            if len(unselected_samples) == 3:
                break
        candidate = 0
        while len(unselected_samples) < 3 and candidate < fields:
            if candidate not in selected and candidate not in unselected_samples:
                unselected_samples.append(candidate)
            candidate += 1
    for item_id in unselected_samples:
        expected = fixed_value(item_id, initial_value_bytes, "v").encode()
        actual = client.command("HGET", key, large_field(item_id))
        if actual != expected:
            raise RuntimeError(
                f"unrelocated value {item_id} mismatch: {actual!r} != {expected!r}"
            )


def run_relocation_once(
    spec: EngineSpec,
    args: argparse.Namespace,
    pattern: str,
    density: float,
    round_index: int,
) -> RelocationResult:
    server = start_engine(spec, ["--hash-listpack-max-entries", "0"])
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            key = (
                f"hsetbench:relocation:{os.getpid()}:{pattern}:"
                f"{density:g}:{round_index}"
            )
            baseline = warm_baseline(client, spec, args)
            _, load_seconds = zbench.time_pipeline(
                client,
                large_hset_commands(
                    args.relocation_fields,
                    key,
                    args.hset_batch,
                    args.value_bytes,
                    "v",
                ),
                args.pipeline,
            )
            verify_large_hash(
                client, key, args.relocation_fields, args.value_bytes, "v"
            )
            time.sleep(args.settle_seconds)
            loaded = memory_sample(client, spec, [key])

            grown_fields = int(round(args.relocation_fields * density))
            grown_fields = min(args.relocation_fields, max(0, grown_fields))
            ids = relocation_field_ids(
                args.relocation_fields,
                grown_fields,
                pattern,
                args.seed + round_index,
            )
            first_growth_latency_us: float | None = None
            remaining_growth_seconds = 0.0
            if ids:
                item_id = ids[0]
                started = time.perf_counter_ns()
                response = client.command(
                    "HSET",
                    key,
                    large_field(item_id),
                    fixed_value(item_id, args.grown_value_bytes, "g"),
                )
                first_growth_latency_us = (
                    time.perf_counter_ns() - started
                ) / 1_000.0
                if response != 0:
                    raise RuntimeError("first relocation HSET did not update a field")

                def remaining_growth_commands() -> Iterable[list[object]]:
                    for grow_id in ids[1:]:
                        yield [
                            "HSET",
                            key,
                            large_field(grow_id),
                            fixed_value(grow_id, args.grown_value_bytes, "g"),
                        ]

                _, remaining_growth_seconds = zbench.time_pipeline(
                    client, remaining_growth_commands(), args.pipeline
                )

            verify_relocation_values(
                client,
                key,
                args.relocation_fields,
                ids,
                args.value_bytes,
                args.grown_value_bytes,
            )
            time.sleep(args.settle_seconds)
            grown_before = memory_sample(client, spec, [key])
            hget_rate = benchmark_hget(
                args, server, key, args.relocation_fields
            )
            optimize = optimize_hash(client, spec, key, args.optimize_density)
            verify_relocation_values(
                client,
                key,
                args.relocation_fields,
                ids,
                args.value_bytes,
                args.grown_value_bytes,
            )
            time.sleep(args.settle_seconds)
            grown_after = memory_sample(client, spec, [key])

            return RelocationResult(
                engine=spec.label,
                binary=str(spec.binary),
                pattern=pattern,
                density=density,
                fields=args.relocation_fields,
                grown_fields=grown_fields,
                load_seconds=load_seconds,
                load_fields_per_second=args.relocation_fields / load_seconds,
                first_growth_latency_us=first_growth_latency_us,
                remaining_growth_seconds=remaining_growth_seconds,
                hget_operations_per_second=hget_rate,
                baseline=baseline,
                loaded=loaded,
                grown_before_optimize=grown_before,
                grown_after_optimize=grown_after,
                optimize=optimize,
            )
        finally:
            client.close()
    finally:
        server.stop()


def median_relocation(results: Sequence[RelocationResult]) -> RelocationResult:
    first = results[0]
    baselines = [r.baseline for r in results]
    baseline = median_memory(baselines)
    return RelocationResult(
        engine=first.engine,
        binary=first.binary,
        pattern=first.pattern,
        density=first.density,
        fields=first.fields,
        grown_fields=first.grown_fields,
        load_seconds=float(statistics.median(r.load_seconds for r in results)),
        load_fields_per_second=float(
            statistics.median(r.load_fields_per_second for r in results)
        ),
        first_growth_latency_us=median_optional_float(
            [r.first_growth_latency_us for r in results]
        ),
        remaining_growth_seconds=float(
            statistics.median(r.remaining_growth_seconds for r in results)
        ),
        hget_operations_per_second=float(
            statistics.median(r.hget_operations_per_second for r in results)
        ),
        baseline=baseline,
        loaded=median_memory_checkpoint(
            [r.loaded for r in results], baselines, baseline
        ),
        grown_before_optimize=median_memory_checkpoint(
            [r.grown_before_optimize for r in results], baselines, baseline
        ),
        grown_after_optimize=median_memory_checkpoint(
            [r.grown_after_optimize for r in results], baselines, baseline
        ),
        optimize=median_optimize([r.optimize for r in results]),
        rounds=len(results),
    )


def run_relocation(
    spec: EngineSpec,
    args: argparse.Namespace,
    pattern: str,
    density: float,
) -> RelocationResult:
    return median_relocation(
        [
            run_relocation_once(spec, args, pattern, density, round_index)
            for round_index in range(args.relocation_rounds)
        ]
    )


def remove_dense(values: list[int], positions: dict[int, int], value: int) -> None:
    position = positions.pop(value)
    last = values.pop()
    if position < len(values):
        values[position] = last
        positions[last] = position


def append_dense(values: list[int], positions: dict[int, int], value: int) -> None:
    positions[value] = len(values)
    values.append(value)


def mixed_commands(
    args: argparse.Namespace,
    key: str,
    seed: int,
) -> tuple[list[tuple[str, list[object], int]], int]:
    """Build deterministic valid commands and their expected integer replies."""
    rng = random.Random(seed)
    live = list(range(args.mixed_fields))
    live_positions = {value: value for value in live}
    short = live.copy()
    short_positions = live_positions.copy()
    grown: set[int] = set()
    next_id = args.mixed_fields
    operations = ("insert", "update", "delete", "grow")
    weights = (
        args.mixed_insert_weight,
        args.mixed_update_weight,
        args.mixed_delete_weight,
        args.mixed_grow_weight,
    )
    commands: list[tuple[str, list[object], int]] = []
    for _ in range(args.mixed_warmup + args.mixed_samples):
        operation = rng.choices(operations, weights=weights, k=1)[0]
        if operation == "delete" and len(live) <= 1:
            operation = "insert"
        if operation == "grow" and not short:
            operation = "update"

        if operation == "insert":
            item_id = next_id
            next_id += 1
            append_dense(live, live_positions, item_id)
            append_dense(short, short_positions, item_id)
            command = [
                "HSET",
                key,
                large_field(item_id),
                fixed_value(item_id, args.value_bytes, "v"),
            ]
            expected = 1
        elif operation == "delete":
            item_id = live[rng.randrange(len(live))]
            remove_dense(live, live_positions, item_id)
            if item_id in short_positions:
                remove_dense(short, short_positions, item_id)
            grown.discard(item_id)
            command = ["HDEL", key, large_field(item_id)]
            expected = 1
        elif operation == "grow":
            item_id = short[rng.randrange(len(short))]
            remove_dense(short, short_positions, item_id)
            grown.add(item_id)
            command = [
                "HSET",
                key,
                large_field(item_id),
                fixed_value(item_id, args.grown_value_bytes, "g"),
            ]
            expected = 0
        else:
            item_id = live[rng.randrange(len(live))]
            width = args.grown_value_bytes if item_id in grown else args.value_bytes
            command = [
                "HSET",
                key,
                large_field(item_id),
                fixed_value(item_id, width, "u"),
            ]
            expected = 0
        commands.append((operation, command, expected))
    return commands, len(live)


def latency_stats_us(latencies: Sequence[float]) -> dict[str, float]:
    ordered = sorted(latencies)
    return {
        "min": ordered[0] * 1_000_000.0,
        "p50": zbench.percentile(ordered, 0.50) * 1_000_000.0,
        "p95": zbench.percentile(ordered, 0.95) * 1_000_000.0,
        "p99": zbench.percentile(ordered, 0.99) * 1_000_000.0,
        "p999": zbench.percentile(ordered, 0.999) * 1_000_000.0,
        "max": ordered[-1] * 1_000_000.0,
    }


def run_mixed_once(
    spec: EngineSpec, args: argparse.Namespace, round_index: int
) -> MixedHashResult:
    server = start_engine(spec)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            key = f"hsetbench:mixed:{os.getpid()}:{spec.label}:{round_index}"
            baseline = warm_baseline(client, spec, args)
            _, load_seconds = zbench.time_pipeline(
                client,
                large_hset_commands(
                    args.mixed_fields,
                    key,
                    args.hset_batch,
                    args.value_bytes,
                    "v",
                ),
                args.pipeline,
            )
            time.sleep(args.settle_seconds)
            loaded = memory_sample(client, spec, [key])

            commands, final_fields = mixed_commands(
                args, key, args.seed
            )
            for _, command, expected in commands[: args.mixed_warmup]:
                response = client.command(*command)
                if response != expected:
                    raise RuntimeError(
                        f"mixed warmup expected {expected}, got {response}"
                    )

            counts = {name: 0 for name in ("insert", "update", "delete", "grow")}
            operation_latencies = {name: [] for name in counts}
            latencies: list[float] = []
            started = time.perf_counter()
            for operation, command, expected in commands[args.mixed_warmup :]:
                command_started = time.perf_counter_ns()
                response = client.command(*command)
                latency = (
                    time.perf_counter_ns() - command_started
                ) / 1_000_000_000.0
                latencies.append(latency)
                operation_latencies[operation].append(latency)
                if response != expected:
                    raise RuntimeError(
                        f"mixed {operation} expected {expected}, got {response}"
                    )
                counts[operation] += 1
            seconds = time.perf_counter() - started
            if client.command("HLEN", key) != final_fields:
                raise RuntimeError("mixed workload final HLEN did not match model")
            time.sleep(args.settle_seconds)
            after_mixed = memory_sample(client, spec, [key])
            stats = latency_stats_us(latencies)
            return MixedHashResult(
                engine=spec.label,
                kind=spec.kind,
                binary=str(spec.binary),
                initial_fields=args.mixed_fields,
                samples=args.mixed_samples,
                warmup=args.mixed_warmup,
                operations=counts,
                operation_latency_us={
                    name: latency_stats_us(samples)
                    for name, samples in operation_latencies.items()
                    if samples
                },
                load_seconds=load_seconds,
                operations_per_second=args.mixed_samples / seconds,
                latency_min_us=stats["min"],
                latency_p50_us=stats["p50"],
                latency_p95_us=stats["p95"],
                latency_p99_us=stats["p99"],
                latency_p999_us=stats["p999"],
                latency_max_us=stats["max"],
                baseline=baseline,
                loaded=loaded,
                after_mixed=after_mixed,
                final_fields=final_fields,
            )
        finally:
            client.close()
    finally:
        server.stop()


def median_mixed(results: Sequence[MixedHashResult]) -> MixedHashResult:
    first = results[0]
    if any(
        result.operations != first.operations or
        result.final_fields != first.final_fields
        for result in results[1:]
    ):
        raise RuntimeError("mixed rounds must use the same deterministic workload")
    baselines = [r.baseline for r in results]
    baseline = median_memory(baselines)
    return MixedHashResult(
        engine=first.engine,
        kind=first.kind,
        binary=first.binary,
        initial_fields=first.initial_fields,
        samples=first.samples,
        warmup=first.warmup,
        operations=dict(first.operations),
        operation_latency_us={
            name: {
                percentile: float(
                    statistics.median(
                        r.operation_latency_us[name][percentile] for r in results
                    )
                )
                for percentile in first.operation_latency_us[name]
            }
            for name in first.operation_latency_us
        },
        load_seconds=float(statistics.median(r.load_seconds for r in results)),
        operations_per_second=float(
            statistics.median(r.operations_per_second for r in results)
        ),
        latency_min_us=float(statistics.median(r.latency_min_us for r in results)),
        latency_p50_us=float(statistics.median(r.latency_p50_us for r in results)),
        latency_p95_us=float(statistics.median(r.latency_p95_us for r in results)),
        latency_p99_us=float(statistics.median(r.latency_p99_us for r in results)),
        latency_p999_us=float(
            statistics.median(r.latency_p999_us for r in results)
        ),
        latency_max_us=float(statistics.median(r.latency_max_us for r in results)),
        baseline=baseline,
        loaded=median_memory_checkpoint(
            [r.loaded for r in results], baselines, baseline
        ),
        after_mixed=median_memory_checkpoint(
            [r.after_mixed for r in results], baselines, baseline
        ),
        final_fields=first.final_fields,
        rounds=len(results),
    )


def run_mixed(spec: EngineSpec, args: argparse.Namespace) -> MixedHashResult:
    return median_mixed(
        [run_mixed_once(spec, args, index) for index in range(args.mixed_rounds)]
    )


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
    relocation: Sequence[RelocationResult],
    mixed: Sequence[MixedHashResult],
) -> list[str]:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [
        "# Goblin Core HSET Speed and Memory Benchmark",
        "",
        f"Generated on a dedicated benchmark host at {generated_at}.",
        "",
        "## Method",
        "",
        "- Every scenario starts a fresh server; engines run one at a time.",
        f"- Before the empty-server baseline, every engine constructs and "
        f"deletes a `{args.baseline_warmup_fields:,}`-field fixed-width hash, "
        "then settles. This warms the hash arena, index, and allocator.",
        "- RSS is `INFO memory`'s `used_memory_rss` before and after the workload. "
        "The builds read the same live `/proc` resident-set field and do "
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
        f"`redis-benchmark` client at pipeline depth `{args.pipeline}` and "
        f"`{args.requests:,}` requests. Each fresh-server round takes a median "
        f"of `{args.rounds}` point runs; the large/many tables then median "
        f"`{args.construction_rounds}` fresh rounds "
        f"(`{args.rounds * args.construction_rounds}` point samples), while "
        f"relocation HGET medians `{args.relocation_rounds}` fresh rounds "
        f"(`{args.rounds * args.relocation_rounds}` point samples).",
        f"- Baselines are component-wise medians of each scenario's "
        f"fresh-server rounds (`{args.construction_rounds}` for "
        f"construction, `{args.mixed_rounds}` for mixed, and "
        f"`{args.relocation_rounds}` for relocation). RSS and "
        f"`used_memory` deltas are paired within each round and then medianed; "
        f"absolute checkpoint values are the median baseline plus that paired "
        f"median delta.",
        "- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one "
        "proactor thread for single-core parity. The target is a modest single-core "
        "memory server; the benchmark host is a quiet 64-core test machine.",
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

    if relocation:
        lines += [
            "",
            "## Goblin Full-Hash Relocation Density",
            "",
            "Goblin is started with `--hash-listpack-max-entries 0` so every "
            "case uses the full representation. A deterministic subset grows "
            f"from `{args.value_bytes}` to `{args.grown_value_bytes}` bytes. "
            "The first growth is timed separately because it lazily allocates "
            "one 64-field relocation block; HGET is measured before "
            "compaction. Each "
            f"row is the median of `{args.relocation_rounds}` fresh-server "
            f"rounds over `{args.relocation_fields:,}` fields.",
            "",
            "| pattern | density | grown fields | first grow us | HGET ops/s | "
            "pre-opt RSS B/field | post-opt RSS B/field | dead MiB | "
            "compact ms |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for result in relocation:
            before_rss = (
                result.grown_before_optimize.rss_bytes
                - result.baseline.rss_bytes
            )
            after_rss = (
                result.grown_after_optimize.rss_bytes
                - result.baseline.rss_bytes
            )
            dead = goblin_stat(
                result.grown_before_optimize, "field_value_dead_bytes"
            )
            lines.append(
                f"| `{result.pattern}` | {result.density * 100:g}% | "
                f"{result.grown_fields:,} | "
                f"{fmt(result.first_growth_latency_us)} | "
                f"{result.hget_operations_per_second:,.0f} | "
                f"{before_rss / result.fields:.2f} | "
                f"{after_rss / result.fields:.2f} | "
                f"{fmt(None if dead is None else dead / 2**20)} | "
                f"{result.optimize.seconds * 1000:.2f} |"
            )

    if mixed:
        lines += [
            "",
            "## Mixed Hash Write Latency",
            "",
            f"A seeded depth-one workload starts with `{args.mixed_fields:,}` "
            "fields and mixes inserts, same-width updates, deletes, and value "
            f"growth. `{args.mixed_warmup:,}` operations warm the exact workload "
            f"before `{args.mixed_samples:,}` measured samples. Percentiles are "
            "end-to-end RESP round-trip latency; p99.9 is not inferred from "
            f"throughput batches. Each row is the median of "
            f"`{args.mixed_rounds}` fresh-server rounds.",
            "",
            "| Engine | ops/s | insert/update/delete/grow | p50 us | p95 us | "
            "p99 us | p99.9 us | max us | RSS B/field | dead MiB | "
            "compacting | final fields |",
            "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | "
            "---: | ---: | ---: |",
        ]
        for result in mixed:
            mix = "/".join(
                f"{result.operations[name]:,}"
                for name in ("insert", "update", "delete", "grow")
            )
            rss_delta = result.after_mixed.rss_bytes - result.baseline.rss_bytes
            dead = goblin_stat(result.after_mixed, "field_value_dead_bytes")
            compacting = goblin_stat(
                result.after_mixed, "field_compaction_active"
            )
            lines.append(
                f"| `{result.engine}` | {result.operations_per_second:,.0f} | "
                f"{mix} | {result.latency_p50_us:.2f} | "
                f"{result.latency_p95_us:.2f} | {result.latency_p99_us:.2f} | "
                f"{result.latency_p999_us:.2f} | {result.latency_max_us:.2f} | "
                f"{rss_delta / result.final_fields:.2f} | "
                f"{fmt(None if dead is None else dead / 2**20)} | "
                f"{fmt(compacting, 0)} | "
                f"{result.final_fields:,} |"
            )

        lines += [
            "",
            "| Engine | operation | p50 us | p95 us | p99 us | p99.9 us | max us |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
        for result in mixed:
            for operation in ("insert", "update", "delete", "grow"):
                stats = result.operation_latency_us.get(operation)
                if stats is None:
                    continue
                lines.append(
                    f"| `{result.engine}` | {operation} | {stats['p50']:.2f} | "
                    f"{stats['p95']:.2f} | {stats['p99']:.2f} | "
                    f"{stats['p999']:.2f} | {stats['max']:.2f} |"
                )

    lines += [
        "",
        "The medium-hash representation crossover is measured separately in "
        "[HASH-THRESHOLD-SWEEP.md](HASH-THRESHOLD-SWEEP.md).",
        "",
        "## Binaries",
        "",
    ]
    for spec in args.engine:
        lines.append(f"- `{spec.label}`: `{spec.binary}`")
    lines.append("")
    return lines


def write_outputs(
    args: argparse.Namespace,
    large: Sequence[LargeHashResult],
    small: Sequence[SmallHashResult],
    relocation: Sequence[RelocationResult],
    mixed: Sequence[MixedHashResult],
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
            "warmup_requests": args.warmup_requests,
            "construction_rounds": args.construction_rounds,
            "baseline_warmup_commands": args.baseline_warmup_commands,
            "baseline_warmup_fields": args.baseline_warmup_fields,
            "optimize_density": args.optimize_density,
            "relocation_fields": args.relocation_fields,
            "relocation_densities": args.relocation_densities,
            "relocation_rounds": args.relocation_rounds,
            "mixed_fields": args.mixed_fields,
            "mixed_samples": args.mixed_samples,
            "mixed_warmup": args.mixed_warmup,
            "mixed_rounds": args.mixed_rounds,
            "mixed_weights": {
                "insert": args.mixed_insert_weight,
                "update": args.mixed_update_weight,
                "delete": args.mixed_delete_weight,
                "grow": args.mixed_grow_weight,
            },
            "seed": args.seed,
            "settle_seconds": args.settle_seconds,
            "redis_benchmark": str(args.redis_benchmark),
        },
        "large_hash": [asdict(result) for result in large],
        "small_hashes": [asdict(result) for result in small],
        "relocation_density": [asdict(result) for result in relocation],
        "mixed_hash_latency": [asdict(result) for result in mixed],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")
    args.report.write_text(
        "\n".join(report_lines(args, large, small, relocation, mixed))
    )


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
    parser.add_argument(
        "--warmup-requests",
        type=int,
        default=10_000,
        help="unmeasured redis-benchmark requests before each point metric",
    )
    parser.add_argument(
        "--construction-rounds",
        type=int,
        default=3,
        help="fresh-server construction/RSS rounds aggregated by median",
    )
    parser.add_argument("--baseline-warmup-commands", type=int, default=16)
    parser.add_argument(
        "--baseline-warmup-fields",
        type=int,
        default=2_048,
        help="fields in the create/delete hash used before baseline sampling",
    )
    parser.add_argument("--optimize-density", type=float, default=0.97)
    parser.add_argument("--relocation-fields", type=int, default=100_000)
    parser.add_argument(
        "--relocation-densities",
        default="0,0.001,0.01,0.1,0.5,1",
        help="comma-separated fractions of values to grow",
    )
    parser.add_argument("--relocation-rounds", type=int, default=1)
    parser.add_argument("--mixed-fields", type=int, default=100_000)
    parser.add_argument("--mixed-samples", type=int, default=10_000)
    parser.add_argument("--mixed-warmup", type=int, default=1_000)
    parser.add_argument("--mixed-rounds", type=int, default=1)
    parser.add_argument("--mixed-insert-weight", type=int, default=25)
    parser.add_argument("--mixed-update-weight", type=int, default=25)
    parser.add_argument("--mixed-delete-weight", type=int, default=25)
    parser.add_argument("--mixed-grow-weight", type=int, default=25)
    parser.add_argument("--seed", type=int, default=0xC0FFEE)
    parser.add_argument("--settle-seconds", type=float, default=0.5)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--skip-large", action="store_true")
    parser.add_argument("--skip-small", action="store_true")
    parser.add_argument("--skip-relocation", action="store_true")
    parser.add_argument("--skip-mixed", action="store_true")
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
    try:
        args.relocation_densities = [
            float(value)
            for value in args.relocation_densities.split(",")
            if value
        ]
    except ValueError as error:
        parser.error(f"--relocation-densities must contain numbers: {error}")
    if (
        args.skip_large
        and args.skip_small
        and args.skip_relocation
        and args.skip_mixed
    ):
        parser.error("at least one benchmark scenario must be enabled")
    if min(
        args.large_fields,
        args.small_total_fields,
        args.value_bytes,
        args.grown_value_bytes,
        args.hset_batch,
        args.pipeline,
        args.requests,
        args.rounds,
        args.construction_rounds,
        args.baseline_warmup_fields,
        args.relocation_fields,
        args.relocation_rounds,
        args.mixed_fields,
        args.mixed_samples,
        args.mixed_rounds,
    ) <= 0 or not args.small_shapes or min(args.small_shapes) <= 0:
        parser.error("all counts, sizes, shapes, pipeline depths, and rounds must be positive")
    if args.warmup_requests < 0 or args.baseline_warmup_commands < 0:
        parser.error("warmup request/command counts must be non-negative")
    if args.mixed_warmup < 0:
        parser.error("--mixed-warmup must be non-negative")
    if (
        not args.relocation_densities
        or min(args.relocation_densities) < 0.0
        or max(args.relocation_densities) > 1.0
    ):
        parser.error("--relocation-densities must be fractions in [0, 1]")
    mixed_weights = (
        args.mixed_insert_weight,
        args.mixed_update_weight,
        args.mixed_delete_weight,
        args.mixed_grow_weight,
    )
    if min(mixed_weights) < 0 or sum(mixed_weights) == 0:
        parser.error("mixed weights must be non-negative with a positive sum")
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
    if max(args.relocation_fields, args.mixed_fields + args.mixed_samples) > (
        10**RANDOM_TOKEN_DIGITS
    ):
        parser.error("relocation/mixed field ids exceed the fixed field width")
    projected_warm_blob = 8 + args.baseline_warmup_fields * (
        3 + 4 + len(large_field(0)) + args.value_bytes
    )
    if projected_warm_blob <= 65_535:
        parser.error(
            "--baseline-warmup-fields must cross the 65,535-byte compact "
            "hash boundary"
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
    relocation: list[RelocationResult] = []
    mixed: list[MixedHashResult] = []

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

    if not args.skip_relocation:
        for spec in (engine for engine in args.engine if engine.is_goblin):
            for pattern in ("random", "clustered"):
                for density in args.relocation_densities:
                    print(
                        f"benchmarking {pattern} relocation density "
                        f"{density * 100:g}% on {spec.label}...",
                        file=sys.stderr,
                        flush=True,
                    )
                    result = run_relocation(spec, args, pattern, density)
                    relocation.append(result)
                    print(
                        f"  first grow {fmt(result.first_growth_latency_us)} us, "
                        f"HGET {result.hget_operations_per_second:,.0f} ops/s, "
                        f"compact {result.optimize.seconds * 1000:.2f} ms",
                        file=sys.stderr,
                        flush=True,
                    )

    if not args.skip_mixed:
        for spec in args.engine:
            print(
                f"benchmarking mixed hash latency on {spec.label}...",
                file=sys.stderr,
                flush=True,
            )
            result = run_mixed(spec, args)
            mixed.append(result)
            print(
                f"  {result.operations_per_second:,.0f} ops/s, "
                f"p99 {result.latency_p99_us:.2f} us, "
                f"p99.9 {result.latency_p999_us:.2f} us",
                file=sys.stderr,
                flush=True,
            )

    write_outputs(args, large, small, relocation, mixed)
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main(sys.argv[1:]))
