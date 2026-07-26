#!/usr/bin/env python3
"""Extract the sorted unique ev:sym channel set from a JSONL market feed."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import sys
import time
from pathlib import Path
from typing import BinaryIO


def _align_to_record(source: BinaryIO, start: int) -> None:
    if start == 0:
        source.seek(0)
        return
    source.seek(start - 1)
    if source.read(1) != b"\n":
        source.readline()


def _scan_range(
    input_path: str, start: int, end: int
) -> tuple[int, set[str]]:
    channels: set[str] = set()
    records = 0
    with open(input_path, "rb", buffering=16 * 1024 * 1024) as source:
        _align_to_record(source, start)
        while source.tell() < end:
            raw_record = source.readline()
            if not raw_record:
                break
            records += 1
            try:
                record = json.loads(raw_record)
                event = record["ev"]
                symbol = record["sym"]
            except (json.JSONDecodeError, KeyError, TypeError) as error:
                raise RuntimeError(
                    f"invalid record near byte {source.tell()}: {error}"
                ) from error
            if not isinstance(event, str) or not isinstance(symbol, str):
                raise RuntimeError(
                    f"record near byte {source.tell()}: "
                    "ev and sym must both be JSON strings"
                )
            channels.add(f"{event}:{symbol}")
    return records, channels


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--progress-every", type=int, default=10_000_000)
    parser.add_argument(
        "--workers", type=int, default=min(8, os.cpu_count() or 1)
    )
    parser.add_argument("--chunks-per-worker", type=int, default=4)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.workers <= 0 or args.chunks_per_worker <= 0:
        raise ValueError("--workers and --chunks-per-worker must be positive")

    channels: set[str] = set()
    records = 0
    started = time.monotonic()
    file_size = args.input.stat().st_size
    task_count = args.workers * args.chunks_per_worker
    ranges = [
        (index * file_size // task_count, (index + 1) * file_size // task_count)
        for index in range(task_count)
    ]

    next_progress = args.progress_every
    with concurrent.futures.ProcessPoolExecutor(
        max_workers=args.workers
    ) as executor:
        futures = [
            executor.submit(_scan_range, str(args.input), start, end)
            for start, end in ranges
        ]
        for future in concurrent.futures.as_completed(futures):
            range_records, range_channels = future.result()
            records += range_records
            channels.update(range_channels)
            if args.progress_every and records >= next_progress:
                elapsed = time.monotonic() - started
                print(
                    f"records={records} channels={len(channels)} "
                    f"records_per_second={records / elapsed:.0f}",
                    file=sys.stderr,
                    flush=True,
                )
                next_progress = (
                    records // args.progress_every + 1
                ) * args.progress_every

    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(
        f".{args.output.name}.tmp.{os.getpid()}"
    )
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as output:
            for channel in sorted(channels):
                output.write(channel)
                output.write("\n")
        os.replace(temporary, args.output)
    finally:
        temporary.unlink(missing_ok=True)

    elapsed = time.monotonic() - started
    print(
        json.dumps(
            {
                "records": records,
                "channels": len(channels),
                "seconds": elapsed,
                "records_per_second": records / elapsed if elapsed else 0,
                "workers": args.workers,
                "output": str(args.output),
            },
            separators=(",", ":"),
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
