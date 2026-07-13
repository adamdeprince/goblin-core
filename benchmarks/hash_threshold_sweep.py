#!/usr/bin/env python3
"""Sweep indexed compact hashes against Goblin's full form and incumbents.

For every requested fields-per-hash value, the runner starts fresh servers for:

* Goblin compact: --hash-listpack-max-entries equals the tested cardinality;
* Goblin full: --hash-listpack-max-entries 0;
* each optional incumbent supplied with --engine.

Each row holds approximately a fixed total field count, measures trusted
`INFO memory` RSS, and drives first/middle/last/missing HGET plus a same-width
middle-field HSET through redis-benchmark.
"""

from __future__ import annotations

import argparse
import json
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

import hset_benchmark as hbench  # noqa: E402
import zset_benchmark as zbench  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SIZES = (
    "32,40,50,64,80,101,128,161,203,256,322,406,512,"
    "645,812,1024,1290,1625,2048"
)


@dataclass(frozen=True)
class Variant:
    label: str
    kind: str
    binary: Path
    hash_listpack_max_entries: int | None = None

    @property
    def is_goblin(self) -> bool:
        return self.kind == "goblin"


@dataclass
class SweepResult:
    fields_per_hash: int
    hashes: int
    total_fields: int
    variant: str
    kind: str
    binary: str
    hash_listpack_max_entries: int | None
    representation: str | None
    load_seconds: float
    load_fields_per_second: float
    load_hashes_per_second: float
    hset_operations_per_second: float
    hget_first_operations_per_second: float
    hget_middle_operations_per_second: float
    hget_last_operations_per_second: float
    hget_miss_operations_per_second: float
    baseline: hbench.MemorySample
    loaded: hbench.MemorySample


def field_name(field_id: int) -> str:
    return f"f:{field_id:012d}"


def key_name(hash_id: int) -> str:
    return f"h:{hash_id:012d}"


def value_for(ordinal: int, width: int) -> str:
    return hbench.fixed_value(ordinal, width, "v")


def load_commands(
    hashes: int, fields_per_hash: int, value_bytes: int
) -> Iterable[list[object]]:
    ordinal = 0
    for hash_id in range(hashes):
        command: list[object] = ["HSET", key_name(hash_id)]
        for field_id in range(fields_per_hash):
            command.append(field_name(field_id))
            command.append(value_for(ordinal, value_bytes))
            ordinal += 1
        yield command


def start_variant(variant: Variant) -> zbench.ServerProcess:
    if variant.is_goblin:
        threshold = variant.hash_listpack_max_entries
        extra_args: tuple[str, ...] = ()
        if threshold is not None:
            extra_args = ("--hash-listpack-max-entries", str(threshold))
        return zbench.start_goblin(
            variant.binary,
            rank_cache=False,
            rank_cache_mode="off",
            extra_args=extra_args,
        )
    spec = hbench.EngineSpec(variant.label, variant.kind, variant.binary)
    return hbench.start_engine(spec)


def sample_memory(
    client: zbench.RespClient,
    variant: Variant,
    sample_keys: Sequence[str] = (),
) -> hbench.MemorySample:
    spec = hbench.EngineSpec(variant.label, variant.kind, variant.binary)
    return hbench.memory_sample(client, spec, sample_keys)


def median_rate(
    args: argparse.Namespace,
    server: zbench.ServerProcess,
    command: Sequence[object],
    keyspace: int,
) -> float:
    return statistics.median(
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
    )


