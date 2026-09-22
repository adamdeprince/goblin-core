#!/usr/bin/env python3
"""Gate a detached full RLE matrix on a verified prefix replay, then report it."""

from __future__ import annotations

import csv
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
from datetime import datetime, timezone
import traceback


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def execute(project: Path, command: list[str], *, env: dict[str, str] | None = None,
            timeout: int | None = None, pid_file: str | None = None) -> int:
    child = subprocess.Popen(command, cwd=project, env=env, start_new_session=True)
    if pid_file:
        (project / pid_file).write_text(f"{child.pid}\n")
    try:
        return child.wait(timeout=timeout)
    except BaseException:
        # This process group was created exclusively for this invocation. It
        # includes its feeds, servers, samplers, and digest subprocesses.
        try:
            os.killpg(child.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            child.wait(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
        raise


def report(project: Path, output: Path, commands: int) -> None:
    with (output / "summary.tsv").open() as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    text = [
        "# Wikimedia typed-layout and RLE comparison", "",
        f"Generated {utc()}. Run: `{project.name}`.", "",
        f"Each variant receives {commands:,} increments in source order. All variants "
        "run concurrently from empty sets using the same frozen Release executable, "
        "merge exponent 0.5, Unix sockets, and ordinary reply-per-command redis-cli.", "",
        "UUID members are zero-extended numeric page IDs. Their longer command file "
        "is generated before timing. Digests normalize UUIDs back to decimal page IDs "
        "and compare against an independent reference, including numeric tie order. "
        "Compare RLE on/off within each layout; UUID and integer wire sizes differ.", "",
        "| Layout | RLE | Feed status | Seconds | Increments/s | Final RSS GiB | "
        "Object GiB | Compressed leaves | Used base score MiB |",
        "|---|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    modes: dict[tuple[str, str], dict[str, float]] = {}
    for row in rows:
        name = row["server"]
        layout, rle = name.removeprefix("goblin-packed-").rsplit("-rle-", 1)
        seconds = float(row["feed_seconds"])
        memory_path = output / "results" / f"{name}.goblin-memory.txt"
        fields = memory_path.read_text().splitlines() if memory_path.exists() else []
        memory = dict(zip(fields[::2], fields[1::2]))
        allocated = int(memory.get("total_allocated_bytes", "0"))
        rss = int(row["os_rss_kb"]) * 1024
        modes[layout, rle] = {"seconds": seconds, "allocated": allocated, "rss": rss}
        text.append(
            f"| {layout} | {rle} | {row['status']} | {seconds:,.3f} | "
            f"{commands / seconds if seconds else 0:,.0f} | {rss / 2**30:.3f} | "
            f"{allocated / 2**30:.3f} | {memory.get('compressed_leaf_count', 'n/a')} | "
            f"{int(memory.get('sorted_score_bytes', '0')) / 2**20:.3f} |"
        )
    text += ["", "Positive savings below mean RLE used less time or memory.", "",
             "| Layout | Feed time saved | RSS saved | Object allocation saved |",
             "|---|---:|---:|---:|"]
    for layout in sorted({key[0] for key in modes}):
        if (layout, "off") not in modes or (layout, "on") not in modes:
            continue
        off, on = modes[layout, "off"], modes[layout, "on"]
        savings = [f"{100 * (1 - on[key] / off[key]):+.2f}%" if off[key] else "n/a"
                   for key in ("seconds", "rss", "allocated")]
        text.append(f"| {layout} | {' | '.join(savings)} |")
    verification = output / "verification.txt"
    text += ["", "## Full-state verification", "", "```text",
             verification.read_text().strip() if verification.exists() else "Unavailable",
             "```", "", "RSS is sampled process residency immediately after feed drain, "
             "before verification; it is not peak RSS. Object allocation is the separate "
             "GOBLIN.MEMORY counter. No final forced compaction is performed. This is one "
             "concurrent trial per variant, including client and protocol costs; it does "
             "not establish maximum throughput or statistical significance.", ""]
    (output / "comparison.md").write_text("\n".join(text))


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: run_rle_full.py PROJECT_DIR")
    project = Path(sys.argv[1]).resolve()
    # Exclusive creation prevents accidentally launching over an existing job.
    log = (project / "controller.log").open("x", buffering=1)
    os.dup2(log.fileno(), sys.stdout.fileno())
    os.dup2(log.fileno(), sys.stderr.fileno())
    (project / "controller.pid").write_text(f"{os.getpid()}\n")
    (project / "started.utc").write_text(utc() + "\n")
    (project / "JOB-RUNNING").touch(exist_ok=False)
    def interrupt(_signal: int, _frame: object) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupt)
    status = 1
    try:
        for name in ("build", "prepare-input"):
            if (project / "preflight" / f"{name}.status").read_text().strip() != "0":
                raise RuntimeError(f"{name} preflight did not pass")
        variants = [f"goblin-packed-{member}-{score}-rle-{rle}"
                    for member in ("int32", "int64", "uuid")
                    for score in ("float32", "float64") for rle in ("off", "on")]
        (project / "matrix.json").write_text(json.dumps({
            "variants": variants, "merge_exponent": 0.5, "concurrent": True,
            "commands_per_full_run": 1483700913, "expected_members": 80798328,
            "uuid_mapping": "128-bit zero extension of nonnegative INT32 page ID",
        }, indent=2) + "\n")
        with (project / "preflight/harness.sha256").open("x") as hashes:
            for path in sorted((project / "harness").iterdir()):
                if path.is_file():
                    hashes.write(hashlib.sha256(path.read_bytes()).hexdigest() +
                                 "  " + str(path.relative_to(project)) + "\n")
        env = os.environ | {
            "GOBLIN": str(project / "bin/goblin-core"),
            "REDIS_CLI": "/home/adam/bench/redis-8.8.0/src/redis-cli",
            "PAYLOAD": str(project / "input/numeric.cmds"),
            "PAYLOAD_UUID": str(project / "input/uuid.cmds"),
            "SERVERS": " ".join(variants), "PACKED_MERGE_EXPONENT": "0.5",
            "HASH_INPUT": "0", "VERIFY": "1", "DIGEST_PAGE_SIZE": "65536",
            "EXPECTED_FULL_COMMANDS": "1483700913", "CMP_KEY": "key",
        }
        for phase, lines, reference, timeout in (
            ("smoke-2000000", "2000000", "reference-prefix-2000000.json", 900),
            ("run", "all", "reference-numeric.json", None),
        ):
            (project / "phase").write_text(phase + "\n")
            (project / f"{phase}-started.utc").write_text(utc() + "\n")
            print(f"{utc()} starting {phase}: {len(variants)} variants", flush=True)
            phase_env = env | {
                "OUTDIR": str(project / phase), "HEAD_LINES": lines,
                "SAMPLE_SECONDS": "60" if phase.startswith("smoke") else "300",
                "REFERENCE_DIGEST": str(project / "preflight" / reference),
            }
            status = execute(project, ["bash", str(project / "harness/run_rle_matrix.sh")],
                             env=phase_env, timeout=timeout, pid_file=f"{phase}.pid")
            if (project / phase / "summary.tsv").exists():
                report(project, project / phase, 2000000 if lines != "all" else 1483700913)
            (project / f"{phase}-finished.utc").write_text(utc() + "\n")
            if status:
                raise RuntimeError(f"{phase} failed with exit status {status}")
        (project / "phase").write_text("post-run-input-verification\n")
        with (project / "input-after.sha256").open("x") as after:
            subprocess.run(["sha256sum", "input/numeric.cmds", "input/uuid.cmds"],
                           cwd=project, stdout=after, check=True)
        if (project / "input-after.sha256").read_bytes() != (
                project / "preflight/payloads.sha256").read_bytes():
            raise RuntimeError("input checksum changed during replay")
        (project / "phase").write_text("complete\n")
        status = 0
    except KeyboardInterrupt:
        status = 130
        print("controller interrupted", flush=True)
    except Exception:
        status = 1
        traceback.print_exc()
    finally:
        (project / "exit.status").write_text(f"{status}\n")
        (project / "finished.utc").write_text(utc() + "\n")
        (project / "JOB-RUNNING").rename(project / ("JOB-PASS" if status == 0 else "JOB-FAILED"))
        print(f"{utc()} controller exit={status}", flush=True)
    return status


if __name__ == "__main__":
    raise SystemExit(main())
