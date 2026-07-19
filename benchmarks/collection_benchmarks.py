#!/usr/bin/env python3
"""Run native SET/ARRAY workloads and render publication-ready artifacts."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import datetime as dt
import json
import os
import pathlib
import signal
import socket
import statistics
import struct
import subprocess
import tempfile
import time
from typing import Any, Iterable


ROOT = pathlib.Path(__file__).resolve().parents[1]


@dataclasses.dataclass(frozen=True)
class EngineSpec:
    label: str
    kind: str
    binary: pathlib.Path
    goblin: bool = False
    transport: str = "resp"
    array_mode: str = "native"
    array_reserved: bool = False


@dataclasses.dataclass
class RunningServer:
    spec: EngineSpec
    process: subprocess.Popen[bytes]
    port: int
    directory: pathlib.Path
    ring: pathlib.Path | None
    log_path: pathlib.Path


class RespError(RuntimeError):
    pass


class RespClient:
    def __init__(self, host: str, port: int, timeout: float = 30.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()

    def close(self) -> None:
        self.sock.close()

    def __enter__(self) -> "RespClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def command(self, *arguments: str) -> Any:
        request = bytearray(f"*{len(arguments)}\r\n".encode())
        for argument in arguments:
            encoded = argument.encode()
            request.extend(f"${len(encoded)}\r\n".encode())
            request.extend(encoded)
            request.extend(b"\r\n")
        self.sock.sendall(request)
        return self._read_value()

    def _receive(self) -> None:
        data = self.sock.recv(65536)
        if not data:
            raise RespError("server closed the connection")
        self.buffer.extend(data)

    def _take(self, count: int) -> bytes:
        while len(self.buffer) < count:
            self._receive()
        result = bytes(self.buffer[:count])
        del self.buffer[:count]
        return result

    def _line(self) -> bytes:
        while True:
            marker = self.buffer.find(b"\r\n")
            if marker >= 0:
                result = bytes(self.buffer[:marker])
                del self.buffer[: marker + 2]
                return result
            self._receive()

    def _read_value(self) -> Any:
        prefix = self._take(1)
        if prefix == b"+":
            return self._line().decode(errors="replace")
        if prefix == b"-":
            raise RespError(self._line().decode(errors="replace"))
        if prefix == b":":
            return int(self._line())
        if prefix == b"$":
            size = int(self._line())
            if size < 0:
                return None
            value = self._take(size)
            if self._take(2) != b"\r\n":
                raise RespError("invalid bulk terminator")
            return value.decode(errors="replace")
        if prefix == b"*":
            count = int(self._line())
            if count < 0:
                return None
            return [self._read_value() for _ in range(count)]
        raise RespError(f"unsupported RESP prefix {prefix!r}")


def parse_engine(value: str) -> EngineSpec:
    fields = value.split(":", 2)
    if len(fields) != 3:
        raise argparse.ArgumentTypeError("engine must be LABEL:KIND:PATH")
    label, kind, binary = fields
    if kind not in {"redis", "dragonfly", "mini-redis-go"}:
        raise argparse.ArgumentTypeError(
            "incumbent KIND must be redis, dragonfly, or mini-redis-go"
        )
    return EngineSpec(label=label, kind=kind, binary=pathlib.Path(binary))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("zset", "set", "array", "all"), default="all")
    parser.add_argument("--worker", type=pathlib.Path, required=True)
    parser.add_argument("--goblin", type=pathlib.Path, required=True)
    parser.add_argument("--engine", action="append", type=parse_engine, default=[])
    parser.add_argument(
        "--parity-config",
        type=pathlib.Path,
        default=ROOT / "benchmarks" / "redis-parity.conf",
    )
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT / "benchmark-results")
    parser.add_argument("--set-report", type=pathlib.Path, default=ROOT / "SET-BENCHMARK.md")
    parser.add_argument("--array-report", type=pathlib.Path, default=ROOT / "ARRAY-BENCHMARK.md")
    parser.add_argument(
        "--array-baseline-json",
        type=pathlib.Path,
        help="retain non-Goblin ARRAY rows from a comparable same-host result",
    )
    parser.add_argument("--zset-report", type=pathlib.Path, default=ROOT / "BENCHMARKS.md")
    parser.add_argument("--zset-members", type=int, default=1_000_000)
    parser.add_argument("--set-members", type=int, default=1_000_000)
    parser.add_argument("--array-members", type=int, default=500_000)
    parser.add_argument("--array-latency-samples", type=int, default=500_000)
    parser.add_argument("--requests", type=int, default=200_000)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--batch", type=int, default=128)
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--value-bytes", type=int, default=16)
    parser.add_argument("--ring-size", default="2mb")
    parser.add_argument("--ring-size-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--server-core", type=int, default=2)
    parser.add_argument("--client-core", type=int, default=3)
    parser.add_argument("--settle-seconds", type=float, default=0.4)
    parser.add_argument("--goblin-arg", action="append", default=[])
    return parser.parse_args()


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def affinity(core: int):
    def apply() -> None:
        if core >= 0 and hasattr(os, "sched_setaffinity"):
            os.sched_setaffinity(0, {core})

    return apply


def server_command(
    spec: EngineSpec,
    port: int,
    directory: pathlib.Path,
    ring: pathlib.Path | None,
    args: argparse.Namespace,
) -> tuple[list[str], dict[str, str]]:
    command = [str(spec.binary)]
    environment = dict(os.environ)
    if spec.goblin:
        command += [
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--client-read-buffer-kib",
            "16",
        ]
        if ring is not None:
            command += ["--ring", str(ring), args.ring_size]
        command += args.goblin_arg
    elif spec.kind == "redis":
        if args.parity_config.exists():
            command.append(str(args.parity_config))
        command += [
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--save",
            "",
            "--appendonly",
            "no",
            "--protected-mode",
            "no",
            "--dir",
            str(directory),
        ]
    elif spec.kind == "dragonfly":
        command += [
            f"--bind=127.0.0.1",
            f"--port={port}",
            "--proactor_threads=1",
            "--maxmemory=0",
            "--dir",
            str(directory),
        ]
    else:
        command += [
            "-bind",
            "127.0.0.1",
            "-port",
            str(port),
            "-appendonly=false",
            "-metrics-addr=",
        ]
        environment["GOMAXPROCS"] = "1"
    return command, environment


@contextlib.contextmanager
def start_server(spec: EngineSpec, args: argparse.Namespace) -> Iterable[RunningServer]:
    temporary = tempfile.TemporaryDirectory(prefix="goblin-collection-bench-")
    directory = pathlib.Path(temporary.name)
    port = free_port()
    ring = directory / "sbe-ring" if spec.transport == "sbe" else None
    log_path = directory / "server.log"
    command, environment = server_command(spec, port, directory, ring, args)
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=environment,
            preexec_fn=affinity(args.server_core),
        )
    running = RunningServer(spec, process, port, directory, ring, log_path)
    try:
        deadline = time.monotonic() + 15.0
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                text = log_path.read_text(errors="replace")
                raise RuntimeError(f"{spec.label} exited during startup: {text[-2000:]}")
            try:
                with RespClient("127.0.0.1", port, 0.5) as client:
                    if client.command("PING") == "PONG":
                        break
            except (OSError, RespError) as error:
                last_error = error
                time.sleep(0.05)
        else:
            raise RuntimeError(f"{spec.label} did not start: {last_error}")
        if ring is not None:
            ring_deadline = time.monotonic() + 10.0
            while not ring.exists() and time.monotonic() < ring_deadline:
                time.sleep(0.02)
            if not ring.exists():
                raise RuntimeError(f"{spec.label} did not create its SBE ring")
        yield running
    finally:
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        temporary.cleanup()


def process_rss_bytes(server: RunningServer) -> int:
    status_path = pathlib.Path(f"/proc/{server.process.pid}/status")
    if server.spec.kind == "mini-redis-go" or not status_path.exists():
        result = subprocess.run(
            ["/bin/ps", "-o", "rss=", "-p", str(server.process.pid)],
            check=True,
            text=True,
            capture_output=True,
        )
        return int(result.stdout.strip()) * 1024
    status = status_path.read_text()
    fields: dict[str, int] = {}
    for line in status.splitlines():
        name, _, rest = line.partition(":")
        if name in {"VmRSS", "HugetlbPages"}:
            fields[name] = int(rest.strip().split()[0]) * 1024
    return fields.get("VmRSS", 0) + fields.get("HugetlbPages", 0)


def process_residency(server: RunningServer) -> dict[str, int] | None:
    status_path = pathlib.Path(f"/proc/{server.process.pid}/status")
    if not status_path.exists():
        return None
    status = status_path.read_text()
    fields: dict[str, int] = {}
    for line in status.splitlines():
        name, _, rest = line.partition(":")
        if name in {"VmLck", "VmSwap"}:
            fields[name] = int(rest.strip().split()[0]) * 1024
    return {
        "locked_bytes": fields.get("VmLck", 0),
        "swap_bytes": fields.get("VmSwap", 0),
    }


def optional_command(server: RunningServer, *command: str) -> Any | None:
    try:
        with RespClient("127.0.0.1", server.port) as client:
            return client.command(*command)
    except (OSError, RespError):
        return None


def info_memory(server: RunningServer) -> dict[str, str]:
    response = optional_command(server, "INFO", "memory")
    if not isinstance(response, str):
        return {}
    result: dict[str, str] = {}
    for line in response.splitlines():
        if line and not line.startswith("#") and ":" in line:
            name, value = line.split(":", 1)
            result[name] = value.rstrip("\r")
    return result


def goblin_memory(server: RunningServer, key: str) -> dict[str, Any]:
    response = optional_command(server, "GOBLIN.MEMORY", key)
    if not isinstance(response, list) or len(response) % 2:
        return {}
    return {str(response[i]): response[i + 1] for i in range(0, len(response), 2)}


def memory_sample(server: RunningServer, key: str, baseline_rss: int) -> dict[str, Any]:
    info = info_memory(server)
    rss = process_rss_bytes(server)
    key_bytes = optional_command(server, "MEMORY", "USAGE", key)
    internal = goblin_memory(server, key) if server.spec.goblin else {}
    if server.spec.goblin:
        key_bytes = internal.get("total_allocated_bytes", key_bytes)
    try:
        used = int(info["used_memory"]) if "used_memory" in info else None
    except ValueError:
        used = None
    return {
        "rss_bytes": rss,
        "rss_delta_bytes": max(0, rss - baseline_rss),
        "used_memory_bytes": used,
        "key_bytes": int(key_bytes) if isinstance(key_bytes, (int, str)) else None,
        "goblin_memory": internal,
    }


def run_worker(
    server: RunningServer,
    args: argparse.Namespace,
    *,
    suite: str,
    action: str,
    key: str,
    members: int,
    requests: int,
    stride: int = 1,
    offset: int = 0,
    second_key: str = "bench:secondary",
    pipeline: int | None = None,
    array_reserve_bytes: int = 0,
) -> dict[str, Any]:
    command = [
        str(args.worker),
        "--suite",
        suite,
        "--action",
        action,
        "--transport",
        server.spec.transport,
        "--host",
        "127.0.0.1",
        "--port",
        str(server.port),
        "--key",
        key,
        "--second-key",
        second_key,
        "--array-mode",
        server.spec.array_mode,
        "--members",
        str(members),
        "--requests",
        str(max(1, requests)),
        "--offset",
        str(offset),
        "--stride",
        str(stride),
        "--value-bytes",
        str(args.value_bytes),
        "--batch",
        str(args.batch),
        "--pipeline",
        str(args.pipeline if pipeline is None else pipeline),
    ]
    if server.ring is not None:
        command += ["--ring", str(server.ring)]
    if array_reserve_bytes:
        command += ["--array-reserve-bytes", str(array_reserve_bytes)]
    result = subprocess.run(
        command,
        text=True,
        capture_output=True,
        timeout=900,
        preexec_fn=affinity(args.client_core),
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    values: dict[str, Any] = {}
    for line in result.stdout.splitlines():
        name, separator, value = line.partition("=")
        if not separator:
            continue
        values[name] = (
            float(value)
            if name == "seconds" or name.startswith("latency_")
            else int(value)
        )
    operations = int(values["logical_operations"])
    seconds = float(values["seconds"])
    values["operations_per_second"] = operations / seconds
    return values


def median_run(
    server: RunningServer,
    args: argparse.Namespace,
    rounds: int,
    **worker_args: Any,
) -> dict[str, Any]:
    samples = [run_worker(server, args, **worker_args) for _ in range(rounds)]
    middle = sorted(samples, key=lambda item: item["operations_per_second"])[len(samples) // 2]
    return {**middle, "round_rates": [item["operations_per_second"] for item in samples]}


def set_specs(args: argparse.Namespace) -> list[EngineSpec]:
    return [
        EngineSpec("goblin-resp-tcp", "goblin", args.goblin, True, "resp"),
        EngineSpec("goblin-sbe-ring", "goblin", args.goblin, True, "sbe"),
        *args.engine,
    ]


def array_specs(args: argparse.Namespace) -> list[EngineSpec]:
    return [
        EngineSpec(
            "goblin-classic-resp-tcp", "goblin", args.goblin, True, "resp", "classic"
        ),
        EngineSpec("goblin-rt-resp-tcp", "goblin", args.goblin, True, "resp", "rt"),
        *args.engine,
    ]


def run_set_suite(args: argparse.Namespace) -> dict[str, Any]:
    engines: list[dict[str, Any]] = []
    operations = {
        "member-hit": args.requests,
        "member-miss": args.requests,
        "multi-member": args.requests,
        "card": args.requests,
        "churn": args.requests // 2,
        "intercard": 20,
    }
    for spec in set_specs(args):
        print(f"[set] {spec.label}", flush=True)
        row: dict[str, Any] = {"label": spec.label, "transport": spec.transport}
        with start_server(spec, args) as server:
            time.sleep(args.settle_seconds)
            baseline_rss = process_rss_bytes(server)
            try:
                load = run_worker(
                    server,
                    args,
                    suite="set",
                    action="load",
                    key="set:primary",
                    members=args.set_members,
                    requests=1,
                )
            except RuntimeError as error:
                row["unsupported"] = str(error)
                engines.append(row)
                continue
            if spec.goblin:
                optimized = optional_command(server, "GOBLIN.OPTIMIZE", "set:primary", "0.97")
                row["optimized"] = optimized is not None
            time.sleep(args.settle_seconds)
            row["population"] = load
            row["memory"] = memory_sample(server, "set:primary", baseline_rss)
            run_worker(
                server,
                args,
                suite="set",
                action="load",
                key="set:secondary",
                members=args.set_members,
                requests=1,
                offset=args.set_members // 2,
            )
            row["operations"] = {}
            for action, requests in operations.items():
                try:
                    row["operations"][action] = median_run(
                        server,
                        args,
                        args.rounds,
                        suite="set",
                        action=action,
                        key="set:primary",
                        second_key="set:secondary",
                        members=args.set_members,
                        requests=requests,
                    )
                except RuntimeError as error:
                    row["operations"][action] = {"unsupported": str(error)}
            row["final_cardinality"] = optional_command(server, "SCARD", "set:primary")
        engines.append(row)
    return {
        "suite": "set",
        "members": args.set_members,
        "value_bytes": args.value_bytes,
        "batch": args.batch,
        "pipeline": args.pipeline,
        "requests": args.requests,
        "rounds": args.rounds,
        "ring_size_bytes": args.ring_size_bytes,
        "engines": engines,
    }


def run_zset_suite(args: argparse.Namespace) -> dict[str, Any]:
    engines: list[dict[str, Any]] = []
    operations = {
        "score-hit": args.requests,
        "rank": args.requests,
        "range-16": args.requests,
        "score-range-16": args.requests,
        "count-all": args.requests,
        "card": args.requests,
        "update": args.requests,
        "churn": args.requests // 2,
    }
    for spec in set_specs(args):
        print(f"[zset] {spec.label}", flush=True)
        row: dict[str, Any] = {"label": spec.label, "transport": spec.transport}
        with start_server(spec, args) as server:
            time.sleep(args.settle_seconds)
            baseline_rss = process_rss_bytes(server)
            try:
                load = run_worker(
                    server,
                    args,
                    suite="zset",
                    action="load",
                    key="zset:primary",
                    members=args.zset_members,
                    requests=1,
                )
            except RuntimeError as error:
                row["unsupported"] = str(error)
                engines.append(row)
                continue
            if spec.goblin:
                optimized = optional_command(server, "GOBLIN.OPTIMIZE", "zset:primary", "0.97")
                row["optimized"] = optimized is not None
            time.sleep(args.settle_seconds)
            row["population"] = load
            row["memory"] = memory_sample(server, "zset:primary", baseline_rss)
            row["operations"] = {}
            for action, requests in operations.items():
                try:
                    row["operations"][action] = median_run(
                        server,
                        args,
                        args.rounds,
                        suite="zset",
                        action=action,
                        key="zset:primary",
                        members=args.zset_members,
                        requests=requests,
                    )
                except RuntimeError as error:
                    row["operations"][action] = {"unsupported": str(error)}
            row["final_cardinality"] = optional_command(server, "ZCARD", "zset:primary")
        engines.append(row)
    return {
        "suite": "zset",
        "members": args.zset_members,
        "value_bytes": args.value_bytes,
        "batch": args.batch,
        "pipeline": args.pipeline,
        "requests": args.requests,
        "rounds": args.rounds,
        "ring_size_bytes": args.ring_size_bytes,
        "engines": engines,
    }


def array_reserve_bytes(value_writes: int, encoded_value_bytes: int) -> int:
    logical_bytes = value_writes * encoded_value_bytes
    # Array chunks are at least 64 KiB. Allow for the worst fixed-value padding
    # at every minimum-sized chunk boundary without materially over-reserving.
    return logical_bytes + (
        logical_bytes // (64 * 1024) + 1
    ) * (encoded_value_bytes - 1)


def reserve_array(
    server: RunningServer,
    key: str,
    max_index: int,
    value_slots: int,
    encoded_bytes: int,
) -> dict[str, Any]:
    started = time.monotonic()
    with RespClient("127.0.0.1", server.port) as client:
        reply = client.command(
            "GOBLIN.RT.ARRESERVE",
            key,
            str(max_index),
            str(value_slots),
            str(encoded_bytes),
        )
    seconds = time.monotonic() - started
    if reply != 1:
        raise RuntimeError("GOBLIN.RT.ARRESERVE returned an unexpected reply")
    return {
        "seconds": seconds,
        "max_index": max_index,
        "value_slots": value_slots,
        "encoded_bytes": encoded_bytes,
    }


def run_array_suite(args: argparse.Namespace) -> dict[str, Any]:
    increment_latency: list[dict[str, Any]] = []
    latency_specs: list[tuple[EngineSpec, int]] = []
    encoded_value_bytes = args.value_bytes + 1
    reserved_arena_bytes = array_reserve_bytes(
        args.array_latency_samples, encoded_value_bytes
    )
    for spec in array_specs(args):
        latency_specs.append((spec, 0))
        if spec.goblin and spec.array_mode == "rt":
            latency_specs.append(
                (
                    EngineSpec(
                        "goblin-rt-reserved-resp-tcp",
                        spec.kind,
                        spec.binary,
                        True,
                        "resp",
                        "rt",
                        True,
                    ),
                    reserved_arena_bytes,
                )
            )
    for spec, reserve_bytes in latency_specs:
        print(f"[array/increment-latency] {spec.label}", flush=True)
        row: dict[str, Any] = {
            "label": spec.label,
            "mode": spec.array_mode,
            "transport": "resp",
        }
        with start_server(spec, args) as server:
            try:
                row["latency"] = run_worker(
                    server,
                    args,
                    suite="array",
                    action="increment-latency",
                    key="array:increment-latency",
                    members=args.array_latency_samples,
                    requests=args.array_latency_samples,
                    pipeline=1,
                    array_reserve_bytes=reserve_bytes,
                )
            except RuntimeError as error:
                row["unsupported"] = str(error)
            row["process_residency"] = process_residency(server)
        increment_latency.append(row)

    scenarios: list[dict[str, Any]] = []
    operations = {
        "get-hit": args.requests,
        "get-miss": args.requests,
        "multi-get": args.requests,
        "count": args.requests,
        "length": args.requests,
        "update": args.requests,
        "churn": args.requests // 2,
        "insert": args.requests // 2,
    }
    for scenario_name, stride in (("dense", 1), ("sparse-stride-16", 16)):
        scenario_rows: list[dict[str, Any]] = []
        scenario_specs: list[EngineSpec] = []
        for spec in array_specs(args):
            scenario_specs.append(spec)
            if spec.goblin and spec.array_mode == "rt":
                scenario_specs.append(
                    EngineSpec(
                        "goblin-rt-reserved-resp-tcp",
                        spec.kind,
                        spec.binary,
                        True,
                        "resp",
                        "rt",
                        True,
                    )
                )
        for spec in scenario_specs:
            print(f"[array/{scenario_name}] {spec.label}", flush=True)
            row: dict[str, Any] = {
                "label": spec.label,
                "mode": spec.array_mode,
                "transport": "resp",
            }
            with start_server(spec, args) as server:
                time.sleep(args.settle_seconds)
                baseline_rss = process_rss_bytes(server)
                if spec.array_reserved:
                    insert_writes = args.rounds * (args.requests // 2)
                    max_index = max(
                        (args.array_members - 1) * stride,
                        max(0, insert_writes - 1),
                    )
                    value_slots = args.array_members + insert_writes
                    value_writes = args.array_members + args.rounds * (
                        args.requests + (args.requests // 2) + (args.requests // 2)
                    )
                    row["reservation"] = reserve_array(
                        server,
                        "array:primary",
                        max_index,
                        value_slots,
                        array_reserve_bytes(value_writes, encoded_value_bytes),
                    )
                try:
                    load = run_worker(
                        server,
                        args,
                        suite="array",
                        action="load",
                        key="array:primary",
                        members=args.array_members,
                        requests=1,
                        stride=stride,
                    )
                except RuntimeError as error:
                    row["unsupported"] = str(error)
                    scenario_rows.append(row)
                    continue
                time.sleep(args.settle_seconds)
                row["population"] = load
                row["memory"] = memory_sample(server, "array:primary", baseline_rss)
                row["process_residency"] = process_residency(server)
                row["operations"] = {}
                for action, requests in operations.items():
                    try:
                        row["operations"][action] = median_run(
                            server,
                            args,
                            args.rounds,
                            suite="array",
                            action=action,
                            key="array:primary",
                            members=args.array_members,
                            requests=requests,
                            stride=stride,
                        )
                    except RuntimeError as error:
                        row["operations"][action] = {"unsupported": str(error)}
                if spec.goblin:
                    row["length"] = optional_command(
                        server,
                        "GOBLIN.RT.ARLEN" if spec.array_mode == "rt" else "GOBLIN.CLASSIC.ARLEN",
                        "array:primary",
                    )
                    row["final_count"] = optional_command(
                        server,
                        "GOBLIN.RT.ARCOUNT" if spec.array_mode == "rt" else "GOBLIN.CLASSIC.ARCOUNT",
                        "array:primary",
                    )
                else:
                    row["final_count"] = optional_command(server, "ARCOUNT", "array:primary")
            scenario_rows.append(row)
        scenarios.append({"name": scenario_name, "stride": stride, "engines": scenario_rows})
    return {
        "suite": "array",
        "members": args.array_members,
        "value_bytes": args.value_bytes,
        "batch": args.batch,
        "pipeline": args.pipeline,
        "requests": args.requests,
        "rounds": args.rounds,
        "increment_latency": {
            "samples": args.array_latency_samples,
            "pipeline": 1,
            "engines": increment_latency,
        },
        "scenarios": scenarios,
    }


def merge_array_incumbents(current: dict[str, Any], baseline: dict[str, Any]) -> None:
    comparable_fields = (
        "suite",
        "members",
        "value_bytes",
        "batch",
        "pipeline",
        "requests",
        "rounds",
    )
    mismatches = [
        name for name in comparable_fields if current.get(name) != baseline.get(name)
    ]
    if mismatches:
        joined = ", ".join(mismatches)
        raise RuntimeError(f"ARRAY baseline is not comparable: {joined}")

    def incumbent_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
        return [row for row in rows if not row["label"].startswith("goblin-")]

    current["increment_latency"]["engines"].extend(
        incumbent_rows(baseline["increment_latency"]["engines"])
    )
    baseline_scenarios = {
        scenario["name"]: scenario for scenario in baseline["scenarios"]
    }
    for scenario in current["scenarios"]:
        try:
            old = baseline_scenarios[scenario["name"]]
        except KeyError as error:
            raise RuntimeError(
                f"ARRAY baseline lacks scenario {scenario['name']!r}"
            ) from error
        if scenario["stride"] != old["stride"]:
            raise RuntimeError(
                f"ARRAY baseline stride differs for {scenario['name']!r}"
            )
        scenario["engines"].extend(incumbent_rows(old["engines"]))

    current["goblin_generated_at"] = current["generated_at"]
    current["incumbent_generated_at"] = baseline.get("generated_at", "unknown")


def fmt_rate(value: float | None) -> str:
    if value is None:
        return "n/a"
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f}M"
    if value >= 1_000:
        return f"{value / 1_000:.0f}K"
    return f"{value:.0f}"


def fmt_mib(value: int | None) -> str:
    return "n/a" if value is None else f"{value / (1024 * 1024):.2f}"


def op_rate(row: dict[str, Any], operation: str) -> float | None:
    value = row.get("operations", {}).get(operation, {})
    return value.get("operations_per_second") if "unsupported" not in value else None


def array_rate_cell(row: dict[str, Any], operation: str) -> str:
    value = row.get("operations", {}).get(operation)
    if not value or "unsupported" in value:
        return "X"
    return fmt_rate(value.get("operations_per_second"))


def array_latency_cell(row: dict[str, Any], percentile: str) -> str:
    latency = row.get("latency")
    if not latency:
        return "X"
    return f"{latency[percentile]:.2f}"


def best_incumbent(rows: list[dict[str, Any]], operation: str) -> float | None:
    values = [
        value
        for row in rows
        if not row["label"].startswith("goblin")
        if "operations" in row
        if (value := op_rate(row, operation)) is not None
    ]
    return max(values) if values else None


def report_preamble(title: str, generated: str) -> list[str]:
    return [
        f"# {title}",
        "",
        f"Generated on a quiet dedicated benchmark host at {generated}.",
        "",
    ]


def set_report(data: dict[str, Any], generated: str) -> str:
    rows = data["engines"]
    goblin_tcp = next(row for row in rows if row["label"] == "goblin-resp-tcp")
    goblin_ring = next(row for row in rows if row["label"] == "goblin-sbe-ring")
    fastest_incumbent = best_incumbent(rows, "member-hit")
    tcp_hit = op_rate(goblin_tcp, "member-hit") or 0.0
    ring_hit = op_rate(goblin_ring, "member-hit") or 0.0
    leanest_incumbent = min(
        row["memory"]["rss_delta_bytes"] / data["members"]
        for row in rows
        if not row["label"].startswith("goblin")
        if "memory" in row
    )
    goblin_memory = goblin_tcp["memory"]["rss_delta_bytes"] / data["members"]
    summary = (
        f"Goblin's RESP/TCP `SISMEMBER` reaches `{fmt_rate(tcp_hit)}` operations/s"
        + (f", `{tcp_hit / fastest_incumbent:.2f}x` the fastest incumbent" if fastest_incumbent else "")
        + f"; SBE over `{data['ring_size_bytes'] // (1024 * 1024)}` MiB shared-memory rings per direction reaches "
        f"`{fmt_rate(ring_hit)}` operations/s. The populated `{data['members']:,}`-member set costs "
        f"`{goblin_memory:.2f}` RSS-delta bytes/member over RESP versus "
        f"`{leanest_incumbent:.2f}` for the leanest incumbent."
    )
    out = report_preamble("Goblin Core SET Benchmark", generated)
    out += ["## Summary", "", summary, "", "## Method", ""]
    out += [
        f"- `{data['members']:,}` distinct `{data['value_bytes']}`-byte binary-safe members in one set.",
        f"- Population uses `SADD` batches of `{data['batch']}`; all timed rows use pipeline depth `{data['pipeline']}`.",
        "- Goblin runs `GOBLIN.OPTIMIZE key 0.97` after population and before memory/operation measurements.",
        f"- Goblin Core is measured twice: RESP/TCP and typed SBE over `{data['ring_size_bytes'] // (1024 * 1024)}` MiB request and reply rings. Every incumbent uses RESP/TCP.",
        f"- Fixed command rows use the median of `{data['rounds']}` native C++ client rounds. Compound churn counts one `SREM` + `SADD` pair as one logical operation.",
        "- The intersection row uses two equally sized sets with 50% overlap and returns only cardinality.",
        "- Servers run one at a time on one serving core; Dragonfly uses one proactor and mini-redis-go uses `GOMAXPROCS=1`.",
        "- Unsupported command cells are `n/a`; error replies are not timed.",
        "- RSS is read from the launched PID. mini-redis-go uses `ps -o rss=`; other engines use `/proc/<pid>/status` as `VmRSS + HugetlbPages`. No server-reported RSS field is trusted.",
        "- Redis-family servers are exercised as black boxes; their implementation source is not inspected.",
        "",
        "## Population And Memory",
        "",
        "| Engine | Transport | members/s | RSS MiB | RSS delta MiB | RSS delta B/member | key-reported B/member |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        if "memory" not in row:
            out.append(
                f"| `{row['label']}` | `{row['transport']}` | n/a | n/a | n/a | n/a | n/a |"
            )
            continue
        memory = row["memory"]
        key_bytes = memory["key_bytes"]
        out.append(
            f"| `{row['label']}` | `{row['transport']}` | {fmt_rate(data['members'] / row['population']['seconds'])} | "
            f"{fmt_mib(memory['rss_bytes'])} | {fmt_mib(memory['rss_delta_bytes'])} | "
            f"{memory['rss_delta_bytes'] / data['members']:.2f} | "
            f"{('n/a' if key_bytes is None else f'{key_bytes / data['members']:.2f}')} |"
        )
    operation_labels = [
        ("member-hit", "SISMEMBER hit"),
        ("member-miss", "SISMEMBER miss"),
        ("multi-member", "SMISMEMBER four hits"),
        ("card", "SCARD"),
        ("churn", "SREM + SADD"),
        ("intercard", "SINTERCARD, 50% overlap"),
    ]
    out += ["", "## Operations", "", "Logical operations per second.", ""]
    out.append("| Operation | " + " | ".join(f"`{row['label']}`" for row in rows) + " |")
    out.append("| --- | " + " | ".join("---:" for _ in rows) + " |")
    for operation, label in operation_labels:
        out.append(
            f"| `{label}` | "
            + " | ".join(fmt_rate(op_rate(row, operation)) for row in rows)
            + " |"
        )
    out += ["", "## Tested Servers", ""]
    out += [f"- `{row['label']}`" for row in rows]
    return "\n".join(out) + "\n"


def zset_report(data: dict[str, Any], generated: str) -> str:
    rows = data["engines"]
    goblin_tcp = next(row for row in rows if row["label"] == "goblin-resp-tcp")
    goblin_ring = next(row for row in rows if row["label"] == "goblin-sbe-ring")
    fastest_incumbent = best_incumbent(rows, "update")
    tcp_update = op_rate(goblin_tcp, "update") or 0.0
    ring_update = op_rate(goblin_ring, "update") or 0.0
    leanest_incumbent = min(
        row["memory"]["rss_delta_bytes"] / data["members"]
        for row in rows
        if not row["label"].startswith("goblin")
        if "memory" in row
    )
    goblin_bytes = goblin_tcp["memory"]["rss_delta_bytes"] / data["members"]
    out = report_preamble("Goblin Core ZSET Benchmark", generated)
    out += [
        "## Summary",
        "",
        f"After loading and packing `{data['members']:,}` members, Goblin Core uses `{goblin_bytes:.2f}` RSS-delta bytes/member versus `{leanest_incumbent:.2f}` for the leanest incumbent. "
        f"RESP/TCP same-member `ZADD` reaches `{fmt_rate(tcp_update)}` operations/s"
        + (f", `{tcp_update / fastest_incumbent:.2f}x` the fastest incumbent" if fastest_incumbent else "")
        + f"; typed SBE over `{data['ring_size_bytes'] // (1024 * 1024)}` MiB request and reply rings reaches `{fmt_rate(ring_update)}` operations/s.",
        "",
        "## Method",
        "",
        f"- `{data['members']:,}` distinct `{data['value_bytes']}`-byte members in one sorted set, with deterministic scattered unsigned 32-bit integer scores.",
        f"- Population uses `ZADD` batches of `{data['batch']}`. Every timed operation uses pipeline depth `{data['pipeline']}`.",
        "- Goblin runs `GOBLIN.OPTIMIZE key 0.97` after population and before memory/operation measurements.",
        f"- Goblin Core is measured over RESP/TCP and typed SBE over `{data['ring_size_bytes'] // (1024 * 1024)}` MiB request and reply rings; every incumbent uses RESP/TCP.",
        f"- Fixed rows are medians of `{data['rounds']}` native C++ client rounds. `ZREM + ZADD` counts as one logical churn operation.",
        "- One serving core per engine. Dragonfly uses one proactor; mini-redis-go uses `GOMAXPROCS=1`.",
        "- A server without sorted-set support remains in the matrix with `n/a` cells; error replies are not timed.",
        "- RSS comes from the launched server PID (`ps` for mini-redis-go, `/proc` for the others). Server-reported RSS is not used.",
        "- Incumbent implementations are black boxes; no incompatible source is inspected.",
        "",
        "## Population And Memory",
        "",
        "| Engine | Transport | members/s | RSS MiB | RSS delta MiB | RSS delta B/member | key-reported B/member |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in rows:
        if "memory" not in row:
            out.append(
                f"| `{row['label']}` | `{row['transport']}` | n/a | n/a | n/a | n/a | n/a |"
            )
            continue
        memory = row["memory"]
        key_bytes = memory["key_bytes"]
        out.append(
            f"| `{row['label']}` | `{row['transport']}` | {fmt_rate(data['members'] / row['population']['seconds'])} | "
            f"{fmt_mib(memory['rss_bytes'])} | {fmt_mib(memory['rss_delta_bytes'])} | "
            f"{memory['rss_delta_bytes'] / data['members']:.2f} | "
            f"{('n/a' if key_bytes is None else f'{key_bytes / data['members']:.2f}')} |"
        )
    operation_labels = [
        ("update", "ZADD existing member"),
        ("score-hit", "ZSCORE hit"),
        ("rank", "ZRANK"),
        ("range-16", "ZRANGE first 16"),
        ("score-range-16", "ZRANGE BYSCORE, LIMIT 16"),
        ("count-all", "ZCOUNT full bounds"),
        ("card", "ZCARD"),
        ("churn", "ZREM + ZADD"),
    ]
    out += ["", "## Operations", "", "Logical operations per second.", ""]
    out.append("| Operation | " + " | ".join(f"`{row['label']}`" for row in rows) + " |")
    out.append("| --- | " + " | ".join("---:" for _ in rows) + " |")
    for operation, label in operation_labels:
        out.append(
            f"| `{label}` | "
            + " | ".join(fmt_rate(op_rate(row, operation)) for row in rows)
            + " |"
        )
    out += [
        "",
        "## Interpretation",
        "",
        "The RESP rows isolate the sorted-set implementation under the same wire protocol. The ring row answers a different deployment question: how much transport and parsing work remains when a native client submits the same pipelined operations as typed SBE messages without a syscall or ASCII conversion. It is reported alongside, never substituted for the protocol-parity result.",
        "",
        "The scattered score generator crosses signed 32-bit range, so Goblin stores this population at `f64` score width. Workloads whose scores fit `i16` or signed `i32` can use less memory.",
        "",
        "## Tested Servers",
        "",
    ]
    out += [f"- `{row['label']}`" for row in rows]
    return "\n".join(out) + "\n"


def array_report(data: dict[str, Any], generated: str) -> str:
    dense = next(s for s in data["scenarios"] if s["name"] == "dense")
    sparse = next(s for s in data["scenarios"] if s["name"] == "sparse-stride-16")
    classic = next(row for row in sparse["engines"] if row["label"].startswith("goblin-classic"))
    redis_8_sparse = next(
        (row for row in sparse["engines"] if row["label"] == "redis-8.8"), None
    )
    latency_rows = data["increment_latency"]["engines"]
    reserved_latency = next(
        row
        for row in latency_rows
        if row["label"] == "goblin-rt-reserved-resp-tcp"
    )
    redis_8_latency = next(
        (row for row in latency_rows if row["label"] == "redis-8.8"), None
    )
    highlights = []
    if redis_8_latency is not None and "latency" in redis_8_latency:
        highlights.append(
            f"resident-locked reserved RT records "
            f"`{reserved_latency['latency']['latency_p9999_us']:.2f}` us P99.99 "
            f"versus `{redis_8_latency['latency']['latency_p9999_us']:.2f}` us for Redis 8.8"
        )
    if redis_8_sparse is not None and "memory" in redis_8_sparse:
        highlights.append(
            f"Classic uses "
            f"`{classic['memory']['rss_delta_bytes'] / data['members']:.2f}` RSS-delta bytes/value "
            f"on the sparse shape versus "
            f"`{redis_8_sparse['memory']['rss_delta_bytes'] / data['members']:.2f}` for Redis 8.8"
        )
    highlight_summary = (
        "Two results stand out: " + "; and ".join(highlights) + "."
        if highlights
        else "The tables report the resulting throughput, memory, and latency without substituting unsupported commands."
    )
    out = report_preamble("Goblin Core ARRAY Benchmark", generated)
    out += [
        "## Summary",
        "",
        f"This report compares Goblin Core's Classic and real-time ARRAY implementations with the Redis-compatible servers in the benchmark suite, using only native `AR*` commands over RESP/TCP. It loads `{data['members']:,}` `{data['value_bytes']}`-byte values into dense and stride-16 sparse arrays, measures population throughput and process RSS, then exercises eight read and mutation workflows at pipeline depth `{data['pipeline']}`. A separate `{data['increment_latency']['samples']:,}`-append, pipeline-1 test reports P90 through P99.99 latency. Engines without native ARRAY commands are marked `X`; hashes are never used as a fallback.",
        "",
        "Classic is the memory-oriented implementation for sparse or evolving index ranges. RT trades fixed dense leaves and additional capacity for a more predictable mutation path; its reserved row provisions and prefaults hard index, metadata, and byte limits before timing. "
        + highlight_summary,
        "",
        "## Method",
        "",
        f"- `{data['members']:,}` logical `{data['value_bytes']}`-byte values in both dense (`stride=1`) and sparse (`stride=16`) shapes.",
        f"- Population and throughput rows use RESP/TCP pipeline depth `{data['pipeline']}`; individual-increment latency uses pipeline depth `1`.",
        "- Goblin Classic uses adaptive sparse/dense leaves and a growable directory. Goblin RT uses fixed-size dense leaves, a fixed-depth directory, and a separate 2x value-arena growth policy.",
        "- Each reserved RT population row provisions its complete index range, enough value slots for the population plus every insert round, and enough encoded bytes for population, update, churn, and insert writes across all rounds. Reservation time is reported separately and excluded from values/s; RSS delta includes the complete reservation.",
        "- Every server receives native `ARSET`, `ARMSET`, `ARGET`, `ARMGET`, `ARCOUNT`, `ARLEN`, `ARDEL`, and `ARINSERT` requests. There is no hash-command fallback.",
        f"- Operation rates are medians of `{data['rounds']}` native C++ client rounds. Churn counts one delete + restore pair as one logical operation.",
        "- Servers run one at a time on one serving core; Dragonfly uses one proactor and mini-redis-go uses `GOMAXPROCS=1`.",
        "- `X` means the server returned a command error for the required AR operation. Error replies are not timed.",
        "- RSS is sampled from the launched PID (`/proc` on Linux; `ps` for mini-redis-go and as the non-Linux fallback), never from a server-reported RSS field.",
        "- Incumbents are exercised strictly through RESP as black boxes; their source is not inspected.",
    ]
    if data.get("incumbent_generated_at"):
        out.append(
            f"- Goblin rows were refreshed at `{data['goblin_generated_at']}`; unchanged incumbent rows are retained from the same-host `{data['incumbent_generated_at']}` run."
        )
    latency = data["increment_latency"]
    p9999_tail_samples = max(1, (latency["samples"] + 9_999) // 10_000)
    residency = []
    for row in latency["engines"]:
        process_status = row.get("process_residency")
        if process_status is not None and row["label"].startswith("goblin-"):
            residency.append(
                f"`{row['label']}` {fmt_mib(process_status['locked_bytes'])} MiB locked / "
                f"{fmt_mib(process_status['swap_bytes'])} MiB swapped"
            )
    residency_summary = (
        "Immediately after timing, `/proc/<pid>/status` reported "
        + "; ".join(residency)
        + ". This verifies the benchmarked mappings were resident-locked and not swapped."
        if residency
        else "Resident-lock verification is unavailable on this non-Linux host because it has no `/proc/<pid>/status`."
    )
    out += [
        "",
        "## Individual Increment Latency",
        "",
        f"Each of the `{latency['samples']:,}` samples is one dense `ARSET key index value` append that advances a fresh array's logical length by one. Values are `{data['value_bytes']}` bytes, indexes are sequential, connection setup is excluded, and RESP/TCP pipeline depth is `{latency['pipeline']}`. At this sample count, the P99.99 tail contains approximately `{p9999_tail_samples}` commands.",
        "The `goblin-rt-reserved` row issues `GOBLIN.RT.ARRESERVE` before timing, prefaulting its complete index, value-slot metadata, and encoded-byte budget. Reservation time and connection setup are excluded; exhaustion would fail instead of allocating in a measured command.",
        residency_summary,
        "",
        "| Engine | P90 (us) | P99 (us) | P99.9 (us) | P99.99 (us) |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for row in latency["engines"]:
        out.append(
            f"| `{row['label']}` | {array_latency_cell(row, 'latency_p90_us')} | "
            f"{array_latency_cell(row, 'latency_p99_us')} | "
            f"{array_latency_cell(row, 'latency_p999_us')} | "
            f"{array_latency_cell(row, 'latency_p9999_us')} |"
        )
    operation_labels = [
        ("get-hit", "point read hit"),
        ("get-miss", "point read miss"),
        ("multi-get", "four-index read"),
        ("count", "populated count"),
        ("length", "logical length (`ARLEN`)"),
        ("update", "same-size update"),
        ("churn", "delete + restore"),
        ("insert", "allocate next index (`ARINSERT`)"),
    ]
    for scenario in data["scenarios"]:
        rows = scenario["engines"]
        title = "Dense Indexes" if scenario["stride"] == 1 else "Sparse Indexes, Stride 16"
        out += ["", f"## {title}", "", "### Population And Memory", ""]
        out += [
            "| Engine | representation | reserve ms | values/s | RSS MiB | RSS delta MiB | RSS delta B/value | key-reported B/value |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
        for row in rows:
            representation = (
                "Classic AR*"
                if row["mode"] == "classic"
                else "RT AR* reserved"
                if row["label"] == "goblin-rt-reserved-resp-tcp"
                else "RT AR*" if row["mode"] == "rt" else "native AR*"
            )
            if "memory" not in row:
                out.append(
                    f"| `{row['label']}` | {representation} | X | X | X | X | X | X |"
                )
                continue
            memory = row["memory"]
            key_bytes = memory["key_bytes"]
            reserve_ms = (
                f"{row['reservation']['seconds'] * 1000:.2f}"
                if "reservation" in row
                else "n/a"
            )
            out.append(
                f"| `{row['label']}` | {representation} | {reserve_ms} | {fmt_rate(data['members'] / row['population']['seconds'])} | "
                f"{fmt_mib(memory['rss_bytes'])} | {fmt_mib(memory['rss_delta_bytes'])} | "
                f"{memory['rss_delta_bytes'] / data['members']:.2f} | "
                f"{('n/a' if key_bytes is None else f'{key_bytes / data['members']:.2f}')} |"
            )
        out += ["", "### Operations", ""]
        out.append("| Operation | " + " | ".join(f"`{row['label']}`" for row in rows) + " |")
        out.append("| --- | " + " | ".join("---:" for _ in rows) + " |")
        for operation, label in operation_labels:
            out.append(
                f"| {label} | "
                + " | ".join(array_rate_cell(row, operation) for row in rows)
                + " |"
            )
    out += [
        "",
        "## Interpretation",
        "",
        "Classic and RT solve different deployment problems. Classic is the memory-oriented choice for sparse or evolving index ranges. RT fixes each touched leaf at its final size and never rebuilds its fixed-depth directory; an out-of-range write fails. Its ordinary value arena grows 2x to reduce relocations. `GOBLIN.RT.ARRESERVE` goes further by allocating and prefaulting the declared index, metadata, and byte capacity before serving, then failing closed at any bound instead of allocating from a timed mutation.",
        "",
        "All comparison rows exercise native AR commands. A row marked `X` does not expose the required command; the harness never substitutes hashes or times an error reply as useful work.",
    ]
    return "\n".join(out) + "\n"


def main() -> int:
    args = parse_args()
    if not args.worker.exists() or not args.goblin.exists():
        raise SystemExit("--worker and --goblin must name existing executables")
    for engine in args.engine:
        if not engine.binary.exists():
            raise SystemExit(f"engine binary does not exist: {engine.binary}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    generated = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    rss_source = (
        "ps for mini-redis-go; /proc VmRSS + HugetlbPages otherwise"
        if pathlib.Path("/proc/self/status").exists()
        else "ps RSS from the launched server PID"
    )
    metadata = {
        "generated_at": generated,
        "rss_source": rss_source,
        "server_core": args.server_core,
        "client_core": args.client_core,
    }
    if args.suite in {"zset", "all"}:
        result = {**metadata, **run_zset_suite(args)}
        (args.output_dir / "zset.json").write_text(json.dumps(result, indent=2) + "\n")
        args.zset_report.write_text(zset_report(result, generated))
        print(f"wrote {args.zset_report}")
    if args.suite in {"set", "all"}:
        result = {**metadata, **run_set_suite(args)}
        (args.output_dir / "set.json").write_text(json.dumps(result, indent=2) + "\n")
        args.set_report.write_text(set_report(result, generated))
        print(f"wrote {args.set_report}")
    if args.suite in {"array", "all"}:
        result = {**metadata, **run_array_suite(args)}
        if args.array_baseline_json is not None:
            if args.engine:
                raise RuntimeError(
                    "--array-baseline-json cannot be combined with --engine"
                )
            baseline = json.loads(args.array_baseline_json.read_text())
            merge_array_incumbents(result, baseline)
        (args.output_dir / "array.json").write_text(json.dumps(result, indent=2) + "\n")
        args.array_report.write_text(array_report(result, generated))
        print(f"wrote {args.array_report}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
