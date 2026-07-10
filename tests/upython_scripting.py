#!/usr/bin/env python3
"""End-to-end MicroPython scripting test: UPYTHON.EVAL / UPYTHON.EVALSHA /
UPYTHON.SCRIPT over a real socket, on the embedded MicroPython (ports/embed).
Also shows it is a distinct interpreter from the Lua-family, Wren, and Tcl
engines while sharing the key space.

Python has no top-level return, so a script produces its reply by assigning to
the global `reply` (like Wren's Redis.setReply_). redis.call(...) talks to the
store; KEYS/ARGV are 0-based lists.
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

    # Core UPYTHON.EVAL behavior + conversions (reply via the `reply` global).
    check("int", c.cmd("UPYTHON.EVAL", "reply = 1 + 2", "0"), ("int", 3))
    check("str", c.cmd("UPYTHON.EVAL", "reply = 'hello'", "0"), ("bulk", "hello"))
    check("no reply -> nil", c.cmd("UPYTHON.EVAL", "x = 5", "0"), ("nil", None))
    check("True -> 1", c.cmd("UPYTHON.EVAL", "reply = True", "0"), ("int", 1))
    check("False -> nil", c.cmd("UPYTHON.EVAL", "reply = False", "0"), ("nil", None))
    check("list -> array", c.cmd("UPYTHON.EVAL", "reply = [1, 2, 3]", "0"),
          ("array", [("int", 1), ("int", 2), ("int", 3)]))
    check("list comprehension", c.cmd("UPYTHON.EVAL", "reply = [x * x for x in range(4)]", "0"),
          ("array", [("int", 0), ("int", 1), ("int", 4), ("int", 9)]))
    check("KEYS 0-based", c.cmd("UPYTHON.EVAL", "reply = KEYS[0]", "1", "mykey"),
          ("bulk", "mykey"))
    check("ARGV generator", c.cmd("UPYTHON.EVAL", "reply = sum(int(x) for x in ARGV)", "0", "10", "20", "12"),
          ("int", 42))

    # The redis helpers.
    check("redis.status", c.cmd("UPYTHON.EVAL", "reply = redis.status('GOOD')", "0"),
          ("status", "GOOD"))
    check("redis.error", c.cmd("UPYTHON.EVAL", "reply = redis.error('My Error')", "0"),
          ("error", "My Error"))
    check("redis.sha1hex", c.cmd("UPYTHON.EVAL", "reply = redis.sha1hex('')", "0"),
          ("bulk", "da39a3ee5e6b4b0d3255bfef95601890afd80709"))

    # redis.call into the shared store.
    check("redis.call ZADD", c.cmd("UPYTHON.EVAL", "reply = redis.call('zadd', KEYS[0], 5, 'm')", "1", "pz"),
          ("int", 1))
    check("write visible to plain cmd", c.cmd("ZSCORE", "pz", "m"), ("bulk", "5"))
    check("redis.call ZSCORE", c.cmd("UPYTHON.EVAL", "reply = redis.call('zscore', KEYS[0], 'm')", "1", "pz"),
          ("bulk", "5"))
    check("try/except catches", c.cmd(
        "UPYTHON.EVAL",
        "try:\n redis.call('zscore')\nexcept Exception:\n reply = 'caught'", "0"),
        ("bulk", "caught"))
    check("uncaught call error", c.cmd("UPYTHON.EVAL", "reply = redis.call('zscore')", "0"),
          ("error", None))
    check("python exception -> error", c.cmd("UPYTHON.EVAL", "reply = 1 // 0", "0"),
          ("error", None))
    check("nested scripting blocked",
          c.cmd("UPYTHON.EVAL", "reply = redis.call('upython.eval', 'reply=1', '0')", "0"),
          ("error", None))

    # UPYTHON.SCRIPT lifecycle + cache isolation.
    sha = c.cmd("UPYTHON.SCRIPT", "LOAD", "reply = 42")
    if sha[0] == "bulk" and len(sha[1]) == 40:
        passed += 1
        print("[ok  ] UPYTHON.SCRIPT LOAD -> 40-hex sha")
        check("UPYTHON.EVALSHA runs cached", c.cmd("UPYTHON.EVALSHA", sha[1], "0"), ("int", 42))
        check("UPYTHON.SCRIPT EXISTS hit", c.cmd("UPYTHON.SCRIPT", "EXISTS", sha[1]), ("array", [("int", 1)]))
        check("Lua cache isolated", c.cmd("SCRIPT", "EXISTS", sha[1]), ("array", [("int", 0)]))
        check("UPYTHON.SCRIPT FLUSH", c.cmd("UPYTHON.SCRIPT", "FLUSH"), ("status", "OK"))
        check("UPYTHON.EVALSHA after flush -> NOSCRIPT", c.cmd("UPYTHON.EVALSHA", sha[1], "0"),
              ("error", "NOSCRIPT"))
    else:
        failed += 1
        print(f"[FAIL] UPYTHON.SCRIPT LOAD  got={sha!r}")
    check("UPYTHON.SCRIPT LOAD rejects syntax error", c.cmd("UPYTHON.SCRIPT", "LOAD", "def f("),
          ("error", None))

    # Distinct interpreter: Python syntax runs under UPYTHON.EVAL but is a syntax
    # error under the Lua-family, Wren, and Tcl engines.
    py = "reply = [x * x for x in range(4)]"
    check("python comprehension (UPYTHON)", c.cmd("UPYTHON.EVAL", py, "0"),
          ("array", [("int", 0), ("int", 1), ("int", 4), ("int", 9)]))
    check("python rejected by EVAL", c.cmd("EVAL", py, "0"), ("error", None))
    check("python rejected by WREN.EVAL", c.cmd("WREN.EVAL", py, "0"), ("error", None))
    check("python rejected by TCL.EVAL", c.cmd("TCL.EVAL", py, "0"), ("error", None))

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
