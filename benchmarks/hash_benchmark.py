#!/usr/bin/env python3
"""Hash memory benchmark, mirroring zset_benchmark.py's methodology.

Loads N field->value pairs into a single hash, runs GOBLIN.OPTIMIZE on Goblin
Core, and measures resident memory per field, the engine-reported memory per
field, and the fragmentation ratio -- across Goblin Core, Redis 7.2.4, Redis
8.8, and Valkey under the shared redis-parity.conf (which pins the
hash-max-listpack thresholds, so large hashes use the hashtable encoding on
every engine, the fair comparison).

The transport, server launch, RSS probe, and INFO/GOBLIN.MEMORY readers are
imported verbatim from zset_benchmark.py so the two benchmarks measure identically.

Example:
  ./hash_benchmark.py \
      --engine goblin:goblin:../build-release/goblin-core \
      --engine redis724:redis:/opt/redis-7.2.4/src/redis-server \
      --engine redis88:redis:/opt/redis-8.8/src/redis-server \
      --engine valkey:redis:/opt/valkey/src/valkey-server \
      --sizes 250000,500000,1000000,2000000,4000000
"""
from __future__ import annotations

import argparse
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import (  # noqa: E402  (path insert above)
    RespClient,
    goblin_memory_stats,
    process_rss_mib,
    redis_used_memory_mib,
    start_dragonfly,
    start_goblin,
    start_redis,
)


def field_for(i: int) -> str:
    return f"field:{i:010d}"


def value_for(i: int, value_bytes: int) -> str:
    # A distinct, fixed-width value of the requested byte length.
    return f"{i:0{value_bytes}d}"


def hset_commands(count: int, key: str, batch: int, value_bytes: int):
    for start in range(0, count, batch):
        command: list[object] = ["HSET", key]
        for i in range(start, min(start + batch, count)):
            command.append(field_for(i))
            command.append(value_for(i, value_bytes))
        yield command


@dataclass
class Row:
    engine: str
    fields: int
    rss_delta_mib: float
    rss_bytes_per_field: float
    used_memory_mib: float | None
    used_bytes_per_field: float | None
    fragmentation: float | None


def start_engine(kind: str, binary: Path):
    if kind == "goblin":
        # Hash keys ignore the zset knobs; start with defaults.
        return start_goblin(binary, rank_cache=False, rank_cache_mode="off")
    if kind == "redis":  # redis-server / valkey-server share the protocol + flags
        return start_redis(binary)
    if kind == "dragonfly":
        return start_dragonfly(binary)
    raise ValueError(f"unknown engine kind: {kind}")


def bench_once(label: str, kind: str, binary: Path, fields: int, batch: int,
               pipeline: int, optimize_density: str | None, settle: float,
               value_bytes: int) -> Row:
    server = start_engine(kind, binary)
    try:
        client = RespClient("127.0.0.1", server.port, timeout=600.0)
        try:
            key = f"hbench:{label}"
            rss_base = process_rss_mib(server.process.pid)

            client.pipeline(hset_commands(fields, key, batch, value_bytes), pipeline)

            if kind == "goblin" and optimize_density is not None:
                client.command("GOBLIN.OPTIMIZE", key, optimize_density)

            time.sleep(settle)
            rss_after = process_rss_mib(server.process.pid)
            rss_delta = rss_after - rss_base

            if kind == "goblin":
                stats = goblin_memory_stats(client, key)
                used_mib = (stats["total_allocated_bytes"] / (1024.0 * 1024.0)
                            if stats and "total_allocated_bytes" in stats else None)
            else:
                used_mib = redis_used_memory_mib(client)

            return Row(
                engine=label,
                fields=fields,
                rss_delta_mib=rss_delta,
                rss_bytes_per_field=rss_delta * 1024.0 * 1024.0 / fields,
                used_memory_mib=used_mib,
                used_bytes_per_field=(used_mib * 1024.0 * 1024.0 / fields
                                      if used_mib is not None else None),
                fragmentation=(rss_after / used_mib
                               if used_mib not in (None, 0) else None),
            )
        finally:
            client.close()
    finally:
        server.stop()


