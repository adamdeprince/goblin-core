#!/usr/bin/env python3
"""End-to-end Wren scripting test: WREN.EVAL / WREN.EVALSHA / WREN.SCRIPT over a
real socket, plus proof that Wren is a distinct interpreter from the Lua-family
engines while sharing the same key space.

Wren is class-based with no top-level `return`, so a script's body runs inside a
function and hands its result back through `Redis.setReply_`; `Redis.call` takes a
List of arguments (Wren has no varargs), and KEYS/ARGV are 0-based Lists.
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

    # Core WREN.EVAL behavior + conversions (value returned via Fn + setReply_).
    check("WREN.EVAL int", c.cmd("WREN.EVAL", "return 1", "0"), ("int", 1))
    check("WREN.EVAL string", c.cmd("WREN.EVAL", 'return "hi"', "0"), ("bulk", "hi"))
    check("WREN.EVAL float truncates", c.cmd("WREN.EVAL", "return 3.9", "0"), ("int", 3))
    check("WREN.EVAL no return -> nil", c.cmd("WREN.EVAL", "var x = 5", "0"), ("nil", None))
    check("WREN.EVAL true->1", c.cmd("WREN.EVAL", "return true", "0"), ("int", 1))
    check("WREN.EVAL false->nil", c.cmd("WREN.EVAL", "return false", "0"), ("nil", None))
    check("WREN.EVAL list->array", c.cmd("WREN.EVAL", "return [1,2,3]", "0"),
          ("array", [("int", 1), ("int", 2), ("int", 3)]))
    check("WREN.EVAL KEYS 0-based", c.cmd("WREN.EVAL", "return KEYS[0]", "1", "mykey"),
          ("bulk", "mykey"))
    check("WREN.EVAL KEYS+ARGV", c.cmd("WREN.EVAL", "return [KEYS[0], ARGV[0]]", "1", "k", "a"),
          ("array", [("bulk", "k"), ("bulk", "a")]))
    # Module "main" is reused across scripts; a second call must still work.
    check("WREN module reuse", c.cmd("WREN.EVAL", "return 2 + 3", "0"), ("int", 5))

    # Reply markers.
    check("WREN Redis.status", c.cmd("WREN.EVAL", 'return Redis.status("GOOD")', "0"),
          ("status", "GOOD"))
    check("WREN Redis.error", c.cmd("WREN.EVAL", 'return Redis.error("My Error")', "0"),
          ("error", "My Error"))
    check("WREN Redis.sha1hex", c.cmd("WREN.EVAL", 'return Redis.sha1hex("")', "0"),
          ("bulk", "da39a3ee5e6b4b0d3255bfef95601890afd80709"))

    # redis.call into the shared store (List of args; there are no varargs).
    check("WREN Redis.call PING", c.cmd("WREN.EVAL", 'return Redis.call(["ping"])', "0"),
          ("status", "PONG"))
    check("WREN Redis.call ZADD", c.cmd("WREN.EVAL", 'return Redis.call(["zadd", KEYS[0], 5, "m"])', "1", "wz"),
          ("int", 1))
    check("write visible to plain cmd", c.cmd("ZSCORE", "wz", "m"), ("bulk", "5"))
    check("WREN Redis.call ZSCORE", c.cmd("WREN.EVAL", 'return Redis.call(["zscore", KEYS[0], "m"])', "1", "wz"),
          ("bulk", "5"))
    check("WREN pcall catches", c.cmd(
        "WREN.EVAL", 'var r = Redis.pcall(["zscore"])\nif (r["err"] != null) return "caught"', "0"),
        ("bulk", "caught"))
    check("WREN call error raises", c.cmd("WREN.EVAL", 'return Redis.call(["zscore"])', "0"),
          ("error", None))
    check("WREN compile error", c.cmd("WREN.EVAL", "this is @@ not wren", "0"), ("error", None))
    check("WREN nested scripting blocked",
          c.cmd("WREN.EVAL", 'return Redis.call(["wren.eval", "return 1", "0"])', "0"), ("error", None))

    # WREN.SCRIPT lifecycle + cache isolation from the Lua engines.
    sha = c.cmd("WREN.SCRIPT", "LOAD", "return 42")
    if sha[0] == "bulk" and len(sha[1]) == 40:
        passed += 1
        print("[ok  ] WREN.SCRIPT LOAD -> 40-hex sha")
        check("WREN.EVALSHA runs cached", c.cmd("WREN.EVALSHA", sha[1], "0"), ("int", 42))
        check("WREN.SCRIPT EXISTS hit", c.cmd("WREN.SCRIPT", "EXISTS", sha[1]), ("array", [("int", 1)]))
        check("Lua caches isolated from Wren", c.cmd("SCRIPT", "EXISTS", sha[1]), ("array", [("int", 0)]))
        check("WREN.SCRIPT FLUSH", c.cmd("WREN.SCRIPT", "FLUSH"), ("status", "OK"))
        check("WREN.EVALSHA after flush -> NOSCRIPT", c.cmd("WREN.EVALSHA", sha[1], "0"),
              ("error", "NOSCRIPT"))
    else:
        failed += 1
        print(f"[FAIL] WREN.SCRIPT LOAD  got={sha!r}")

    # Distinct interpreter: Wren dialect (method blocks, class-based) is neither
    # PUC-Lua nor Luau. A Wren list-comprehension chain runs under WREN.EVAL but
    # is a syntax error under EVAL / LUAU.EVAL.
    check("Wren map/where chain (WREN)",
          c.cmd("WREN.EVAL", "return [1,2,3,4].where{|x| x % 2 == 0}.toList", "0"),
          ("array", [("int", 2), ("int", 4)]))
    check("Wren syntax rejected by EVAL",
          c.cmd("EVAL", "return [1,2,3,4].where{|x| x % 2 == 0}.toList", "0"), ("error", None))
    check("Wren syntax rejected by LUAU.EVAL",
          c.cmd("LUAU.EVAL", "return [1,2,3,4].where{|x| x % 2 == 0}.toList", "0"), ("error", None))

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
