#!/usr/bin/env python3
"""Digest redis-cli's alternating member/score ZRANGE output in O(1) memory."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys


MASK128 = (1 << 128) - 1


def clean_line(line: bytes) -> bytes:
    if line.endswith(b"\n"):
        line = line[:-1]
    if line.endswith(b"\r"):
        line = line[:-1]
    return line


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tie-order", choices=("lexical", "numeric"), required=True
    )
    args = parser.parse_args()

    ordered = hashlib.sha256()
    unordered_xor = 0
    unordered_sum = 0
    unordered_square_sum = 0
    count = 0
    order_violations = 0
    all_integral = True
    score_sum = 0
    min_member: int | None = None
    max_member: int | None = None
    min_score: float | None = None
    max_score: float | None = None
    previous_member_bytes: bytes | None = None
    previous_member_number: int | None = None
    previous_score: float | None = None

    stream = sys.stdin.buffer
    while True:
        member_line = stream.readline()
        if not member_line:
            break
        score_line = stream.readline()
        if not score_line:
            raise RuntimeError("ZRANGE output ended after a member without a score")

        member = clean_line(member_line)
        score_bytes = clean_line(score_line)
        try:
            member_number = int(member)
            score = float(score_bytes)
        except ValueError as exc:
            raise RuntimeError(
                f"non-numeric ZRANGE pair: member={member!r} score={score_bytes!r}"
            ) from exc
        if not math.isfinite(score):
            raise RuntimeError(f"non-finite score for member {member!r}")

        ordered.update(len(member).to_bytes(4, "little"))
        ordered.update(member)
        ordered.update(len(score_bytes).to_bytes(4, "little"))
        ordered.update(score_bytes)

        pair_hash = hashlib.blake2b(
            member + b"\0" + score_bytes,
            digest_size=16,
            person=b"wiki-zset-v1",
        ).digest()
        value = int.from_bytes(pair_hash, "little")
        unordered_xor ^= value
        unordered_sum = (unordered_sum + value) & MASK128
        unordered_square_sum = (unordered_square_sum + value * value) & MASK128

        if previous_score is not None:
            if score < previous_score:
                order_violations += 1
            elif score == previous_score:
                if args.tie_order == "lexical":
                    if previous_member_bytes is not None and member < previous_member_bytes:
                        order_violations += 1
                elif previous_member_number is not None and member_number < previous_member_number:
                    order_violations += 1

        integral_score = int(score)
        if score != integral_score:
            all_integral = False
        else:
            score_sum += integral_score

        min_member = member_number if min_member is None else min(min_member, member_number)
        max_member = member_number if max_member is None else max(max_member, member_number)
        min_score = score if min_score is None else min(min_score, score)
        max_score = score if max_score is None else max(max_score, score)
        previous_member_bytes = member
        previous_member_number = member_number
        previous_score = score
        count += 1

    result = {
        "count": count,
        "ordered_sha256": ordered.hexdigest(),
        "mapping_xor_blake2b128": f"{unordered_xor:032x}",
        "mapping_sum_blake2b128": f"{unordered_sum:032x}",
        "mapping_square_sum_blake2b128": f"{unordered_square_sum:032x}",
        "tie_order": args.tie_order,
        "order_violations": order_violations,
        "min_member": min_member,
        "max_member": max_member,
        "min_score": min_score,
        "max_score": max_score,
        "all_scores_integral": all_integral,
        "score_sum": score_sum if all_integral else None,
    }
    json.dump(result, sys.stdout, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"zset_digest.py: {exc}", file=sys.stderr)
        raise SystemExit(1)

