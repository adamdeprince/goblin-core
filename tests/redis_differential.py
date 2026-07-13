#!/usr/bin/env python3
"""Compare Goblin Core's sorted-set and list behavior against Redis.

The test starts one Goblin Core process and one Redis process, sends the same
deterministic workload to both over RESP, and fails on the first semantic
mismatch. It covers the implemented sorted-set surface plus LPUSH/RPUSH,
LPUSHX/RPUSHX, LPOP/RPOP, LLEN, LINDEX, LRANGE, LSET, LTRIM, LREM, and LINSERT.
Redis receives the standard list names; Goblin receives a qualified list family.
"""

from __future__ import annotations

import argparse
import math
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parents[1]
LIST_COMMANDS = {
    "LPUSH", "RPUSH", "LPUSHX", "RPUSHX", "LPOP", "RPOP", "LLEN",
    "LINDEX", "LRANGE", "LSET", "LTRIM", "LREM", "LINSERT",
}


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


class RespError(RuntimeError):
    pass


class RespClient:
    def __init__(self, port: int, timeout: float) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buffer = bytearray()

    def close(self) -> None:
        self.sock.close()

    def command(self, *parts: object) -> object:
        self.send_command(*parts)
        return self.read_response()

    def send_command(self, *parts: object) -> None:
        self.sock.sendall(encode_command(parts))

    def send_batch(self, commands: Sequence[Sequence[object]]) -> None:
        payload = bytearray()
        for command in commands:
            payload.extend(encode_command(command))
        self.sock.sendall(payload)

    def read_response(self) -> object:
        prefix = self._read_exact(1)
        if prefix == b"+":
            return self._read_line().decode()
        if prefix == b"-":
            raise RespError(self._read_line().decode(errors="replace"))
        if prefix == b":":
            return int(self._read_line())
        if prefix == b"$":
            length = int(self._read_line())
            if length == -1:
                return None
            data = self._read_exact(length)
            if self._read_exact(2) != b"\r\n":
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


@dataclass(frozen=True)
class ErrorResponse:
    message: str


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def resolve_executable(path: Path, label: str) -> Path:
    if path.exists():
        return path
    resolved = shutil.which(str(path))
    if resolved is not None:
        return Path(resolved)
    raise FileNotFoundError(f"{label} not found: {path}")


def wait_for_server(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            client = RespClient(port, timeout=1.0)
            try:
                if client.command("PING") in ("PONG", b"PONG"):
                    return
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - preserve startup context.
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"server on port {port} did not become ready: {last_error}")


def start_goblin(binary: Path,
                 timeout: float,
                 rank_cache: bool,
                 rank_cache_mode: str | None = None) -> ServerProcess:
    binary = resolve_executable(binary, "Goblin Core binary")
    port = free_port()
    if rank_cache_mode is None:
        rank_cache_mode = "exact" if rank_cache else "off"
    command = [str(binary), "--port", str(port)]
    if rank_cache_mode != "off":
        command.extend(["--rank-cache-mode", rank_cache_mode])
    process = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    server = ServerProcess("goblin", process, port)
    try:
        wait_for_server(port, timeout)
    except Exception:
        server.stop()
        raise
    return server


