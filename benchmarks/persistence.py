#!/usr/bin/env python3
"""Persistence: save time, file size, and load time, on one host.

For each engine: populate an N-member sorted set, save (Goblin Core's background
GOBLIN.SAVE / Redis's blocking SAVE), then start a fresh server pointing at the
saved file and time startup-to-ready minus an empty-start baseline. Reuses
zset_benchmark.py's transport + parity config.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from zset_benchmark import RespClient, free_port, wait_for_server  # noqa: E402

PARITY_CONF = Path(__file__).resolve().parent / "redis-parity.conf"
DEVNULL = subprocess.DEVNULL


def start_redis(binary: Path, port: int, directory: Path) -> subprocess.Popen:
    cmd = [str(binary)]
    if PARITY_CONF.exists():
        cmd.append(str(PARITY_CONF))
    cmd += ["--bind", "127.0.0.1", "--port", str(port), "--save", "",
            "--appendonly", "no", "--protected-mode", "no", "--dir", str(directory)]
    return subprocess.Popen(cmd, stdout=DEVNULL, stderr=DEVNULL)


def start_goblin(binary: Path, port: int, load_path: Path | None) -> subprocess.Popen:
    cmd = [str(binary), "--port", str(port)]
    if load_path is not None:
        cmd += ["--load", str(load_path)]
    return subprocess.Popen(cmd, stdout=DEVNULL, stderr=DEVNULL)


def populate(port: int, members: int, batch: int, pipeline: int) -> None:
    client = RespClient("127.0.0.1", port, timeout=600.0)

    def commands():
        for start in range(0, members, batch):
            cmd: list[object] = ["ZADD", "persist"]
            for i in range(start, min(start + batch, members)):
                cmd.append(float((i * 1_103_515_245 + 12_345) & 0xFFFFFFFF))
                cmd.append(f"member:{i:010d}")
            yield cmd

    client.pipeline(commands(), pipeline)
    client.close()


def time_ready(start_fn) -> float:
    port = free_port()
    t0 = time.monotonic()
    proc = start_fn(port)
    wait_for_server(port, timeout=60.0)
    elapsed = time.monotonic() - t0
    proc.terminate()
    proc.wait(timeout=10)
    return elapsed


def bench_goblin(binary: Path, members: int, batch: int, pipeline: int, accel: bool):
    path = Path(tempfile.gettempdir()) / "persist_goblin.gcsn"
    if path.exists():
        path.unlink()
    port = free_port()
    proc = start_goblin(binary, port, None)
    wait_for_server(port)
    populate(port, members, batch, pipeline)
    client = RespClient("127.0.0.1", port, timeout=600.0)
    t0 = time.monotonic()
    client.command("GOBLIN.SAVE", str(path)) if accel else \
        client.command("GOBLIN.SAVE", str(path), "NOACCEL")
    while not path.exists():  # background fork renames into place on completion
        time.sleep(0.001)
    save_time = time.monotonic() - t0
    client.close()
    proc.terminate()
    proc.wait(timeout=10)
    file_mb = path.stat().st_size / (1024.0 * 1024.0)

    baseline = time_ready(lambda p: start_goblin(binary, p, None))
    loaded = time_ready(lambda p: start_goblin(binary, p, path))
    return save_time, file_mb, max(0.0, loaded - baseline)


def bench_redis(binary: Path, members: int, batch: int, pipeline: int):
    directory = Path(tempfile.mkdtemp(prefix="persist-redis-"))
    port = free_port()
    proc = start_redis(binary, port, directory)
    wait_for_server(port)
    populate(port, members, batch, pipeline)
    client = RespClient("127.0.0.1", port, timeout=600.0)
    t0 = time.monotonic()
    client.command("SAVE")
    save_time = time.monotonic() - t0
    client.close()
    proc.terminate()
    proc.wait(timeout=10)
    rdb = directory / "dump.rdb"
    file_mb = rdb.stat().st_size / (1024.0 * 1024.0)

    empty = Path(tempfile.mkdtemp(prefix="persist-redis-empty-"))
    baseline = time_ready(lambda p: start_redis(binary, p, empty))
    loaded = time_ready(lambda p: start_redis(binary, p, directory))
    return save_time, file_mb, max(0.0, loaded - baseline)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", action="append", required=True,
                        metavar="LABEL:KIND:PATH")
    parser.add_argument("--members", type=int, default=1_000_000)
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    engines = [tuple(spec.split(":", 2)) for spec in args.engine]
    rows = []
    for label, kind, path in engines:
        best = None
        for _ in range(max(1, args.repeats)):
            if kind == "goblin":
                r = bench_goblin(Path(path), args.members, args.batch, args.pipeline, True)
            else:
                r = bench_redis(Path(path), args.members, args.batch, args.pipeline)
            # keep the run with the fastest load (least noise)
            if best is None or r[2] < best[2]:
                best = r
        save_time, file_mb, load_time = best
        rows.append((label, save_time, file_mb, load_time))
        print(f"  {label:>10}: save {save_time:.3f}s  file {file_mb:.1f}MB  "
              f"load {load_time:.3f}s", file=sys.stderr)

    print("\n| engine | save | file | load |")
    print("| --- | ---: | ---: | ---: |")
    for label, save_time, file_mb, load_time in rows:
        print(f"| {label} | `{save_time:.3f}s` | `{file_mb:.1f}` MB | `{load_time:.3f}s` |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
