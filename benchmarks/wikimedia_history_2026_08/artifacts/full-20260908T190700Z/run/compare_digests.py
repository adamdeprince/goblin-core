#!/usr/bin/env python3
"""Compare the independent Wikimedia benchmark's per-engine zset digests."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


MAPPING_FIELDS = (
    "count",
    "mapping_xor_blake2b128",
    "mapping_sum_blake2b128",
    "mapping_square_sum_blake2b128",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("digest_dir", type=Path)
    parser.add_argument("expected_increments", type=int)
    parser.add_argument("engines", nargs="+")
    parser.add_argument("--baseline-digest", type=Path,
                        help="reuse a known-good digest without rerunning its engine")
    args = parser.parse_args()

    loaded: dict[str, dict[str, object]] = {}
    for engine in args.engines:
        path = args.digest_dir / f"{engine}.json"
        try:
            loaded[engine] = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print(f"{engine}: FAIL: cannot read {path}: {exc}")

    baseline = loaded.get("redis-8.8")
    if args.baseline_digest is not None:
        try:
            baseline = json.loads(args.baseline_digest.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print(f"baseline: FAIL: cannot read {args.baseline_digest}: {exc}")
            return 1
    failed = baseline is None
    if baseline is None:
        print("baseline: FAIL: redis-8.8 digest is unavailable")

    for engine in args.engines:
        digest = loaded.get(engine)
        if digest is None:
            failed = True
            continue
        problems: list[str] = []
        if digest.get("order_violations") != 0:
            problems.append(f"order_violations={digest.get('order_violations')}")
        if digest.get("all_scores_integral") is not True:
            problems.append("non-integral score")
        if digest.get("score_sum") != args.expected_increments:
            problems.append(
                f"score_sum={digest.get('score_sum')} expected={args.expected_increments}"
            )
        if baseline is not None:
            differing = [
                field
                for field in MAPPING_FIELDS
                if digest.get(field) != baseline.get(field)
            ]
            if differing:
                problems.append("mapping differs: " + ",".join(differing))
            if digest.get("tie_order") == baseline.get("tie_order") and (
                digest.get("ordered_sha256") != baseline.get("ordered_sha256")
            ):
                problems.append("ordered ZRANGE digest differs")
        if problems:
            failed = True
            print(f"{engine}: FAIL: {'; '.join(problems)}")
        else:
            print(
                f"{engine}: PASS: {digest['count']} members, "
                f"score_sum={digest['score_sum']}, max_score={digest['max_score']}"
            )

    if not failed:
        print("all engines contain the same page_id -> edit-count mapping")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
