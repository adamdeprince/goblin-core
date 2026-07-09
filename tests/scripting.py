#!/usr/bin/env python3
"""End-to-end scripting test: drive EVAL / EVALSHA / SCRIPT over a real socket.

Unlike the C++ unit tests (which call the ScriptEngine directly), this exercises
the full server path -- the poll loop, command dispatch, and the engine wired
into it -- against a launched goblin-core process.
"""
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Sequence

ROOT = Path(__file__).resolve().parent.parent


def default_goblin_binary() -> Path:
    release = ROOT / "build-release" / "goblin-core"
    if release.exists():
        return release
    return ROOT / "build" / "goblin-core"


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def encode(*args: object) -> bytes:
    parts = [f"*{len(args)}\r\n".encode()]
    for a in args:
        b = a.encode() if isinstance(a, str) else str(a).encode()
        parts.append(b"$%d\r\n%s\r\n" % (len(b), b))
    return b"".join(parts)


class Conn:
    def __init__(self, port: int, timeout: float = 5.0):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.buf = b""

    def _fill(self) -> None:
        data = self.s.recv(65536)
        if not data:
            raise EOFError("server closed the connection")
        self.buf += data

    def _line(self) -> bytes:
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return ("status", rest.decode())
        if t == b"-":
            return ("error", rest.decode())
        if t == b":":
            return ("int", int(rest))
        if t == b"$":
            n = int(rest)
            if n < 0:
                return ("nil", None)
            while len(self.buf) < n + 2:
                self._fill()
            payload, self.buf = self.buf[:n], self.buf[n + 2:]
            return ("bulk", payload.decode("utf-8", "replace"))
        if t == b"*":
            n = int(rest)
            if n < 0:
                return ("nil", None)
            return ("array", [self.read() for _ in range(n)])
        raise ValueError(f"unexpected reply header {line!r}")

    def cmd(self, *args: object):
        self.s.sendall(encode(*args))
        return self.read()

    def close(self) -> None:
        try:
            self.s.close()
        except OSError:
            pass


