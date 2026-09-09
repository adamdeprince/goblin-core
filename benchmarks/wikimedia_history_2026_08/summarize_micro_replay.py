#!/usr/bin/env python3
"""Validate every paired replay before reporting medians or memory use."""
import argparse
import csv
import json
from pathlib import Path
from statistics import median

INPUT_SHA256 = "3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e"
ORDER_SHA256 = "5bbb15683fa301f4c873b6740b978dd23c9c81fa53b9e478323cd8fb8654106e"
ENGINE = "goblin-packed-int32-float32"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--stages", nargs="+", default=["00-retained", "07-selected"])
    parser.add_argument("--trials", type=int, default=3)
    args = parser.parse_args()
    reference = None
    allocations = set()
    report = {}
    for stage in args.stages:
        directory = args.project / "stages" / stage
        if not (directory / "REPLAYS-PASS").is_file():
            raise ValueError(f"incomplete replays: {stage}")
        with (directory / "replay-summary.tsv").open() as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        if len(rows) != args.trials or [row["trial"] for row in rows] != [str(i) for i in range(1, args.trials + 1)]:
            raise ValueError(f"incomplete/duplicate trials: {stage}")
        memory = []
        for trial, row in enumerate(rows, 1):
            replay = directory / f"replay-{trial}"
            if row["status"] != "PASS" or not (replay / "COMPLETE").is_file():
                raise ValueError(f"failed replay: {replay}")
            metadata = (replay / "metadata.txt").read_text().splitlines()
            for expected in ("verification_status=0", "payload_bytes=41237066", "head_lines=2000000"):
                if expected not in metadata:
                    raise ValueError(f"missing {expected}: {replay}")
            if (replay / "input.sha256").read_text().split()[0] != INPUT_SHA256:
                raise ValueError(f"input changed: {replay}")
            for log in (replay / "logs").iterdir():
                if log.name.endswith((".stderr", ".command-errors")) and log.stat().st_size:
                    raise ValueError(f"nonempty error log: {log}")
            if (replay / f"digests/{ENGINE}.status").read_text().split() != ["0", "0"]:
                raise ValueError(f"digest pipeline failed: {replay}")
            digest = json.loads((replay / f"digests/{ENGINE}.json").read_text())
            if reference is None:
                reference = digest
            if digest != reference or digest["ordered_sha256"] != ORDER_SHA256 or digest["score_sum"] != 2000000 or digest["count"] != 279458:
                raise ValueError(f"incorrect final mapping/order: {replay}")
            fields = (replay / f"results/{ENGINE}.goblin-memory.txt").read_text().splitlines()
            usage = dict(zip(fields[::2], fields[1::2]))
            memory.append(int(usage["total_allocated_bytes"]))
            allocations.add(memory[-1])
        seconds = [float(row["feed_seconds"]) for row in rows]
        report[stage] = {"seconds": seconds, "median_seconds": median(seconds),
                         "rss_kib": [int(row["os_rss_kb"]) for row in rows],
                         "allocated_bytes": memory}
    if len(allocations) != 1:
        raise ValueError("object allocation differs between replays")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
