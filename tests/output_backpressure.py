#!/usr/bin/env python3
"""Regression test for bounded server output buffering."""

from __future__ import annotations

import argparse
import shutil
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parents[1]


def default_goblin_binary() -> Path:
    release = ROOT / "build-release" / "goblin-core"
    if release.exists():
        return release
    return ROOT / "build" / "goblin-core"


def encode_command(parts: Sequence[object]) -> bytes:
    out = bytearray()
    out.extend(f"*{len(parts)}\r\n".encode())
    for part in parts:
        data = part if isinstance(part, bytes) else str(part).encode()
        out.extend(f"${len(data)}\r\n".encode())
        out.extend(data)
        out.extend(b"\r\n")
    return bytes(out)


def process_rss_mib(pid: int) -> float:
    output = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True)
    rss_kib = int(output.strip() or "0")
    return rss_kib / 1024.0


class PeakRssSampler:
    def __init__(self, pid: int, interval_seconds: float, initial_mib: float) -> None:
        self.pid = pid
        self.interval_seconds = interval_seconds
        self.peak_mib = initial_mib
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self) -> "PeakRssSampler":
        self.sample_once()
        self._thread.start()
        return self

    def __exit__(self, *_exc: object) -> None:
        self._stop.set()
        self._thread.join()
        self.sample_once()

    def sample_once(self) -> None:
        try:
            rss = process_rss_mib(self.pid)
        except Exception:  # noqa: BLE001 - process may exit during teardown.
            return
        with self._lock:
            self.peak_mib = max(self.peak_mib, rss)

    def peak(self) -> float:
        with self._lock:
            return self.peak_mib

    def _run(self) -> None:
        while not self._stop.wait(self.interval_seconds):
            self.sample_once()


class RespClient:
    def __init__(self, port: int, timeout: float) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()

    def close(self) -> None:
        self.sock.close()

    def command_skip(self, *parts: object) -> None:
        self.sock.sendall(encode_command(parts))
        self.read_response_skip()

    def send_batch_no_read(self, commands: Iterable[Sequence[object]]) -> int:
        count = 0
        payload = bytearray()
        for command in commands:
            payload.extend(encode_command(command))
            count += 1
        self.sock.sendall(payload)
        return count

    def pipeline_skip(self, commands: Iterable[Sequence[object]], flush_every: int) -> int:
        count = 0
        pending = 0
        payload = bytearray()
        for command in commands:
            payload.extend(encode_command(command))
            count += 1
            pending += 1
            if pending >= flush_every:
                self.sock.sendall(payload)
                payload.clear()
                for _ in range(pending):
                    self.read_response_skip()
                pending = 0

        if pending:
            self.sock.sendall(payload)
            for _ in range(pending):
                self.read_response_skip()

        return count

    def read_response_skip(self) -> None:
        prefix = self._read_exact(1)
        if prefix in (b"+", b"-", b":"):
            self._read_line()
            return
        if prefix == b"$":
            length = int(self._read_line())
            if length == -1:
                return
            payload = self._read_exact(length + 2)
            if payload[-2:] != b"\r\n":
                raise RuntimeError("invalid RESP bulk terminator")
            return
        if prefix == b"*":
            count = int(self._read_line())
            if count == -1:
                return
            for _ in range(count):
                self.read_response_skip()
            return
        raise RuntimeError(f"invalid RESP prefix: {prefix!r}")

    def _read_line(self) -> bytes:
        while True:
            pos = self.buffer.find(b"\r\n")
            if pos >= 0:
                line = bytes(self.buffer[:pos])
                del self.buffer[: pos + 2]
                return line
            self._recv_more()

    def _read_exact(self, size: int) -> bytes:
        while len(self.buffer) < size:
            self._recv_more()
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    def _recv_more(self) -> None:
        data = self.sock.recv(1024 * 1024)
        if not data:
            raise RuntimeError("connection closed while reading RESP response")
        self.buffer.extend(data)


@dataclass
class ServerProcess:
    process: subprocess.Popen[bytes]
    port: int

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def resolve_executable(path: Path) -> Path:
    if path.exists():
        return path
    resolved = shutil.which(str(path))
    if resolved is None:
        raise FileNotFoundError(f"Goblin Core binary not found: {path}")
    return Path(resolved)


def wait_for_server(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            client = RespClient(port, timeout=1.0)
            try:
                client.command_skip("PING")
                return
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - preserve startup context.
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"server on port {port} did not become ready: {last_error}")


