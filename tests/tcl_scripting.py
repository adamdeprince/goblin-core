#!/usr/bin/env python3
"""End-to-end Tcl scripting test: TCL.EVAL / TCL.EVALSHA / TCL.SCRIPT over a real
socket, on the embedded Jim Tcl interpreter. Also shows Tcl is a distinct
interpreter from the Lua-family and Wren engines while sharing the key space.

Tcl is string-centric: the script result is the reply (a canonical integer
replies as an integer, everything else as a bulk string), and the `redis` command
provides `redis call`/`pcall` plus explicit reply builders. KEYS/ARGV are Tcl
lists (use `lindex`).
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

    # Core TCL.EVAL behavior + conversions.
    check("TCL.EVAL expr -> int", c.cmd("TCL.EVAL", "expr {1 + 2}", "0"), ("int", 3))
    check("TCL.EVAL return string", c.cmd("TCL.EVAL", "return hello", "0"), ("bulk", "hello"))
    check("TCL.EVAL set result", c.cmd("TCL.EVAL", "set x [expr {3 * 4}]", "0"), ("int", 12))
    check("TCL.EVAL KEYS lindex", c.cmd("TCL.EVAL", "lindex $KEYS 0", "1", "mykey"),
          ("bulk", "mykey"))
    check("TCL.EVAL ARGV loop", c.cmd(
        "TCL.EVAL", "set s 0; foreach n $ARGV { incr s $n }; return $s", "0", "10", "20", "12"),
        ("int", 42))

    # The `redis` reply builders.
    check("redis status", c.cmd("TCL.EVAL", "redis status GOOD", "0"), ("status", "GOOD"))
    check("redis error", c.cmd("TCL.EVAL", "redis error {My Error}", "0"), ("error", "My Error"))
    check("redis integer", c.cmd("TCL.EVAL", "redis integer 42", "0"), ("int", 42))
    check("redis nil", c.cmd("TCL.EVAL", "redis nil", "0"), ("nil", None))
    check("redis array", c.cmd("TCL.EVAL", "redis array {a b c}", "0"),
          ("array", [("bulk", "a"), ("bulk", "b"), ("bulk", "c")]))
    check("redis sha1hex", c.cmd("TCL.EVAL", "redis sha1hex {}", "0"),
          ("bulk", "da39a3ee5e6b4b0d3255bfef95601890afd80709"))

    # redis call into the shared store.
    check("redis call ZADD", c.cmd("TCL.EVAL", "redis call zadd [lindex $KEYS 0] 5 m", "1", "tz"),
          ("int", 1))
    check("write visible to plain cmd", c.cmd("ZSCORE", "tz", "m"), ("bulk", "5"))
    # The score comes back from redis call as the string "5"; returning it, the
    # value-based rule replies as an integer (a canonical integer -> :N).
    check("redis call ZSCORE (int-promoted)",
          c.cmd("TCL.EVAL", "redis call zscore [lindex $KEYS 0] m", "1", "tz"), ("int", 5))
    # A non-integer stays a bulk string.
    check("bulk string result", c.cmd("TCL.EVAL", "return 5.5", "0"), ("bulk", "5.5"))
    check("catch recovers error", c.cmd(
        "TCL.EVAL", "if {[catch {redis call zscore} e]} {return caught}", "0"),
        ("bulk", "caught"))
    check("tcl error -> error reply", c.cmd("TCL.EVAL", "error boom", "0"), ("error", None))
    check("nested scripting blocked",
          c.cmd("TCL.EVAL", "redis call tcl.eval {return 1} 0", "0"), ("error", None))

    # Sandbox: process- and host-touching commands are removed.
    check("exit removed", c.cmd("TCL.EVAL", "exit", "0"), ("error", None))
    check("source removed", c.cmd("TCL.EVAL", "source /etc/passwd", "0"), ("error", None))

    # TCL.SCRIPT lifecycle + cache isolation from the other interpreters.
    sha = c.cmd("TCL.SCRIPT", "LOAD", "return 42")
    if sha[0] == "bulk" and len(sha[1]) == 40:
        passed += 1
        print("[ok  ] TCL.SCRIPT LOAD -> 40-hex sha")
        check("TCL.EVALSHA runs cached", c.cmd("TCL.EVALSHA", sha[1], "0"), ("int", 42))
        check("TCL.SCRIPT EXISTS hit", c.cmd("TCL.SCRIPT", "EXISTS", sha[1]), ("array", [("int", 1)]))
        check("Lua cache isolated from Tcl", c.cmd("SCRIPT", "EXISTS", sha[1]), ("array", [("int", 0)]))
        check("TCL.SCRIPT FLUSH", c.cmd("TCL.SCRIPT", "FLUSH"), ("status", "OK"))
        check("TCL.EVALSHA after flush -> NOSCRIPT", c.cmd("TCL.EVALSHA", sha[1], "0"),
              ("error", "NOSCRIPT"))
    else:
        failed += 1
        print(f"[FAIL] TCL.SCRIPT LOAD  got={sha!r}")
    check("TCL.SCRIPT LOAD rejects unbalanced", c.cmd("TCL.SCRIPT", "LOAD", "set x {"),
          ("error", None))

    # Distinct interpreter: a Tcl one-liner runs under TCL.EVAL but is a syntax
    # error under the Lua-family and Wren engines.
    check("Tcl string command (TCL)",
          c.cmd("TCL.EVAL", "return [string toupper hello]", "0"), ("bulk", "HELLO"))
    check("Tcl rejected by EVAL", c.cmd("EVAL", "return [string toupper hello]", "0"),
          ("error", None))
    check("Tcl rejected by WREN.EVAL", c.cmd("WREN.EVAL", "return [string toupper hello]", "0"),
          ("error", None))

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
