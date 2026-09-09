#!/usr/bin/env python3
"""Summarize preserved incremental packed-merge trials; exclude warmups."""
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
    parser.add_argument("root", type=Path)
    parser.add_argument("--micro-dir", default="micro")
    parser.add_argument("--complete", action="store_true")
    args = parser.parse_args()
    result = {}
    checksums = {}
    for stage in ("00-baseline", "01-bitmap", "02-run-merge", "03-redistribute"):
        directory = args.root / stage
        if not directory.exists():
            if args.complete:
                raise ValueError(f"missing stage: {stage}")
            continue
        if args.complete and not (directory / "REPLAYS-PASS").exists():
            raise ValueError(f"replays not complete: {stage}")
        report = {}
        summary = directory / "replay-summary.tsv"
        if summary.exists():
            trials = rows(summary)
            if args.complete and len(trials) != 3:
                raise ValueError(f"expected three replay trials: {stage}")
            if any(row["status"] != "PASS" for row in trials):
                raise ValueError(f"failed replay in {stage}")
            if trials:
                seconds = [float(row["feed_seconds"]) for row in trials]
                report["replay_seconds"] = seconds
                report["median_seconds"] = median(seconds)
                report["rss_kib"] = [int(row["os_rss_kb"]) for row in trials]
        micro = {}
        for shape in ("local", "increment", "random", "churn", "rebalance"):
            timings = []
            for trial in (1, 2, 3):
                path = directory / args.micro_dir / f"{shape}-{trial}.tsv"
                if not path.exists():
                    if args.complete:
                        raise ValueError(f"missing timing: {path}")
                    continue
                [row] = rows(path)
                identity = (row["members"], row["operations"], row["checksum"])
                if checksums.setdefault(shape, identity) != identity:
                    raise ValueError(f"micro workload/result differs: {stage}/{shape}")
                timings.append(float(row["us_per_op"]))
            if timings:
                micro[shape] = {"us_per_op": timings, "median_us": median(timings)}
        if micro:
            report["micro"] = micro
        result[stage] = report
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