def parse_engine(spec: str) -> tuple[str, str, Path]:
    # LABEL:KIND:PATH  e.g. redis724:redis:/opt/redis-7.2.4/src/redis-server
    parts = spec.split(":", 2)
    if len(parts) != 3:
        raise argparse.ArgumentTypeError(
            f"--engine expects LABEL:KIND:PATH, got {spec!r}")
    label, kind, path = parts
    if kind not in ("goblin", "redis", "dragonfly"):
        raise argparse.ArgumentTypeError(
            f"kind must be goblin|redis|dragonfly, got {kind!r}")
    return label, kind, Path(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", type=parse_engine, required=True,
                        metavar="LABEL:KIND:PATH",
                        help="engine to test; repeatable (kind = goblin|redis)")
    parser.add_argument("--sizes", default="250000,500000,1000000,2000000,4000000",
                        help="comma-separated field counts to sweep")
    parser.add_argument("--value-bytes", type=int, default=16,
                        help="value length in bytes (field is fixed at 16)")
    parser.add_argument("--hset-batch", type=int, default=128,
                        help="fields per HSET command")
    parser.add_argument("--pipeline", type=int, default=256,
                        help="commands flushed per pipeline batch")
    parser.add_argument("--optimize-density", default="0.97",
                        help="GOBLIN.OPTIMIZE packing density (Goblin only)")
    parser.add_argument("--settle-seconds", type=float, default=0.2)
    parser.add_argument("--repeats", type=int, default=1,
                        help="runs per (engine, size); the min RSS is reported")
    args = parser.parse_args()

    sizes = [int(s) for s in args.sizes.split(",") if s]

    results: list[Row] = []
    for fields in sizes:
        for label, kind, binary in args.engine:
            runs = [bench_once(label, kind, binary, fields, args.hset_batch,
                               args.pipeline, args.optimize_density,
                               args.settle_seconds, args.value_bytes)
                    for _ in range(max(1, args.repeats))]
            # Report the leanest run (least allocator/settle noise), like the
            # zset memory sweep's min-of-runs.
            best = min(runs, key=lambda r: r.rss_bytes_per_field)
            results.append(best)
            print(f"  {label:>10} {fields:>9,} fields: "
                  f"{best.rss_bytes_per_field:6.1f} B/field RSS, "
                  f"{(best.used_bytes_per_field or 0):6.1f} B/field used, "
                  f"frag {best.fragmentation or float('nan'):.2f}",
                  file=sys.stderr)

    # Markdown: RSS bytes/field is the headline, one column per engine.
    labels = [label for label, _, _ in args.engine]
    print("\n**RSS bytes/field:**\n")
    print("| fields | " + " | ".join(labels) + " |")
    print("| --- | " + " | ".join("---:" for _ in labels) + " |")
    for fields in sizes:
        cells = []
        for label in labels:
            row = next((r for r in results if r.engine == label and r.fields == fields),
                       None)
            cells.append(f"`{row.rss_bytes_per_field:.1f}`" if row else "-")
        print(f"| {fields:,} | " + " | ".join(cells) + " |")

    print("\n**Engine-reported bytes/field (used_memory / total_allocated):**\n")
    print("| fields | " + " | ".join(labels) + " |")
    print("| --- | " + " | ".join("---:" for _ in labels) + " |")
    for fields in sizes:
        cells = []
        for label in labels:
            row = next((r for r in results if r.engine == label and r.fields == fields),
                       None)
            cells.append(f"`{row.used_bytes_per_field:.1f}`"
                         if row and row.used_bytes_per_field else "-")
        print(f"| {fields:,} | " + " | ".join(cells) + " |")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
