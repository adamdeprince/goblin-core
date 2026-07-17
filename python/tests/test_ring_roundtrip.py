"""End-to-end test: start goblin-core with a ring, drive it through goblin_core.Redis.

    PYTHONPATH=python python python/tests/test_ring_roundtrip.py [path-to-goblin-core]

Exercises the redis-py-compatible surface and asserts redis-py return conventions.
"""
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "python"))

from goblin_core import Redis, ResponseError  # noqa: E402


def start_server(binary, ring_paths, sock_path):
    cmd = [binary, "--unixsocket", sock_path]
    for ring_path in ring_paths:
        cmd += ["--ring", ring_path, "64kb"]
    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    return proc


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "goblin-core")
    if not os.path.exists(binary):
        print(f"goblin-core binary not found: {binary}", file=sys.stderr)
        return 2

    tmp = tempfile.mkdtemp(prefix="goblin-py-")
    ring_path = os.path.join(tmp, "ring")
    # A ring is a single-writer channel and the SBE handshake is per-connection, so a
    # second concurrent client needs its own ring rather than sharing the first one.
    ring2_path = os.path.join(tmp, "ring2")
    sock_path = os.path.join(tmp, "sock")
    proc = start_server(binary, [ring_path, ring2_path], sock_path)
    try:
        r = Redis(ring_path, connect_timeout=5.0)

        # connection
        assert r.ping() is True
        assert r.echo("hi") == b"hi"

        # strings
        assert r.set("k", "v") is True
        assert r.get("k") == b"v"
        assert r.get("missing") is None
        assert r.set("k", "v2", nx=True) is None          # NX fails, key exists
        assert r.get("k") == b"v"
        assert r.incr("n") == 1
        assert r.incr("n") == 2
        assert r.incrby("n", 5) == 7
        assert r.decr("n") == 6
        assert r.setnx("s", "1") is True
        assert r.setnx("s", "2") is False
        assert r.append("a", "xy") == 2
        assert r.strlen("a") == 2
        assert r.getrange("a", 0, 0) == b"x"
        assert r.mset({"x": "1", "y": "2"}) is True
        assert r.mget(["x", "y", "z"]) == [b"1", b"2", None]
        assert abs(r.incrbyfloat("f", 1.5) - 1.5) < 1e-9

        # keys / ttl
        assert r.exists("k") == 1
        assert r.exists("k", "missing") == 1
        assert r.type("k") == "string"
        assert r.type("missing") == "none"
        assert r.expire("k", 100) is True
        assert 90 < r.ttl("k") <= 100
        assert r.persist("k") is True
        assert r.ttl("k") == -1
        assert r.delete("k") == 1
        assert r.exists("k") == 0

        # hashes
        assert r.hset("h", "f", "1") == 1
        assert r.hset("h", mapping={"g": "2", "j": "3"}) == 2
        assert r.hget("h", "f") == b"1"
        assert r.hlen("h") == 3
        assert r.hexists("h", "g") is True
        assert r.hincrby("h", "f", 4) == 5
        assert r.hgetall("h") == {b"f": b"5", b"g": b"2", b"j": b"3"}
        assert sorted(r.hkeys("h")) == [b"f", b"g", b"j"]
        assert r.hmget("h", ["f", "nope"]) == [b"5", None]
        assert r.hdel("h", "f") == 1

        # sets
        assert r.sadd("s", "a", "b", "c") == 3
        assert r.sadd("s", "a") == 0
        assert r.scard("s") == 3
        assert r.sismember("s", "b") is True
        assert r.smismember("s", ["a", "z"]) == [b"1", b"0"] or r.smismember(
            "s", ["a", "z"]
        ) == ["1", "0"]
        assert r.smembers("s") == {b"a", b"b", b"c"} or r.smembers("s") == {
            "a",
            "b",
            "c",
        }
        assert r.srem("s", "a") == 1
        assert r.sadd("s2", "b", "c", "d") == 3
        assert r.sinter("s", "s2")  # non-empty intersection
        assert r.sunionstore("su", "s", "s2") >= 3
        cur, batch = r.sscan("s2", 0, count=10)
        assert cur == 0
        assert len(batch) == 3

        # sorted sets
        assert r.zadd("z", {"a": 1, "b": 2, "c": 3}) == 3
        assert r.zcard("z") == 3
        assert r.zscore("z", "b") == 2.0
        assert r.zscore("z", "nope") is None
        assert r.zrange("z", 0, -1) == [b"a", b"b", b"c"]
        assert r.zrange("z", 0, -1, withscores=True) == [(b"a", 1.0), (b"b", 2.0), (b"c", 3.0)]
        assert r.zrange("z", 0, -1, desc=True) == [b"c", b"b", b"a"]
        assert r.zrank("z", "b") == 1
        assert r.zrevrank("z", "b") == 1
        assert r.zrem("z", "a") == 1
        assert r.zremrangebyscore("z", 2, 3) == 2

        # scripting
        assert r.eval("return 1", 0) == 1
        sha = r.script_load("return 42")
        assert r.evalsha(sha, 0) == 42

        # info
        info = r.info()
        assert isinstance(info, dict) and len(info) > 0

        # GOBLIN.* native extensions
        r.set("q", "0")
        assert r.incrbound("q", 3, 10) == 3
        assert r.incrbound("q", 100, 10) == -1
        assert r.hsetgt("wm", "high", 100) == 1
        assert r.hsetgt("wm", "high", 50) == 0
        r.set("lock", "tokenA")
        assert r.cad("lock", "wrong") == 0
        assert r.cad("lock", "tokenA") == 1
        assert r.claim("cl", "cl:r", "worker-1", 30) == b"CLAIMED"

        # error mapping: a type mismatch raises ResponseError (like redis-py)
        r.set("str", "v")
        try:
            r.execute_command("ZADD", "str", "1", "x")
            raise AssertionError("expected a ResponseError for WRONGTYPE")
        except ResponseError as exc:
            assert "WRONGTYPE" in str(exc)

        # decode_responses gives str out (its own ring -- see ring2_path above)
        r2 = Redis(ring2_path, decode_responses=True)
        r2.set("d", "hello")
        assert r2.get("d") == "hello"
        assert r2.type("d") == "string"
        assert r2.zadd("dz", {"m": 9}) == 1
        assert r2.zrange("dz", 0, -1, withscores=True) == [("m", 9.0)]

        print("goblin_core redis-py roundtrip OK")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