def wait_for_server(port: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            c = Conn(port, timeout=1.0)
            try:
                if c.cmd("PING") == ("status", "PONG"):
                    return
            finally:
                c.close()
        except Exception as exc:  # noqa: BLE001
            last = exc
            time.sleep(0.05)
    raise RuntimeError(f"server on port {port} never became ready: {last}")


def run(port: int) -> int:
    c = Conn(port)
    passed = failed = 0

    def check(name, got, expect):
        nonlocal passed, failed
        if expect[0] == "error":
            ok = got[0] == "error" and (expect[1] is None or got[1].startswith(expect[1]))
        else:
            ok = got == expect
        if ok:
            passed += 1
            print(f"[ok  ] {name}")
        else:
            failed += 1
            print(f"[FAIL] {name}  expected={expect!r} got={got!r}")

    check("EVAL return 1", c.cmd("EVAL", "return 1", "0"), ("int", 1))
    check("EVAL return string", c.cmd("EVAL", "return 'hello'", "0"), ("bulk", "hello"))
    check("EVAL float truncates", c.cmd("EVAL", "return 3.99", "0"), ("int", 3))
    check("EVAL true->1", c.cmd("EVAL", "return true", "0"), ("int", 1))
    check("EVAL false->nil", c.cmd("EVAL", "return false", "0"), ("nil", None))
    check("EVAL array", c.cmd("EVAL", "return {1,2,3}", "0"),
          ("array", [("int", 1), ("int", 2), ("int", 3)]))
    check("EVAL array stops at nil", c.cmd("EVAL", "return {1,2,nil,4}", "0"),
          ("array", [("int", 1), ("int", 2)]))
    check("EVAL KEYS/ARGV", c.cmd("EVAL", "return {KEYS[1], ARGV[1]}", "1", "k", "a"),
          ("array", [("bulk", "k"), ("bulk", "a")]))
    check("EVAL status_reply", c.cmd("EVAL", "return redis.status_reply('GOOD')", "0"),
          ("status", "GOOD"))
    check("EVAL error_reply", c.cmd("EVAL", "return redis.error_reply('My Error')", "0"),
          ("error", "My Error"))
    check("EVAL sha1hex(empty)", c.cmd("EVAL", "return redis.sha1hex('')", "0"),
          ("bulk", "da39a3ee5e6b4b0d3255bfef95601890afd80709"))
    check("redis.call PING", c.cmd("EVAL", "return redis.call('ping')", "0"),
          ("status", "PONG"))
    check("redis.call ZADD", c.cmd("EVAL", "return redis.call('zadd', KEYS[1], 1, 'a')", "1", "z"),
          ("int", 1))
    check("redis.call ZSCORE", c.cmd("EVAL", "return redis.call('zscore', KEYS[1], 'a')", "1", "z"),
          ("bulk", "1"))
    check("write visible to plain cmd", c.cmd("ZSCORE", "z", "a"), ("bulk", "1"))
    check("redis.pcall catches", c.cmd(
        "EVAL", "local r = redis.pcall('zscore'); if r.err then return 'caught' end", "0"),
        ("bulk", "caught"))
    check("redis.call error raises", c.cmd("EVAL", "return redis.call('zscore')", "0"),
          ("error", None))
    check("global write denied", c.cmd("EVAL", "x = 1", "0"), ("error", None))
    check("nested eval denied", c.cmd("EVAL", "return redis.call('eval','return 1','0')", "0"),
          ("error", None))
    check("compile error", c.cmd("EVAL", "this is not lua", "0"), ("error", None))
    check("cjson.encode", c.cmd("EVAL", "return cjson.encode({1,2,3})", "0"), ("bulk", "[1,2,3]"))
    check("cjson.decode", c.cmd("EVAL", "return cjson.decode('[10,20,30]')[2]", "0"), ("int", 20))
    check("cmsgpack roundtrip",
          c.cmd("EVAL", "return cmsgpack.unpack(cmsgpack.pack('hi'))", "0"), ("bulk", "hi"))
    check("bit.band", c.cmd("EVAL", "return bit.band(6,3)", "0"), ("int", 2))
    check("struct roundtrip",
          c.cmd("EVAL", "return struct.unpack('>I2', struct.pack('>I2', 258))", "0"), ("int", 258))

    # SCRIPT LOAD + EVALSHA + EXISTS + FLUSH lifecycle.
    sha = c.cmd("SCRIPT", "LOAD", "return 42")
    if sha[0] == "bulk" and len(sha[1]) == 40:
        passed += 1
        print("[ok  ] SCRIPT LOAD -> 40-hex sha")
        check("EVALSHA runs cached", c.cmd("EVALSHA", sha[1], "0"), ("int", 42))
        check("SCRIPT EXISTS hit", c.cmd("SCRIPT", "EXISTS", sha[1]),
              ("array", [("int", 1)]))
        check("SCRIPT FLUSH", c.cmd("SCRIPT", "FLUSH"), ("status", "OK"))
        check("SCRIPT EXISTS miss after flush", c.cmd("SCRIPT", "EXISTS", sha[1]),
              ("array", [("int", 0)]))
        check("EVALSHA after flush -> NOSCRIPT", c.cmd("EVALSHA", sha[1], "0"),
              ("error", "NOSCRIPT"))
    else:
        failed += 1
        print(f"[FAIL] SCRIPT LOAD  got={sha!r}")

    check("EVALSHA unknown -> NOSCRIPT",
          c.cmd("EVALSHA", "f" * 40, "0"), ("error", "NOSCRIPT"))

    c.close()
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin_binary())
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args(argv)

    binary = args.goblin_bin
    if not binary.exists():
        print(f"goblin-core binary not found: {binary}", file=sys.stderr)
        return 2

    port = free_port()
    proc = subprocess.Popen(
        [str(binary), "--port", str(port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_server(port, args.timeout)
        return run(port)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