def start_redis(binary: Path, timeout: float) -> ServerProcess:
    binary = resolve_executable(binary, "Redis server")
    port = free_port()
    temp_dir = Path(tempfile.mkdtemp(prefix="goblin-redis-diff-"))
    command = [
        str(binary),
        "--save",
        "",
        "--appendonly",
        "no",
        "--daemonize",
        "no",
        "--dir",
        str(temp_dir),
        "--bind",
        "127.0.0.1",
        "--protected-mode",
        "no",
        "--port",
        str(port),
    ]
    process = subprocess.Popen(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    server = ServerProcess("redis", process, port, temp_dir)
    try:
        wait_for_server(port, timeout)
    except Exception:
        server.stop()
        raise
    return server


def score_text(value: float) -> str:
    if not math.isfinite(value):
        raise ValueError("non-finite score")
    if value == 0.0:
        return "0"
    return format(value, ".17g")


def fixed_zset_commands() -> list[list[object]]:
    return [
        ["ZCARD", "missing"],
        ["ZSCORE", "missing", "member-1"],
        ["ZRANK", "missing", "member-1"],
        ["ZREVRANK", "missing", "member-1"],
        ["ZRANGE", "missing", "0", "-1"],
        ["ZRANGE", "missing", "0", "-1", "WITHSCORES"],
        ["ZREVRANGE", "missing", "0", "-1"],
        ["ZREVRANGE", "missing", "0", "-1", "WITHSCORES"],
        ["ZREM", "missing", "member-1", "member-2"],
        ["ZADD", "bad", "not-a-float", "member-1"],
        ["ZADD", "bad", "nan", "member-1"],
        ["ZADD", "bad", "1"],
        ["ZRANGE", "bad", "zero", "1"],
        ["ZRANGE", "bad", "0", "1", "BADSCORES"],
        ["ZREVRANGE", "bad", "zero", "1"],
        ["ZREVRANGE", "bad", "0", "1", "BADSCORES"],
        ["ZREVRANK", "bad"],
        ["ZCARD"],
        ["ZSCORE", "bad"],
        ["ZADD", "leaders", "2", "bravo", "1", "alpha", "2", "charlie"],
        ["ZRANGE", "leaders", "0", "-1", "WITHSCORES"],
        ["ZREVRANGE", "leaders", "0", "-1", "WITHSCORES"],
        ["ZRANK", "leaders", "bravo"],
        ["ZREVRANK", "leaders", "bravo"],
        ["ZSCORE", "leaders", "charlie"],
        ["ZADD", "leaders", "-1.25", "bravo"],
        ["ZRANGE", "leaders", "0", "-1", "WITHSCORES"],
        ["ZREVRANGE", "leaders", "0", "-1", "WITHSCORES"],
        ["ZADD", "leaders", "4", "delta", "5", "delta"],
        ["ZSCORE", "leaders", "delta"],
        ["ZREM", "leaders", "alpha", "alpha", "missing"],
        ["ZRANGE", "leaders", "-10", "99", "WITHSCORES"],
        ["ZREVRANGE", "leaders", "-10", "99", "WITHSCORES"],
        ["ZRANGE", "leaders", "2", "1"],
        ["ZRANGE", "leaders", "-2", "-1"],
        ["ZREVRANGE", "leaders", "2", "1"],
        ["ZREVRANGE", "leaders", "-2", "-1"],
    ]


def fixed_list_commands() -> list[list[object]]:
    return [
        ["LLEN", "list:missing"],
        ["LINDEX", "list:missing", "0"],
        ["LRANGE", "list:missing", "0", "-1"],
        ["LPOP", "list:missing"],
        ["LPOP", "list:missing", "3"],
        ["LPUSHX", "list:missing", "x"],
        ["RPUSHX", "list:missing", "x"],
        ["LPUSH", "list:fixed", "one", "two", "three"],
        ["RPUSH", "list:fixed", "four", "five"],
        ["LRANGE", "list:fixed", "0", "-1"],
        ["LRANGE", "list:fixed", "-4", "-2"],
        ["LINDEX", "list:fixed", "-1"],
        ["LINDEX", "list:fixed", "999"],
        ["LINSERT", "list:fixed", "BEFORE", "four", "before-four"],
        ["LINSERT", "list:fixed", "AFTER", "four", "after-four"],
        ["LINSERT", "list:fixed", "BEFORE", "absent", "x"],
        ["LSET", "list:fixed", "-1", "tail"],
        ["LREM", "list:fixed", "1", "four"],
        ["LPUSH", "list:fixed", "dup", "dup"],
        ["RPUSH", "list:fixed", "dup"],
        ["LREM", "list:fixed", "-2", "dup"],
        ["LTRIM", "list:fixed", "1", "-2"],
        ["LPOP", "list:fixed", "2"],
        ["RPOP", "list:fixed", "2"],
        ["LRANGE", "list:fixed", "0", "-1"],
        ["LPOP", "list:fixed", "-1"],
        ["LINSERT", "list:fixed", "SIDEWAYS", "x", "y"],
        ["LSET", "list:missing", "0", "x"],
    ]


def command_stream(args: argparse.Namespace) -> list[list[object]]:
    return [
        *fixed_zset_commands(),
        *fixed_list_commands(),
        *random_zset_commands(args),
        *random_list_commands(args),
    ]


def random_member(rng: random.Random, member_count: int) -> str:
    base = rng.randrange(member_count)
    suffixes = ["", ":x", ".dot", "_under", "-dash", " upper"]
    return f"member-{base:04d}{rng.choice(suffixes)}"


def random_key(rng: random.Random, key_count: int) -> str:
    return f"zset:{rng.randrange(key_count)}"


def random_score(rng: random.Random) -> str:
    if rng.random() < 0.65:
        return str(rng.randint(-10_000, 10_000))
    return score_text(rng.uniform(-10_000.0, 10_000.0))


def random_zset_commands(args: argparse.Namespace) -> Iterable[list[object]]:
    rng = random.Random(args.seed)
    for _ in range(args.ops):
        roll = rng.random()
        key = random_key(rng, args.keys)
        if roll < 0.32:
            pairs = rng.randint(1, args.max_zadd_pairs)
            command: list[object] = ["ZADD", key]
            duplicate_member = random_member(rng, args.members)
            for pair_index in range(pairs):
                member = duplicate_member if pair_index > 0 and rng.random() < 0.2 else random_member(rng, args.members)
                command.extend([random_score(rng), member])
            yield command
        elif roll < 0.48:
            count = rng.randint(1, args.max_zrem_members)
            yield ["ZREM", key, *[random_member(rng, args.members) for _ in range(count)]]
        elif roll < 0.58:
            yield ["ZSCORE", key, random_member(rng, args.members)]
        elif roll < 0.70:
            command = "ZREVRANK" if rng.random() < 0.5 else "ZRANK"
            yield [command, key, random_member(rng, args.members)]
        elif roll < 0.93:
            size = args.members
            start = rng.randint(-size - 10, size + 10)
            stop = start + rng.randint(-5, args.max_range_span)
            command = [
                "ZREVRANGE" if rng.random() < 0.5 else "ZRANGE",
                key,
                str(start),
                str(stop),
            ]
            if rng.random() < 0.5:
                command.append("WITHSCORES")
            yield command
        else:
            yield ["ZCARD", key]


def random_list_commands(args: argparse.Namespace) -> Iterable[list[object]]:
    rng = random.Random(args.seed ^ 0x4C495354)
    for _ in range(args.ops):
        roll = rng.random()
        key = f"list:{rng.randrange(args.keys)}"
        value = f"value-{rng.randrange(args.members):04d}"
        if roll < 0.22:
            command = "LPUSH" if rng.random() < 0.5 else "RPUSH"
            count = rng.randint(1, 4)
            yield [command, key, *[
                f"value-{rng.randrange(args.members):04d}" for _ in range(count)
            ]]
        elif roll < 0.28:
            command = "LPUSHX" if rng.random() < 0.5 else "RPUSHX"
            yield [command, key, value]
        elif roll < 0.41:
            command = "LPOP" if rng.random() < 0.5 else "RPOP"
            if rng.random() < 0.5:
                yield [command, key]
            else:
                yield [command, key, str(rng.randint(0, 5))]
        elif roll < 0.48:
            yield ["LLEN", key]
        elif roll < 0.58:
            yield ["LINDEX", key, str(rng.randint(-30, 30))]
        elif roll < 0.72:
            start = rng.randint(-30, 30)
            stop = start + rng.randint(-4, 20)
            yield ["LRANGE", key, str(start), str(stop)]
        elif roll < 0.78:
            yield ["LSET", key, str(rng.randint(-20, 20)), value]
        elif roll < 0.86:
            start = rng.randint(-20, 20)
            stop = start + rng.randint(-4, 15)
            yield ["LTRIM", key, str(start), str(stop)]
        elif roll < 0.93:
            yield ["LREM", key, str(rng.randint(-3, 3)), value]
        else:
            side = "BEFORE" if rng.random() < 0.5 else "AFTER"
            pivot = f"value-{rng.randrange(args.members):04d}"
            yield ["LINSERT", key, side, pivot, value]


def is_with_scores(command: Sequence[object]) -> bool:
    return len(command) == 5 and str(command[4]).upper() == "WITHSCORES"


def as_float(value: object) -> float | None:
    if value is None:
        return None
    if isinstance(value, bytes):
        return float(value.decode())
    return float(value)


def normalize_response(command: Sequence[object], response: object) -> object:
    if isinstance(response, ErrorResponse):
        return ("error", error_category(response.message))

    name = str(command[0]).upper()
    if name == "ZSCORE":
        return as_float(response)
    if name in ("ZRANGE", "ZREVRANGE") and is_with_scores(command):
        assert isinstance(response, list)
        normalized: list[object] = []
        for index, item in enumerate(response):
            normalized.append(as_float(item) if index % 2 else item)
        return normalized
    return response


def error_category(message: str) -> str:
    text = message.lower()
    if "wrong number" in text:
        return "arity"
    if "not a valid float" in text:
        return "float"
    if "not an integer" in text or "out of range" in text:
        return "integer"
    if "syntax" in text:
        return "syntax"
    return "other"


def responses_equal(expected: object, actual: object) -> bool:
    if isinstance(expected, float) and isinstance(actual, float):
        return math.isclose(expected, actual, rel_tol=0.0, abs_tol=1e-12)
    if isinstance(expected, list) and isinstance(actual, list):
        return len(expected) == len(actual) and all(
            responses_equal(left, right) for left, right in zip(expected, actual)
        )
    return expected == actual


def format_command(command: Sequence[object]) -> str:
    return " ".join(repr(part) if isinstance(part, bytes) else str(part) for part in command)


def execute(client: RespClient, command: Sequence[object]) -> object:
    try:
        return client.command(*command)
    except RespError as exc:
        return ErrorResponse(str(exc))


def goblin_command(command: Sequence[object], list_prefix: str) -> list[object]:
    translated = list(command)
    name = str(translated[0]).upper()
    if name in LIST_COMMANDS:
        translated[0] = f"{list_prefix}{name}"
    return translated


def read_response(client: RespClient) -> object:
    try:
        return client.read_response()
    except RespError as exc:
        return ErrorResponse(str(exc))


def compare_command(command: Sequence[object],
                    redis_response: object,
                    goblin_response: object,
                    history: list[str]) -> None:
    expected = normalize_response(command, redis_response)
    actual = normalize_response(command, goblin_response)
    history.append(
        f"{format_command(command)} => redis={expected!r} goblin={actual!r}"
    )
    del history[:-25]
    if responses_equal(expected, actual):
        return

    context = "\n".join(f"  {line}" for line in history)
    raise AssertionError(
        "Goblin Core/Redis response mismatch\n"
        f"command: {format_command(command)}\n"
        f"redis raw: {redis_response!r}\n"
        f"goblin raw: {goblin_response!r}\n"
        f"redis normalized: {expected!r}\n"
        f"goblin normalized: {actual!r}\n"
        f"recent history:\n{context}"
    )


def run_command_pair(goblin: RespClient,
                     redis: RespClient,
                     command: Sequence[object],
                     history: list[str],
                     list_prefix: str) -> None:
    compare_command(
        command,
        execute(redis, command),
        execute(goblin, goblin_command(command, list_prefix)),
        history,
    )


def run_command_batch(goblin: RespClient,
                      redis: RespClient,
                      commands: Sequence[Sequence[object]],
                      history: list[str],
                      list_prefix: str) -> None:
    redis.send_batch(commands)
    goblin.send_batch([goblin_command(command, list_prefix) for command in commands])
    for command in commands:
        redis_response = read_response(redis)
        goblin_response = read_response(goblin)
        compare_command(command, redis_response, goblin_response, history)


def run_commands(goblin: RespClient,
                 redis: RespClient,
                 commands: Sequence[Sequence[object]],
                 pipeline_depth: int,
                 list_prefix: str) -> int:
    history: list[str] = []
    if pipeline_depth <= 1:
        for command in commands:
            run_command_pair(goblin, redis, command, history, list_prefix)
        return len(commands)

    for start in range(0, len(commands), pipeline_depth):
        run_command_batch(
            goblin,
            redis,
            commands[start:start + pipeline_depth],
            history,
            list_prefix,
        )
    return len(commands)


def run_differential(args: argparse.Namespace) -> None:
    if args.pipeline_depth < 1:
        raise ValueError("--pipeline-depth must be at least 1")

    commands = command_stream(args)
    goblin_server = start_goblin(
        args.goblin_bin,
        args.timeout,
        args.rank_cache,
        args.rank_cache_mode,
    )
    redis_server = start_redis(args.redis_server, args.timeout)
    try:
        goblin = RespClient(goblin_server.port, args.timeout)
        redis = RespClient(redis_server.port, args.timeout)
        try:
            count = run_commands(
                goblin, redis, commands, args.pipeline_depth, args.goblin_list_prefix
            )
        finally:
            goblin.close()
            redis.close()
    finally:
        goblin_server.stop()
        redis_server.stop()
    mode = "sequential" if args.pipeline_depth == 1 else "pipelined"
    print(
        f"redis differential passed: {count} commands, mode={mode}, "
        f"pipeline_depth={args.pipeline_depth}, seed={args.seed}, "
        f"rank_cache_mode={args.rank_cache_mode or ('exact' if args.rank_cache else 'off')}"
    )


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin_binary())
    parser.add_argument("--redis-server", type=Path,
                        default=Path(shutil.which("redis-server") or "redis-server"))
    parser.add_argument("--ops", type=int, default=5_000)
    parser.add_argument("--seed", type=int, default=12_345)
    parser.add_argument("--keys", type=int, default=6)
    parser.add_argument("--members", type=int, default=1_000)
    parser.add_argument("--max-zadd-pairs", type=int, default=4)
    parser.add_argument("--max-zrem-members", type=int, default=4)
    parser.add_argument("--max-range-span", type=int, default=80)
    parser.add_argument("--pipeline-depth", type=int, default=1,
                        help="Commands per pipelined write. 1 keeps sequential mode.")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--rank-cache", action="store_true",
                        help="Run Goblin Core with --rank-cache enabled.")
    parser.add_argument("--rank-cache-mode", choices=["off", "exact", "block-hint"])
    parser.add_argument(
        "--goblin-list-prefix",
        default="GOBLIN.PMA.",
        help="Concrete Goblin list command prefix (PMA or SEGMENTED); use an empty string for aliases.",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    run_differential(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
