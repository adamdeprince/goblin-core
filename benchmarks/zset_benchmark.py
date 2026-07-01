#!/usr/bin/env python3
"""Benchmark Goblin Core zset operations against Redis over RESP.

The harness starts each server on a temporary localhost port, drives the same
workload through a small RESP client, and records throughput plus RSS memory.
Redis also reports `INFO memory.used_memory` when available.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


def encode_command(parts: Sequence[object]) -> bytes:
    out = bytearray()
    out.extend(f"*{len(parts)}\r\n".encode())
    for part in parts:
        if isinstance(part, bytes):
            data = part
        else:
            data = str(part).encode()
        out.extend(f"${len(data)}\r\n".encode())
        out.extend(data)
        out.extend(b"\r\n")
    return bytes(out)


class RespClient:
    def __init__(self, host: str, port: int, timeout: float = 10.0) -> None:
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()

    def close(self) -> None:
        self.sock.close()

    def command(self, *parts: object) -> object:
        self.sock.sendall(encode_command(parts))
        return self.read_response()

    def pipeline(self, commands: Iterable[Sequence[object]], flush_every: int) -> int:
        pending = 0
        sent = 0
        batch = bytearray()
        for command in commands:
            batch.extend(encode_command(command))
            pending += 1
            sent += 1
            if pending >= flush_every:
                self.sock.sendall(batch)
                batch.clear()
                for _ in range(pending):
                    self.read_response()
                pending = 0

        if pending:
            self.sock.sendall(batch)
            for _ in range(pending):
                self.read_response()

        return sent

    def read_response(self) -> object:
        prefix = self._read_exact(1)
        if prefix == b"+":
            return self._read_line().decode()
        if prefix == b"-":
            message = self._read_line().decode(errors="replace")
            raise RuntimeError(f"server returned error: {message}")
        if prefix == b":":
            return int(self._read_line())
        if prefix == b"$":
            length = int(self._read_line())
            if length == -1:
                return None
            data = self._read_exact(length)
            crlf = self._read_exact(2)
            if crlf != b"\r\n":
                raise RuntimeError("invalid RESP bulk terminator")
            return data
        if prefix == b"*":
            count = int(self._read_line())
            if count == -1:
                return None
            return [self.read_response() for _ in range(count)]
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
        data = self.sock.recv(65536)
        if not data:
            raise RuntimeError("connection closed while reading RESP response")
        self.buffer.extend(data)


@dataclass
class ServerProcess:
    name: str
    process: subprocess.Popen[bytes]
    port: int
    temp_dir: Path | None = None

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.temp_dir is not None:
            shutil.rmtree(self.temp_dir, ignore_errors=True)


@dataclass
class BenchmarkResult:
    target: str
    metric: str
    score_shape: str
    range_size: int
    count: int
    seconds: float
    per_second: float
    rss_baseline_mib: float
    rss_after_load_mib: float
    rss_delta_mib: float
    rss_delta_bytes_per_member: float
    rss_final_mib: float
    redis_used_memory_mib: float | None
    redis_used_bytes_per_member: float | None
    redis_final_used_memory_mib: float | None
    goblin_member_storage_allocated_mib: float | None
    goblin_member_index_allocated_mib: float | None
    goblin_score_index_allocated_mib: float | None
    goblin_score_string_cache_allocated_mib: float | None
    goblin_rank_location_cache_allocated_mib: float | None
    goblin_total_allocated_mib: float | None
    latency_min_us: float | None = None
    latency_p50_us: float | None = None
    latency_p95_us: float | None = None
    latency_p99_us: float | None = None
    latency_max_us: float | None = None


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_server(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            client = RespClient("127.0.0.1", port, timeout=1.0)
            try:
                if client.command("PING") in ("PONG", b"PONG"):
                    return
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - collect startup failure context.
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"server on port {port} did not become ready: {last_error}")


def resolve_executable(path: Path, label: str) -> Path:
    if path.exists():
        return path
    resolved = shutil.which(str(path))
    if resolved is not None:
        return Path(resolved)
    raise FileNotFoundError(f"{label} not found: {path}")


def start_goblin(binary: Path,
                 rank_cache: bool,
                 rank_cache_mode: str | None = None,
                 max_output_buffer_mib: int | None = None,
                 score_string_cache: bool = False,
                 initial_output_buffer_kib: int | None = None) -> ServerProcess:
    binary = resolve_executable(binary, "Goblin Core binary")
    port = free_port()
    if rank_cache_mode is None:
        rank_cache_mode = "exact" if rank_cache else "off"
    command = [str(binary), "--port", str(port)]
    if rank_cache_mode != "off":
        command.extend(["--rank-cache-mode", rank_cache_mode])
    if score_string_cache:
        command.append("--score-string-cache")
    if max_output_buffer_mib is not None:
        command.extend(["--max-output-buffer-mib", str(max_output_buffer_mib)])
    if initial_output_buffer_kib is not None:
        command.extend(["--initial-output-buffer-kib", str(initial_output_buffer_kib)])
    process = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    server = ServerProcess("goblin", process, port)
    try:
        wait_for_server(port)
    except Exception:
        server.stop()
        raise
    return server


def start_redis(binary: Path) -> ServerProcess:
    binary = resolve_executable(binary, "Redis server")
    port = free_port()
    temp_dir = Path(tempfile.mkdtemp(prefix="goblin-redis-bench-"))
    process = subprocess.Popen(
        [
            str(binary),
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
            str(temp_dir),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    server = ServerProcess("redis", process, port, temp_dir)
    try:
        wait_for_server(port)
    except Exception:
        server.stop()
        raise
    return server


def process_rss_mib(pid: int) -> float:
    output = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True)
    rss_kib = int(output.strip() or "0")
    return rss_kib / 1024.0


def redis_used_memory_mib(client: RespClient) -> float | None:
    try:
        response = client.command("INFO", "memory")
    except Exception:
        return None
    if not isinstance(response, bytes):
        return None
    for line in response.decode(errors="replace").splitlines():
        if line.startswith("used_memory:"):
            return int(line.split(":", 1)[1]) / (1024.0 * 1024.0)
    return None


def goblin_memory_stats(client: RespClient, key: str) -> dict[str, int | str] | None:
    try:
        response = client.command("GOBLIN.MEMORY", key)
    except Exception:
        return None
    if not isinstance(response, list) or len(response) % 2 != 0:
        return None

    stats: dict[str, int | str] = {}
    for i in range(0, len(response), 2):
        name = response[i]
        value = response[i + 1]
        if not isinstance(name, bytes) or not isinstance(value, bytes):
            return None
        decoded_name = name.decode()
        try:
            stats[decoded_name] = int(value)
        except ValueError:
            stats[decoded_name] = value.decode()
    return stats


SCORE_SHAPES = ("integer", "short-decimal", "long-decimal", "random-double")


def splitmix64(value: int) -> int:
    value = (value + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return value ^ (value >> 31)


def score_for(member_id: int, score_shape: str, seed: int) -> float:
    # Deterministic but unsorted scores to exercise middle-of-list inserts.
    if score_shape == "integer":
        value = (member_id * 1_103_515_245 + 12_345) & 0xFFFFFFFF
        return float(value)

    hashed = splitmix64(member_id ^ (seed << 1))
    if score_shape == "short-decimal":
        return float(hashed % 1_000_000_000) / 100.0
    if score_shape == "long-decimal":
        whole = hashed % 10_000_000
        fraction = splitmix64(hashed) % 1_000_000_000_000
        return float(whole) + float(fraction) / 1_000_000_000_000.0
    if score_shape == "random-double":
        return float(hashed >> 11) * (1.0 / 9_007_199_254_740_992.0) * 1_000_000_000.0

    raise ValueError(f"unknown score shape: {score_shape}")


def member_for(member_id: int) -> str:
    return f"member:{member_id:010d}"


def shuffled_ids(count: int, seed: int) -> list[int]:
    ids = list(range(count))
    random.Random(seed).shuffle(ids)
    return ids


def zadd_commands(ids: Sequence[int],
                  key: str,
                  batch_size: int,
                  score_shape: str,
                  seed: int) -> Iterable[list[object]]:
    for start in range(0, len(ids), batch_size):
        command: list[object] = ["ZADD", key]
        for member_id in ids[start : start + batch_size]:
            command.append(score_for(member_id, score_shape, seed))
            command.append(member_for(member_id))
        yield command


def one_member_commands(command_name: str,
                        ids: Sequence[int],
                        key: str) -> Iterable[list[object]]:
    for member_id in ids:
        yield [command_name, key, member_for(member_id)]


def range_commands(command_name: str,
                   count: int,
                   member_count: int,
                   key: str,
                   range_size: int,
                   seed: int,
                   with_scores: bool = False) -> Iterable[list[object]]:
    rng = random.Random(seed)
    max_start = max(0, member_count - range_size)
    for _ in range(count):
        start = rng.randint(0, max_start) if max_start > 0 else 0
        command: list[object] = [command_name, key, start, start + range_size - 1]
        if with_scores:
            command.append("WITHSCORES")
        yield command


def zrange_commands(count: int,
                    member_count: int,
                    key: str,
                    range_size: int,
                    seed: int,
                    with_scores: bool = False) -> Iterable[list[object]]:
    return range_commands(
        "ZRANGE", count, member_count, key, range_size, seed, with_scores)


def zrem_commands(ids: Sequence[int], key: str, batch_size: int) -> Iterable[list[object]]:
    for start in range(0, len(ids), batch_size):
        command: list[object] = ["ZREM", key]
        command.extend(member_for(member_id) for member_id in ids[start : start + batch_size])
        yield command


def time_pipeline(client: RespClient,
                  commands: Iterable[Sequence[object]],
                  flush_every: int) -> tuple[int, float]:
    started = time.perf_counter()
    command_count = client.pipeline(commands, flush_every)
    elapsed = time.perf_counter() - started
    return command_count, elapsed


def percentile(sorted_values: Sequence[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    index = round((len(sorted_values) - 1) * fraction)
    return sorted_values[index]


def latency_stats_us(latencies: Sequence[float]) -> dict[str, float]:
    ordered = sorted(latencies)
    return {
        "min": ordered[0] * 1_000_000.0,
        "p50": percentile(ordered, 0.50) * 1_000_000.0,
        "p95": percentile(ordered, 0.95) * 1_000_000.0,
        "p99": percentile(ordered, 0.99) * 1_000_000.0,
        "max": ordered[-1] * 1_000_000.0,
    }


def time_latency_samples(client: RespClient,
                         commands: Iterable[Sequence[object]],
                         warmup: int,
                         samples: int,
                         pipeline_depth: int) -> tuple[int, float, dict[str, float]]:
    iterator = iter(commands)
    for _ in range(warmup):
        command = next(iterator)
        client.command(*command)

    latencies: list[float] = []
    started = time.perf_counter()

    if pipeline_depth == 1:
        for _ in range(samples):
            command = next(iterator)
            command_started = time.perf_counter()
            client.command(*command)
            latencies.append(time.perf_counter() - command_started)
    else:
        remaining = samples
        while remaining > 0:
            batch: list[Sequence[object]] = []
            for _ in range(min(pipeline_depth, remaining)):
                batch.append(next(iterator))
            batch_started = time.perf_counter()
            payload = bytearray()
            for command in batch:
                payload.extend(encode_command(command))
            client.sock.sendall(payload)
            for _ in batch:
                client.read_response()
                latencies.append(time.perf_counter() - batch_started)
            remaining -= len(batch)

    elapsed = time.perf_counter() - started
    return samples, elapsed, latency_stats_us(latencies)


def make_result(target: str,
                metric: str,
                count: int,
                seconds: float,
                loaded_members: int,
                rss_baseline: float,
                rss_after_load: float,
                rss_final: float,
                redis_used_memory: float | None,
                redis_final_used_memory: float | None,
                goblin_memory: dict[str, int | str] | None = None,
                latency_stats: dict[str, float] | None = None) -> BenchmarkResult:
    rss_delta = rss_after_load - rss_baseline

    def goblin_mib(name: str) -> float | None:
        if goblin_memory is None:
            return None
        value = goblin_memory.get(name, 0)
        if not isinstance(value, int):
            return None
        return value / (1024.0 * 1024.0)

    return BenchmarkResult(
        target=target,
        metric=metric,
        score_shape="integer",
        range_size=0,
        count=count,
        seconds=seconds,
        per_second=(count / seconds) if seconds > 0 else 0.0,
        rss_baseline_mib=rss_baseline,
        rss_after_load_mib=rss_after_load,
        rss_delta_mib=rss_delta,
        rss_delta_bytes_per_member=(rss_delta * 1024.0 * 1024.0 / loaded_members),
        rss_final_mib=rss_final,
        redis_used_memory_mib=redis_used_memory,
        redis_used_bytes_per_member=(
            None if redis_used_memory is None
            else redis_used_memory * 1024.0 * 1024.0 / loaded_members
        ),
        redis_final_used_memory_mib=redis_final_used_memory,
        goblin_member_storage_allocated_mib=goblin_mib("member_storage_allocated_bytes"),
        goblin_member_index_allocated_mib=goblin_mib("member_index_allocated_bytes"),
        goblin_score_index_allocated_mib=goblin_mib("score_index_allocated_bytes"),
        goblin_score_string_cache_allocated_mib=goblin_mib(
            "score_string_cache_allocated_bytes"
        ),
        goblin_rank_location_cache_allocated_mib=goblin_mib(
            "rank_location_cache_allocated_bytes"
        ),
        goblin_total_allocated_mib=goblin_mib("total_allocated_bytes"),
        latency_min_us=None if latency_stats is None else latency_stats["min"],
        latency_p50_us=None if latency_stats is None else latency_stats["p50"],
        latency_p95_us=None if latency_stats is None else latency_stats["p95"],
        latency_p99_us=None if latency_stats is None else latency_stats["p99"],
        latency_max_us=None if latency_stats is None else latency_stats["max"],
    )


def append_latency_result(results: list[BenchmarkResult],
                          client: RespClient,
                          server: ServerProcess,
                          metric: str,
                          commands: Iterable[Sequence[object]],
                          args: argparse.Namespace,
                          rss_baseline: float,
                          rss_after_load: float,
                          used_memory: float | None,
                          goblin_memory: dict[str, int | str] | None) -> None:
    count, seconds, stats = time_latency_samples(
        client,
        commands,
        args.latency_warmup,
        args.latency_samples,
        args.latency_pipeline_depth,
    )
    results.append(
        make_result(server.name, metric, count, seconds, args.members,
                    rss_baseline, rss_after_load, rss_after_load, used_memory,
                    used_memory, goblin_memory, stats)
    )


def run_target(server: ServerProcess, args: argparse.Namespace) -> list[BenchmarkResult]:
    client = RespClient("127.0.0.1", server.port, timeout=args.timeout)
    try:
        key = f"zbench:{os.getpid()}:{server.name}"
        load_ids = shuffled_ids(args.members, args.seed)
        lookup_ids = [load_ids[i % len(load_ids)] for i in range(args.ops)]
        random.Random(args.seed + 1).shuffle(lookup_ids)
        remove_ids = load_ids[: min(args.remove_members, args.members)]

        rss_baseline = process_rss_mib(server.process.pid)

        zadd_command_count, zadd_seconds = time_pipeline(
            client,
            zadd_commands(load_ids, key, args.zadd_batch, args.score_shape, args.seed),
            args.pipeline,
        )
        time.sleep(args.settle_seconds)
        rss_after_load = process_rss_mib(server.process.pid)
        used_memory = redis_used_memory_mib(client) if server.name == "redis" else None
        goblin_memory = (
            goblin_memory_stats(client, key) if server.name == "goblin" else None
        )

        results: list[BenchmarkResult] = [
            make_result(
                server.name,
                "zadd_members",
                args.members,
                zadd_seconds,
                args.members,
                rss_baseline,
                rss_after_load,
                rss_after_load,
                used_memory,
                used_memory,
                goblin_memory,
            ),
            make_result(
                server.name,
                "zadd_commands",
                zadd_command_count,
                zadd_seconds,
                args.members,
                rss_baseline,
                rss_after_load,
                rss_after_load,
                used_memory,
                used_memory,
                goblin_memory,
            ),
        ]

        _, zscore_seconds = time_pipeline(
            client,
            one_member_commands("ZSCORE", lookup_ids, key),
            args.pipeline,
        )
        results.append(
            make_result(server.name, "zscore_ops", args.ops, zscore_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_after_load, used_memory,
                        used_memory, goblin_memory)
        )

        _, zrank_seconds = time_pipeline(
            client,
            one_member_commands("ZRANK", lookup_ids, key),
            args.pipeline,
        )
        results.append(
            make_result(server.name, "zrank_ops", args.ops, zrank_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_after_load, used_memory,
                        used_memory, goblin_memory)
        )

        _, zrevrank_seconds = time_pipeline(
            client,
            one_member_commands("ZREVRANK", lookup_ids, key),
            args.pipeline,
        )
        results.append(
            make_result(server.name, "zrevrank_ops", args.ops, zrevrank_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_after_load, used_memory,
                        used_memory, goblin_memory)
        )

        _, zrange_seconds = time_pipeline(
            client,
            zrange_commands(args.ops, args.members, key, args.range_size, args.seed + 2),
            args.pipeline,
        )
        results.append(
            make_result(server.name, "zrange_ops", args.ops, zrange_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_after_load, used_memory,
                        used_memory, goblin_memory)
        )

        _, zrange_withscores_seconds = time_pipeline(
            client,
            zrange_commands(
                args.ops, args.members, key, args.range_size, args.seed + 7, True),
            args.pipeline,
        )
        results.append(
            make_result(
                server.name,
                "zrange_withscores_ops",
                args.ops,
                zrange_withscores_seconds,
                args.members,
                rss_baseline,
                rss_after_load,
                rss_after_load,
                used_memory,
                used_memory,
                goblin_memory,
            )
        )

        _, zrevrange_seconds = time_pipeline(
            client,
            range_commands("ZREVRANGE", args.ops, args.members, key,
                           args.range_size, args.seed + 3),
            args.pipeline,
        )
        results.append(
            make_result(server.name, "zrevrange_ops", args.ops, zrevrange_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_after_load, used_memory,
                        used_memory, goblin_memory)
        )

        _, zrevrange_withscores_seconds = time_pipeline(
            client,
            range_commands(
                "ZREVRANGE", args.ops, args.members, key,
                args.range_size, args.seed + 8, True),
            args.pipeline,
        )
        results.append(
            make_result(
                server.name,
                "zrevrange_withscores_ops",
                args.ops,
                zrevrange_withscores_seconds,
                args.members,
                rss_baseline,
                rss_after_load,
                rss_after_load,
                used_memory,
                used_memory,
                goblin_memory,
            )
        )

        if args.latency_samples > 0:
            latency_count = args.latency_samples + args.latency_warmup
            latency_ids = [
                load_ids[i % len(load_ids)] for i in range(latency_count)
            ]
            random.Random(args.seed + 4).shuffle(latency_ids)
            append_latency_result(
                results, client, server, "zscore_latency",
                one_member_commands("ZSCORE", latency_ids, key),
                args, rss_baseline, rss_after_load, used_memory, goblin_memory)
            append_latency_result(
                results, client, server, "zrank_latency",
                one_member_commands("ZRANK", latency_ids, key),
                args, rss_baseline, rss_after_load, used_memory, goblin_memory)
            append_latency_result(
                results, client, server, "zrevrank_latency",
                one_member_commands("ZREVRANK", latency_ids, key),
                args, rss_baseline, rss_after_load, used_memory, goblin_memory)
            append_latency_result(
                results, client, server, "zrange_latency",
                range_commands("ZRANGE", latency_count, args.members, key,
                               args.range_size, args.seed + 5),
                args, rss_baseline, rss_after_load, used_memory, goblin_memory)
            append_latency_result(
                results, client, server, "zrevrange_latency",
                range_commands("ZREVRANGE", latency_count, args.members, key,
                               args.range_size, args.seed + 6),
                args, rss_baseline, rss_after_load, used_memory, goblin_memory)

        zrem_command_count, zrem_seconds = time_pipeline(
            client,
            zrem_commands(remove_ids, key, args.zrem_batch),
            args.pipeline,
        )
        time.sleep(args.settle_seconds)
        rss_final = process_rss_mib(server.process.pid)
        final_used_memory = redis_used_memory_mib(client) if server.name == "redis" else None
        results.append(
            make_result(server.name, "zrem_members", len(remove_ids), zrem_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_final, used_memory,
                        final_used_memory, goblin_memory)
        )
        results.append(
            make_result(server.name, "zrem_commands", zrem_command_count, zrem_seconds,
                        args.members,
                        rss_baseline, rss_after_load, rss_final, used_memory,
                        final_used_memory, goblin_memory)
        )

        for result in results:
            result.score_shape = args.score_shape
            result.range_size = args.range_size
        return results
    finally:
        client.close()


def print_markdown(results: Sequence[BenchmarkResult]) -> None:
    headers = [
        "target",
        "metric",
        "score_shape",
        "range_size",
        "count",
        "seconds",
        "per_sec",
        "rss_base_mib",
        "rss_load_mib",
        "rss_delta_mib",
        "rss_delta_b/member",
        "rss_final_mib",
        "redis_used_mib",
        "redis_used_b/member",
        "redis_final_used_mib",
        "goblin_member_storage_mib",
        "goblin_member_index_mib",
        "goblin_score_index_mib",
        "goblin_score_string_cache_mib",
        "goblin_rank_cache_mib",
        "goblin_total_alloc_mib",
        "latency_min_us",
        "latency_p50_us",
        "latency_p95_us",
        "latency_p99_us",
        "latency_max_us",
    ]
    print("| " + " | ".join(headers) + " |")
    print("| " + " | ".join("---" for _ in headers) + " |")
    for row in results:
        print(
            "| "
            + " | ".join(
                [
                    row.target,
                    row.metric,
                    row.score_shape,
                    str(row.range_size),
                    str(row.count),
                    f"{row.seconds:.6f}",
                    f"{row.per_second:.2f}",
                    f"{row.rss_baseline_mib:.2f}",
                    f"{row.rss_after_load_mib:.2f}",
                    f"{row.rss_delta_mib:.2f}",
                    f"{row.rss_delta_bytes_per_member:.2f}",
                    f"{row.rss_final_mib:.2f}",
                    "" if row.redis_used_memory_mib is None else f"{row.redis_used_memory_mib:.2f}",
                    ""
                    if row.redis_used_bytes_per_member is None
                    else f"{row.redis_used_bytes_per_member:.2f}",
                    ""
                    if row.redis_final_used_memory_mib is None
                    else f"{row.redis_final_used_memory_mib:.2f}",
                    ""
                    if row.goblin_member_storage_allocated_mib is None
                    else f"{row.goblin_member_storage_allocated_mib:.2f}",
                    ""
                    if row.goblin_member_index_allocated_mib is None
                    else f"{row.goblin_member_index_allocated_mib:.2f}",
                    ""
                    if row.goblin_score_index_allocated_mib is None
                    else f"{row.goblin_score_index_allocated_mib:.2f}",
                    ""
                    if row.goblin_score_string_cache_allocated_mib is None
                    else f"{row.goblin_score_string_cache_allocated_mib:.2f}",
                    ""
                    if row.goblin_rank_location_cache_allocated_mib is None
                    else f"{row.goblin_rank_location_cache_allocated_mib:.2f}",
                    ""
                    if row.goblin_total_allocated_mib is None
                    else f"{row.goblin_total_allocated_mib:.2f}",
                    ""
                    if row.latency_min_us is None
                    else f"{row.latency_min_us:.2f}",
                    ""
                    if row.latency_p50_us is None
                    else f"{row.latency_p50_us:.2f}",
                    ""
                    if row.latency_p95_us is None
                    else f"{row.latency_p95_us:.2f}",
                    ""
                    if row.latency_p99_us is None
                    else f"{row.latency_p99_us:.2f}",
                    ""
                    if row.latency_max_us is None
                    else f"{row.latency_max_us:.2f}",
                ]
            )
            + " |"
        )


def write_output(results: Sequence[BenchmarkResult], fmt: str, output: Path | None) -> None:
    rows = [asdict(row) for row in results]
    if output is None:
        if fmt == "json":
            print(json.dumps(rows, indent=2))
        elif fmt == "csv":
            writer = csv.DictWriter(sys.stdout, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
        else:
            print_markdown(results)
        return

    output.parent.mkdir(parents=True, exist_ok=True)
    if fmt == "json":
        output.write_text(json.dumps(rows, indent=2) + "\n")
    elif fmt == "csv":
        with output.open("w", newline="") as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)
    else:
        with output.open("w") as file:
            original_stdout = sys.stdout
            try:
                sys.stdout = file
                print_markdown(results)
            finally:
                sys.stdout = original_stdout


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=["both", "goblin", "redis"], default="both")
    default_goblin = Path("build-release/goblin-core")
    if not default_goblin.exists():
        default_goblin = Path("build/goblin-core")
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--goblin-rank-cache", action="store_true")
    parser.add_argument("--goblin-rank-cache-mode",
                        choices=["off", "exact", "block-hint"])
    parser.add_argument("--goblin-score-string-cache", action="store_true")
    parser.add_argument("--goblin-max-output-buffer-mib", type=int)
    parser.add_argument("--redis-server", type=Path, default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--members", type=int, default=1_000_000)
    parser.add_argument("--ops", type=int, default=1_000_000)
    parser.add_argument("--remove-members", type=int, default=500_000)
    parser.add_argument("--zadd-batch", type=int, default=128)
    parser.add_argument("--zrem-batch", type=int, default=128)
    parser.add_argument("--range-size", type=int, default=16)
    parser.add_argument("--score-shape", choices=SCORE_SHAPES, default="integer")
    parser.add_argument("--pipeline", type=int, default=256)
    parser.add_argument("--latency-samples", type=int, default=0,
                        help="Per-read-command latency samples. 0 disables latency mode.")
    parser.add_argument("--latency-warmup", type=int, default=100,
                        help="Warmup commands per latency metric before sampling.")
    parser.add_argument("--latency-pipeline-depth", type=int, default=1,
                        help="Commands per latency batch. 1 measures single-command round trips.")
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--settle-seconds", type=float, default=0.1)
    parser.add_argument("--format", choices=["markdown", "json", "csv"], default="markdown")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    if args.members <= 0:
        parser.error("--members must be positive")
    if args.ops < 0 or args.remove_members < 0 or args.latency_samples < 0:
        parser.error("--ops, --remove-members, and --latency-samples must be non-negative")
    if args.latency_warmup < 0:
        parser.error("--latency-warmup must be non-negative")
    if args.goblin_max_output_buffer_mib is not None and args.goblin_max_output_buffer_mib < 0:
        parser.error("--goblin-max-output-buffer-mib must be non-negative")
    if min(args.zadd_batch, args.zrem_batch, args.range_size, args.pipeline,
           args.latency_pipeline_depth) <= 0:
        parser.error(
            "--zadd-batch, --zrem-batch, --range-size, --pipeline, "
            "and --latency-pipeline-depth must be positive"
        )
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    goblin_rank_cache_mode = (
        args.goblin_rank_cache_mode
        if args.goblin_rank_cache_mode is not None
        else ("exact" if args.goblin_rank_cache else "off")
    )

    starters = []
    if args.target in ("both", "goblin"):
        starters.append(
            lambda: start_goblin(
                args.goblin_bin,
                args.goblin_rank_cache,
                goblin_rank_cache_mode,
                args.goblin_max_output_buffer_mib,
                args.goblin_score_string_cache,
            )
        )
    if args.target in ("both", "redis"):
        if not args.redis_server.exists() and shutil.which(str(args.redis_server)) is None:
            print(
                f"redis-server not found: {args.redis_server}. "
                "Install Redis or pass --redis-server /path/to/redis-server.",
                file=sys.stderr,
            )
            return 2
        starters.append(lambda: start_redis(args.redis_server))

    results: list[BenchmarkResult] = []
    for starter in starters:
        server = starter()
        try:
            results.extend(run_target(server, args))
        finally:
            server.stop()

    write_output(results, args.format, args.output)
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main(sys.argv[1:]))
