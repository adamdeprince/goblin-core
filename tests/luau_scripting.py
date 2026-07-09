#!/usr/bin/env python3
"""End-to-end Luau scripting test: LUAU.EVAL / LUAU.EVALSHA / LUAU.SCRIPT over a
real socket, plus proof that the Luau interpreter is distinct from the PUC-Lua
one driven by EVAL (different dialect, different standard library, separate
script cache) while sharing the same key space.
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

    # Core LUAU.EVAL behavior + conversions.
    check("LUAU.EVAL int", c.cmd("LUAU.EVAL", "return 1", "0"), ("int", 1))
    check("LUAU.EVAL string", c.cmd("LUAU.EVAL", "return 'hi'", "0"), ("bulk", "hi"))
    check("LUAU.EVAL array", c.cmd("LUAU.EVAL", "return {1,2,3}", "0"),
          ("array", [("int", 1), ("int", 2), ("int", 3)]))
    check("LUAU.EVAL KEYS/ARGV", c.cmd("LUAU.EVAL", "return {KEYS[1], ARGV[1]}", "1", "k", "a"),
          ("array", [("bulk", "k"), ("bulk", "a")]))
    check("LUAU status_reply", c.cmd("LUAU.EVAL", "return redis.status_reply('GOOD')", "0"),
          ("status", "GOOD"))
    check("LUAU error_reply", c.cmd("LUAU.EVAL", "return redis.error_reply('My Error')", "0"),
          ("error", "My Error"))
    check("LUAU sha1hex", c.cmd("LUAU.EVAL", "return redis.sha1hex('')", "0"),
          ("bulk", "da39a3ee5e6b4b0d3255bfef95601890afd80709"))

    # redis.call into the shared store.
    check("LUAU redis.call PING", c.cmd("LUAU.EVAL", "return redis.call('ping')", "0"),
          ("status", "PONG"))
    check("LUAU redis.call ZADD", c.cmd("LUAU.EVAL", "return redis.call('zadd', KEYS[1], 1, 'a')", "1", "z"),
          ("int", 1))
    check("write visible to plain cmd", c.cmd("ZSCORE", "z", "a"), ("bulk", "1"))
    check("LUAU pcall catches", c.cmd(
        "LUAU.EVAL", "local r = redis.pcall('zscore') if r.err then return 'caught' end", "0"),
        ("bulk", "caught"))
    check("LUAU redis.call error raises", c.cmd("LUAU.EVAL", "return redis.call('zscore')", "0"),
          ("error", None))
    check("LUAU compile error", c.cmd("LUAU.EVAL", "this is not lua", "0"), ("error", None))
    check("LUAU nested scripting blocked",
          c.cmd("LUAU.EVAL", "return redis.call('luau.eval','return 1','0')", "0"), ("error", None))

    # LUAU.SCRIPT lifecycle + cache isolation from the PUC engine.
    sha = c.cmd("LUAU.SCRIPT", "LOAD", "return 42")
    if sha[0] == "bulk" and len(sha[1]) == 40:
        passed += 1
        print("[ok  ] LUAU.SCRIPT LOAD -> 40-hex sha")
        check("LUAU.EVALSHA runs cached", c.cmd("LUAU.EVALSHA", sha[1], "0"), ("int", 42))
        check("LUAU.SCRIPT EXISTS hit", c.cmd("LUAU.SCRIPT", "EXISTS", sha[1]), ("array", [("int", 1)]))
        check("PUC SCRIPT cache isolated", c.cmd("SCRIPT", "EXISTS", sha[1]), ("array", [("int", 0)]))
        check("LUAU.SCRIPT FLUSH", c.cmd("LUAU.SCRIPT", "FLUSH"), ("status", "OK"))
        check("LUAU.EVALSHA after flush -> NOSCRIPT", c.cmd("LUAU.EVALSHA", sha[1], "0"),
              ("error", "NOSCRIPT"))
    else:
        failed += 1
        print(f"[FAIL] LUAU.SCRIPT LOAD  got={sha!r}")

    # Distinct interpreters: Luau dialect + stdlib vs PUC-Lua 5.1.
    check("Luau type annotation works (LUAU)", c.cmd("LUAU.EVAL", "local x: number = 7 return x", "0"),
          ("int", 7))
    check("Luau type annotation rejected (EVAL)", c.cmd("EVAL", "local x: number = 7 return x", "0"),
          ("error", None))
    check("bit32 in LUAU", c.cmd("LUAU.EVAL", "return bit32.band(6,3)", "0"), ("int", 2))
    check("bit in EVAL", c.cmd("EVAL", "return bit.band(6,3)", "0"), ("int", 2))
    check("bit32 absent under EVAL", c.cmd("EVAL", "return bit32.band(6,3)", "0"), ("error", None))
    check("bit absent under LUAU", c.cmd("LUAU.EVAL", "return bit.band(6,3)", "0"), ("error", None))

    c.close()
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin_binary())
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args(argv)

    if not args.goblin_bin.exists():
        print(f"goblin-core binary not found: {args.goblin_bin}", file=sys.stderr)
        return 2

    port = free_port()
    proc = subprocess.Popen(
        [str(args.goblin_bin), "--port", str(port)],
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