def verify_hashes(
    client: zbench.RespClient,
    hashes: int,
    fields_per_hash: int,
    value_bytes: int,
) -> None:
    for hash_id in (0, hashes // 2, hashes - 1):
        key = key_name(hash_id)
        if client.command("HLEN", key) != fields_per_hash:
            raise RuntimeError(f"{key} has the wrong field count")
        for field_id in (0, fields_per_hash // 2, fields_per_hash - 1):
            ordinal = hash_id * fields_per_hash + field_id
            expected = value_for(ordinal, value_bytes).encode()
            actual = client.command("HGET", key, field_name(field_id))
            if actual != expected:
                raise RuntimeError(
                    f"HGET mismatch for {key} field {field_id}: "
                    f"expected {expected!r}, got {actual!r}"
                )


def verify_updates(
    client: zbench.RespClient,
    hashes: int,
    fields_per_hash: int,
    value_bytes: int,
) -> None:
    middle = fields_per_hash // 2
    updated = ("u" * value_bytes).encode()
    for hash_id in (0, hashes // 2, hashes - 1):
        key = key_name(hash_id)
        if client.command("HLEN", key) != fields_per_hash:
            raise RuntimeError(f"{key} has the wrong field count after HSET")
        if client.command("HGET", key, field_name(middle)) != updated:
            raise RuntimeError(f"{key} has the wrong middle value after HSET")
        for field_id in (0, fields_per_hash - 1):
            ordinal = hash_id * fields_per_hash + field_id
            expected = value_for(ordinal, value_bytes).encode()
            if client.command("HGET", key, field_name(field_id)) != expected:
                raise RuntimeError(
                    f"{key} field {field_id} changed during middle-field HSET"
                )


def warm_server(client: zbench.RespClient, value_bytes: int) -> None:
    key = "__hash_sweep_warmup__"
    client.command("HSET", key, "field", "w" * value_bytes)
    client.command("HGET", key, "field")
    client.command("DEL", key)


def verify_goblin_representation(
    sample: hbench.MemorySample,
    variant: Variant,
    fields_per_hash: int,
    value_bytes: int,
) -> str | None:
    if not variant.is_goblin or sample.goblin_memory is None:
        return None
    index_bytes = sample.goblin_memory.get("field_index_allocated_bytes")
    if not isinstance(index_bytes, int):
        raise RuntimeError("GOBLIN.MEMORY did not report field index bytes")
    representation = "compact" if index_bytes == 0 else "full"
    threshold = variant.hash_listpack_max_entries
    if threshold == 0 and index_bytes == 0:
        raise RuntimeError("forced-full Goblin unexpectedly used a listpack")
    if threshold is not None and threshold > 0:
        field_bytes = len(field_name(0))
        projected_blob_bytes = 8 + fields_per_hash * (
            3 + 4 + field_bytes + value_bytes
        )
        expect_compact = (
            fields_per_hash <= threshold and projected_blob_bytes <= 0xFFFF
        )
        if expect_compact != (representation == "compact"):
            raise RuntimeError(
                f"Goblin used {representation} at {fields_per_hash} fields; "
                f"projected compact blob is {projected_blob_bytes} bytes"
            )
    return representation


def run_one(
    args: argparse.Namespace,
    variant: Variant,
    fields_per_hash: int,
) -> SweepResult:
    hashes = max(1, args.total_fields // fields_per_hash)
    total_fields = hashes * fields_per_hash
    server = start_variant(variant)
    try:
        client = zbench.RespClient("127.0.0.1", server.port, timeout=args.timeout)
        try:
            warm_server(client, args.value_bytes)
            time.sleep(args.settle_seconds)
            baseline = sample_memory(client, variant)
            _, load_seconds = zbench.time_pipeline(
                client,
                load_commands(hashes, fields_per_hash, args.value_bytes),
                args.load_pipeline,
            )
            verify_hashes(client, hashes, fields_per_hash, args.value_bytes)
            sample_keys = [
                key_name(0),
                key_name(hashes // 2),
                key_name(hashes - 1),
            ]
            time.sleep(args.settle_seconds)
            loaded = sample_memory(client, variant, sample_keys)
            representation = verify_goblin_representation(
                loaded, variant, fields_per_hash, args.value_bytes
            )

            middle = field_name(fields_per_hash // 2)
            rates = {
                "first": median_rate(
                    args, server, ["HGET", "h:__rand_int__", field_name(0)], hashes
                ),
                "middle": median_rate(
                    args, server, ["HGET", "h:__rand_int__", middle], hashes
                ),
                "last": median_rate(
                    args,
                    server,
                    ["HGET", "h:__rand_int__", field_name(fields_per_hash - 1)],
                    hashes,
                ),
                "miss": median_rate(
                    args, server, ["HGET", "h:__rand_int__", "missing"], hashes
                ),
                "hset": median_rate(
                    args,
                    server,
                    ["HSET", "h:__rand_int__", middle, "u" * args.value_bytes],
                    hashes,
                ),
            }
            verify_updates(client, hashes, fields_per_hash, args.value_bytes)
            return SweepResult(
                fields_per_hash=fields_per_hash,
                hashes=hashes,
                total_fields=total_fields,
                variant=variant.label,
                kind=variant.kind,
                binary=str(variant.binary),
                hash_listpack_max_entries=variant.hash_listpack_max_entries,
                representation=representation,
                load_seconds=load_seconds,
                load_fields_per_second=total_fields / load_seconds,
                load_hashes_per_second=hashes / load_seconds,
                hset_operations_per_second=rates["hset"],
                hget_first_operations_per_second=rates["first"],
                hget_middle_operations_per_second=rates["middle"],
                hget_last_operations_per_second=rates["last"],
                hget_miss_operations_per_second=rates["miss"],
                baseline=baseline,
                loaded=loaded,
            )
        finally:
            client.close()
    finally:
        server.stop()


def rss_delta(result: SweepResult) -> int:
    return result.loaded.rss_bytes - result.baseline.rss_bytes


def used_delta(result: SweepResult) -> int | None:
    return hbench.delta(
        result.loaded.used_memory_bytes, result.baseline.used_memory_bytes
    )


def report_lines(
    args: argparse.Namespace,
    variants: Sequence[Variant],
    results: Sequence[SweepResult],
) -> list[str]:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [
        "# Indexed Compact Hash Threshold Sweep",
        "",
        f"Generated on a dedicated benchmark host at {generated_at}.",
        "",
        "## Method",
        "",
        "- Every cardinality/variant starts a fresh server; servers run one at a time.",
        "- A one-field temporary hash warms HSET/HGET/DEL before the RSS baseline.",
        "- `goblin-compact` sets the compact-count threshold to the tested "
        "cardinality; the 64 KiB blob ceiling may still promote it. "
        "`goblin-full` forces the Swiss index and arena with threshold zero.",
        "- Each row loads approximately "
        f"`{args.total_fields:,}` total fields with `{args.value_bytes}`-byte values.",
        "- Construction is a Python RESP pipeline with one multi-field HSET per hash.",
        "- RSS is the trusted live `INFO memory used_memory_rss` delta. Sampled "
        "key bytes are `GOBLIN.MEMORY total_allocated_bytes` or `MEMORY USAGE`.",
        f"- Point rates use one client, pipeline `{args.pipeline}`, "
        f"`{args.requests:,}` requests, median of `{args.rounds}` runs.",
        "- Incumbents are exercised only as black-box RESP servers.",
        "",
        "## Memory",
        "",
        "| fields/hash | variant | representation | RSS MiB | RSS B/hash | RSS B/field | "
        "used B/hash | sampled B/hash |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        rss = rss_delta(result)
        used = used_delta(result)
        lines.append(
            f"| {result.fields_per_hash} | `{result.variant}` | "
            f"{result.representation or 'engine-defined'} | "
            f"{result.loaded.rss_bytes / 2**20:.2f} | "
            f"{rss / result.hashes:.2f} | {rss / result.total_fields:.2f} | "
            f"{hbench.fmt(hbench.per_unit(used, result.hashes))} | "
            f"{hbench.fmt(result.loaded.key_memory_bytes)} |"
        )

    lines += [
        "",
        "## Point Operations",
        "",
        "| fields/hash | variant | representation | HSET middle | HGET first | HGET middle | "
        "HGET last | HGET miss |",
        "| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for result in results:
        lines.append(
            f"| {result.fields_per_hash} | `{result.variant}` | "
            f"{result.representation or 'engine-defined'} | "
            f"{result.hset_operations_per_second:,.0f} | "
            f"{result.hget_first_operations_per_second:,.0f} | "
            f"{result.hget_middle_operations_per_second:,.0f} | "
            f"{result.hget_last_operations_per_second:,.0f} | "
            f"{result.hget_miss_operations_per_second:,.0f} |"
        )

    lines += [
        "",
        "## Construction",
        "",
        "| fields/hash | variant | representation | load fields/s | load hashes/s |",
        "| ---: | --- | --- | ---: | ---: |",
    ]
    for result in results:
        lines.append(
            f"| {result.fields_per_hash} | `{result.variant}` | "
            f"{result.representation or 'engine-defined'} | "
            f"{result.load_fields_per_second:,.0f} | "
            f"{result.load_hashes_per_second:,.0f} |"
        )

    lines += ["", "## Binaries", ""]
    for variant in variants:
        lines.append(f"- `{variant.label}`: `{variant.binary}`")
    lines.append("")
    return lines


def parse_sizes(parser: argparse.ArgumentParser, text: str) -> list[int]:
    try:
        values = [int(value) for value in text.split(",") if value]
    except ValueError as error:
        parser.error(f"--sizes must contain integers: {error}")
    if not values or min(values) <= 0 or len(set(values)) != len(values):
        parser.error("--sizes must contain distinct positive integers")
    return values


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goblin-bin", required=True, type=Path)
    parser.add_argument("--redis-benchmark", required=True, type=Path)
    parser.add_argument(
        "--engine",
        action="append",
        type=hbench.parse_engine,
        default=[],
        metavar="LABEL:KIND:PATH",
        help="optional incumbent; repeatable",
    )
    parser.add_argument("--sizes", default=DEFAULT_SIZES)
    parser.add_argument("--total-fields", type=int, default=500_000)
    parser.add_argument("--value-bytes", type=int, default=16)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--load-pipeline", type=int, default=128)
    parser.add_argument("--requests", type=int, default=200_000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--settle-seconds", type=float, default=0.25)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument(
        "--output-json",
        type=Path,
        default=ROOT / "benchmark-results" / "hash-threshold-sweep.json",
    )
    parser.add_argument(
        "--report",
        type=Path,
        default=ROOT / "benchmark-results" / "hash-threshold-sweep.md",
    )
    args = parser.parse_args(argv)
    args.sizes = parse_sizes(parser, args.sizes)
    if min(
        args.total_fields,
        args.value_bytes,
        args.pipeline,
        args.load_pipeline,
        args.requests,
        args.rounds,
    ) <= 0:
        parser.error("counts, sizes, pipeline depths, and rounds must be positive")
    if args.value_bytes <= 1 or args.value_bytes > 65535:
        parser.error("--value-bytes must be in [2, 65535]")
    if args.settle_seconds < 0 or args.timeout <= 0:
        parser.error("settle time must be non-negative and timeout positive")
    max_ordinal = args.total_fields - 1
    if len(f"{max_ordinal:x}") > args.value_bytes - 1:
        parser.error("--value-bytes is too small for distinct values")
    try:
        args.goblin_bin = zbench.resolve_executable(args.goblin_bin, "Goblin Core")
        args.redis_benchmark = zbench.resolve_executable(
            args.redis_benchmark, "redis-benchmark"
        )
        for engine in args.engine:
            zbench.resolve_executable(engine.binary, f"{engine.label} binary")
    except FileNotFoundError as error:
        parser.error(str(error))
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    variants = [
        Variant("goblin-compact", "goblin", args.goblin_bin),
        Variant("goblin-full", "goblin", args.goblin_bin, 0),
    ]
    variants.extend(
        Variant(engine.label, engine.kind, engine.binary) for engine in args.engine
    )
    results: list[SweepResult] = []
    for fields_per_hash in args.sizes:
        for base_variant in variants:
            variant = base_variant
            if base_variant.label == "goblin-compact":
                variant = Variant(
                    base_variant.label,
                    base_variant.kind,
                    base_variant.binary,
                    fields_per_hash,
                )
            print(
                f"benchmarking {fields_per_hash} fields/hash on {variant.label}...",
                file=sys.stderr,
                flush=True,
            )
            result = run_one(args, variant, fields_per_hash)
            results.append(result)
            print(
                f"  HSET {result.hset_operations_per_second:,.0f}, "
                f"HGET middle {result.hget_middle_operations_per_second:,.0f}, "
                f"RSS {rss_delta(result) / result.hashes:.1f} B/hash, "
                f"representation {result.representation or 'engine-defined'}",
                file=sys.stderr,
                flush=True,
            )

    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "host": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "logical_cpus": os.cpu_count(),
        },
        "config": {
            "sizes": args.sizes,
            "total_fields": args.total_fields,
            "value_bytes": args.value_bytes,
            "pipeline": args.pipeline,
            "load_pipeline": args.load_pipeline,
            "requests": args.requests,
            "rounds": args.rounds,
            "settle_seconds": args.settle_seconds,
            "redis_benchmark": str(args.redis_benchmark),
        },
        "results": [asdict(result) for result in results],
    }
    args.output_json.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.output_json.write_text(json.dumps(payload, indent=2) + "\n")
    args.report.write_text("\n".join(report_lines(args, variants, results)))
    print(f"wrote {args.output_json}")
    print(f"wrote {args.report}")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main(sys.argv[1:]))