def start_goblin(binary: Path, timeout: float, max_output_buffer_mib: int) -> ServerProcess:
    binary = resolve_executable(binary)
    port = free_port()
    process = subprocess.Popen(
        [
            str(binary),
            "--port",
            str(port),
            "--max-output-buffer-mib",
            str(max_output_buffer_mib),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    server = ServerProcess(process, port)
    try:
        wait_for_server(port, timeout)
    except Exception:
        server.stop()
        raise
    return server


def zadd_commands(member_count: int, key: str, batch_size: int) -> Iterable[list[object]]:
    for start in range(0, member_count, batch_size):
        command: list[object] = ["ZADD", key]
        for member_id in range(start, min(start + batch_size, member_count)):
            command.extend([member_id, f"member-{member_id:08d}"])
        yield command


def range_commands(command_count: int,
                   member_count: int,
                   key: str,
                   range_size: int) -> Iterable[list[object]]:
    max_start = member_count - range_size
    for i in range(command_count):
        start = (i * 17) % (max_start + 1)
        yield ["ZRANGE", key, start, start + range_size - 1, "WITHSCORES"]


def run_test(args: argparse.Namespace) -> None:
    server = start_goblin(args.goblin_bin, args.timeout, args.max_output_buffer_mib)
    client = RespClient(server.port, args.timeout)
    key = "backpressure"
    try:
        client.pipeline_skip(zadd_commands(args.members, key, args.zadd_batch), args.load_pipeline)
        time.sleep(args.settle_seconds)
        rss_after_load = process_rss_mib(server.process.pid)

        with PeakRssSampler(
            server.process.pid,
            args.rss_sample_interval_ms / 1000.0,
            rss_after_load,
        ) as sampler:
            command_count = client.send_batch_no_read(
                range_commands(args.range_commands, args.members, key, args.range_size)
            )
            time.sleep(args.read_delay_ms / 1000.0)
            for _ in range(command_count):
                client.read_response_skip()
            rss_final = process_rss_mib(server.process.pid)

        peak_rss = max(sampler.peak(), rss_final)
        peak_delta = peak_rss - rss_after_load
        if peak_delta > args.max_peak_rss_delta_mib:
            raise RuntimeError(
                "output backpressure peak RSS delta too high: "
                f"load={rss_after_load:.2f} MiB peak={peak_rss:.2f} MiB "
                f"delta={peak_delta:.2f} MiB "
                f"limit={args.max_peak_rss_delta_mib:.2f} MiB"
            )

        print(
            "output backpressure passed: "
            f"commands={command_count}, range_size={args.range_size}, "
            f"rss_load={rss_after_load:.2f} MiB, "
            f"peak={peak_rss:.2f} MiB, delta={peak_delta:.2f} MiB, "
            f"cap={args.max_output_buffer_mib} MiB"
        )
    finally:
        client.close()
        server.stop()


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin_binary())
    parser.add_argument("--members", type=int, default=10_000)
    parser.add_argument("--range-commands", type=int, default=4_096)
    parser.add_argument("--range-size", type=int, default=256)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--load-pipeline", type=int, default=128)
    parser.add_argument("--max-output-buffer-mib", type=int, default=1)
    parser.add_argument("--max-peak-rss-delta-mib", type=float, default=24.0)
    parser.add_argument("--rss-sample-interval-ms", type=float, default=2.0)
    parser.add_argument("--read-delay-ms", type=float, default=100.0)
    parser.add_argument("--settle-seconds", type=float, default=0.05)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args(argv)

    if min(args.members, args.range_commands, args.range_size,
           args.zadd_batch, args.load_pipeline) <= 0:
        parser.error("member, command, range, batch, and pipeline counts must be positive")
    if args.range_size > args.members:
        parser.error("--range-size must be less than or equal to --members")
    if args.max_output_buffer_mib < 0:
        parser.error("--max-output-buffer-mib must be non-negative")
    if args.max_peak_rss_delta_mib <= 0 or args.rss_sample_interval_ms <= 0:
        parser.error("--max-peak-rss-delta-mib and --rss-sample-interval-ms must be positive")
    if args.read_delay_ms < 0 or args.settle_seconds < 0 or args.timeout <= 0:
        parser.error("--read-delay-ms and --settle-seconds must be non-negative; timeout must be positive")
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    run_test(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
