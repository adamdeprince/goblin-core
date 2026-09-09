#!/usr/bin/env python3
"""Validate and summarize the four frozen packed update-path stages."""
import argparse
import csv
import json
from pathlib import Path
from statistics import median


def rows(path):
    with path.open() as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("stages", type=Path)
    parser.add_argument("--final-stage", default="03-shared-path")
    parser.add_argument("--micro-dir", default="confirmation")
    args = parser.parse_args()
    result = {}
    identities = {}
    digest_reference = None
    for stage in ("00-baseline", "01-hash", "02-single-probe", args.final_stage):
        directory = args.stages / stage
        if not (directory / "REPLAYS-PASS").exists():
            raise ValueError(f"incomplete stage: {stage}")
        if stage != "00-baseline" and not (directory / "BUILD-TESTS-PASS").exists():
            raise ValueError(f"missing tests: {stage}")
        trials = rows(directory / "replay-summary.tsv")
        if len(trials) != 3 or any(row["status"] != "PASS" for row in trials):
            raise ValueError(f"expected three passing replay trials: {stage}")
        allocation = []
        for trial in (1, 2, 3):
            replay = directory / f"replay-{trial}"
            if "verification_status=0" not in (replay / "metadata.txt").read_text():
                raise ValueError(f"verification failed: {replay}")
            for log in (replay / "logs").iterdir():
                if log.name.endswith((".stderr", ".command-errors")) and log.stat().st_size:
                    raise ValueError(f"nonempty error log: {log}")
            status = (replay / "digests/goblin-packed-int32-float32.status").read_text().split()
            if status != ["0", "0"]:
                raise ValueError(f"digest pipeline failed: {replay}")
            digest = json.loads((replay / "digests/goblin-packed-int32-float32.json").read_text())
            if digest_reference is None:
                digest_reference = digest
            if digest != digest_reference or digest["score_sum"] != 2000000:
                raise ValueError(f"mapping/order mismatch: {replay}")
            fields = (replay / "results/goblin-packed-int32-float32.goblin-memory.txt").read_text().splitlines()
            memory = dict(zip(fields[::2], fields[1::2]))
            allocation.append(int(memory["total_allocated_bytes"]))
        seconds = [float(row["feed_seconds"]) for row in trials]
        report = {"replay_seconds": seconds, "median_seconds": median(seconds),
                  "rss_kib": [int(row["os_rss_kb"]) for row in trials],
                  "object_allocated_bytes": allocation, "micro": {}}
        for bench, shapes in (("packed-update", ("local", "increment", "random", "insert")),
                              ("packed-merge", ("local", "increment", "random", "churn", "rebalance"))):
            for shape in shapes:
                name = f"{bench}-{shape}"
                timings = []
                for trial in (1, 2, 3):
                    [row] = rows(directory / args.micro_dir / f"{name}-{trial}.tsv")
                    identity = (row["members"], row["operations"], row["checksum"])
                    if identities.setdefault(name, identity) != identity:
                        raise ValueError(f"focused identity mismatch: {stage}/{name}")
                    value = float(row["ns_per_op"]) if bench == "packed-update" else float(row["us_per_op"]) * 1000
                    timings.append(value)
                report["micro"][name] = {"ns_per_op": timings, "median_ns": median(timings)}
        result[stage] = report
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
