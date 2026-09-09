#!/usr/bin/env python3
"""Check focused workload identities and report paired, warmup-excluded timings."""
import argparse
import csv
import json
from pathlib import Path
from statistics import median


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("--label", required=True)
    parser.add_argument("--stages", nargs="+", required=True)
    parser.add_argument("--trials", type=int, default=7)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    identities = {}
    report = {}
    for bench, shapes in (
        ("packed-update", ("local", "increment", "random", "insert", "churn", "erase", "noop")),
        ("packed-merge", ("local", "increment", "random", "churn", "rebalance")),
    ):
        for shape in shapes:
            name = f"{bench}-{shape}"
            report[name] = {}
            for stage in args.stages:
                timings = []
                for trial in range(args.trials + 1):
                    path = args.project / "stages" / stage / args.label / f"{name}-{trial}.tsv"
                    with path.open() as stream:
                        [row] = list(csv.DictReader(stream, delimiter="\t"))
                    identity = tuple(row.get(field) for field in
                                     ("shape", "members", "operations", "checksum", "allocated_bytes", "leaves"))
                    if identities.setdefault(name, identity) != identity:
                        raise ValueError(f"workload/memory/result mismatch: {path}")
                    ns = (float(row["ns_per_op"]) if bench == "packed-update"
                          else 1000 * float(row["us_per_op"]))
                    if ns <= 0:
                        raise ValueError(f"invalid timing: {path}")
                    if trial:
                        timings.append(ns)
                baseline = report[name].get(args.stages[0])
                paired = ([100 * (new / old - 1) for new, old in
                           zip(timings, baseline["ns_per_op"])] if baseline else [0.0] * len(timings))
                report[name][stage] = {
                    "ns_per_op": timings, "median_ns": median(timings),
                    "median_paired_change_percent": median(paired),
                    "identity": dict(zip(("shape", "members", "operations", "checksum", "allocated_bytes", "leaves"), identity)),
                }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print("workload\t" + "\t".join(args.stages))
        for name, stages in report.items():
            print(name + "\t" + "\t".join(
                f"{data['median_ns']:.3f} ({data['median_paired_change_percent']:+.2f}%)"
                for data in stages.values()))


if __name__ == "__main__":
    main()
