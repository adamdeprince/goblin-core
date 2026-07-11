"""goblin_core -- a redis-py-shaped client that talks to goblin-core over a
shared-memory ring buffer instead of a socket.

    from goblin_core import Redis
    r = Redis("/tmp/a", decode_responses=True)   # server: goblin-core --ring /tmp/a 64kb
    r.set("user:42", "alice")
    r.get("user:42")            # 'alice'

The transport, the busy-poll (with the PAUSE/YIELD relax hint), and RESP parsing
live in the C++ extension (`_goblin_core`); this module is the ergonomic surface.

Only the redis-py methods whose commands goblin-core actually implements are
provided -- there is no `lpush`/`sadd`/`xadd`, because the server has no lists,
sets, or streams. A few `GOBLIN.*` native commands are exposed as extensions, and
`execute_command(*args)` reaches anything the server understands.
"""
from __future__ import annotations

from typing import Any, Iterable, Mapping, Optional, Sequence, Union

from . import _goblin_core
from ._goblin_core import ResponseError, RingError

__all__ = ["Redis", "StrictRedis", "ResponseError", "RingError"]

EncodableT = Union[str, bytes, bytearray, memoryview, int, float, bool]


def _encode(value: EncodableT) -> bytes:
    """Encode one command argument the way redis-py does."""
    if isinstance(value, bytes):
        return value
    if isinstance(value, (bytearray, memoryview)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, bool):
        return b"1" if value else b"0"
    if isinstance(value, int):
        return str(value).encode("ascii")
    if isinstance(value, float):
        return repr(value).encode("ascii")
    raise TypeError(f"cannot encode {type(value).__name__} as a redis argument")


def _decode_deep(obj: Any) -> Any:
    """Recursively decode bytes -> str (used when decode_responses=True)."""
    if isinstance(obj, bytes):
        return obj.decode("utf-8")
    if isinstance(obj, list):
        return [_decode_deep(x) for x in obj]
    if isinstance(obj, tuple):
        return tuple(_decode_deep(x) for x in obj)
    if isinstance(obj, dict):
        return {_decode_deep(k): _decode_deep(v) for k, v in obj.items()}
    return obj


def _as_str(v: Any) -> Any:
    return v.decode("utf-8") if isinstance(v, bytes) else v


def _num(v: Any, cast):
    return None if v is None else cast(_as_str(v))


class Redis:
    """A redis-py-compatible client over a goblin-core ring buffer.

    Unlike redis-py, the "address" is a ring file path, not host/port -- the
    client and server must share memory (same host). Start the server with
    ``goblin-core --ring <path> <size>`` first.
    """

    def __init__(
        self,
        ring_path: str,
        *,
        decode_responses: bool = False,
        connect_timeout: float = 2.0,
        command_timeout: float = 5.0,
    ) -> None:
        self._client = _goblin_core.Client(ring_path, int(connect_timeout * 1000))
        self._decode_responses = decode_responses
        self._timeout_ms = int(command_timeout * 1000)
        self._ring_path = ring_path

    def __repr__(self) -> str:
        return f"Redis(ring_path={self._ring_path!r}, decode_responses={self._decode_responses})"

    # -- the low-level path everything else is built on -------------------------
    def execute_command(self, *args: EncodableT) -> Any:
        """Encode and run one command; return the raw parsed reply (undecoded)."""
        return self._client.execute_command([_encode(a) for a in args], self._timeout_ms)

    def _decode(self, obj: Any) -> Any:
        return _decode_deep(obj) if self._decode_responses else obj

    # -- connection -------------------------------------------------------------
    def ping(self, message: Optional[EncodableT] = None) -> Any:
        if message is None:
            return self.execute_command("PING") == b"PONG"
        return self._decode(self.execute_command("PING", message))

    def echo(self, value: EncodableT) -> Any:
        return self._decode(self.execute_command("ECHO", value))

    def info(self, section: Optional[str] = None) -> dict:
        raw = self.execute_command("INFO", section) if section else self.execute_command("INFO")
        text = raw.decode("utf-8") if isinstance(raw, bytes) else str(raw)
        out: dict[str, Any] = {}
        for line in text.splitlines():
            if not line or line.startswith("#") or ":" not in line:
                continue
            key, _, val = line.partition(":")
            for cast in (int, float):
                try:
                    val = cast(val)  # type: ignore[assignment]
                    break
                except ValueError:
                    pass
            out[key] = val
        return out

    # -- strings ----------------------------------------------------------------
    def get(self, name: EncodableT) -> Any:
        return self._decode(self.execute_command("GET", name))

    def set(
        self,
        name: EncodableT,
        value: EncodableT,
        ex: Optional[int] = None,
        px: Optional[int] = None,
        exat: Optional[int] = None,
        pxat: Optional[int] = None,
        nx: bool = False,
        xx: bool = False,
        keepttl: bool = False,
        get: bool = False,
    ) -> Any:
        pieces: list[EncodableT] = ["SET", name, value]
        if ex is not None:
            pieces += ["EX", ex]
        if px is not None:
            pieces += ["PX", px]
        if exat is not None:
            pieces += ["EXAT", exat]
        if pxat is not None:
            pieces += ["PXAT", pxat]
        if nx:
            pieces.append("NX")
        if xx:
            pieces.append("XX")
        if keepttl:
            pieces.append("KEEPTTL")
        if get:
            pieces.append("GET")
        reply = self.execute_command(*pieces)
        if get:
            return self._decode(reply)  # old value or None
        return True if reply == b"OK" else None

    def getset(self, name: EncodableT, value: EncodableT) -> Any:
        return self._decode(self.execute_command("GETSET", name, value))

    def setnx(self, name: EncodableT, value: EncodableT) -> bool:
        return bool(self.execute_command("SETNX", name, value))

    def getdel(self, name: EncodableT) -> Any:
        return self._decode(self.execute_command("GETDEL", name))

    def strlen(self, name: EncodableT) -> int:
        return self.execute_command("STRLEN", name)

    def append(self, name: EncodableT, value: EncodableT) -> int:
        return self.execute_command("APPEND", name, value)

    def incr(self, name: EncodableT, amount: int = 1) -> int:
        return self.execute_command("INCRBY", name, amount)

    def incrby(self, name: EncodableT, amount: int = 1) -> int:
        return self.execute_command("INCRBY", name, amount)

    def decr(self, name: EncodableT, amount: int = 1) -> int:
        return self.execute_command("DECRBY", name, amount)

    def decrby(self, name: EncodableT, amount: int = 1) -> int:
        return self.execute_command("DECRBY", name, amount)

    def incrbyfloat(self, name: EncodableT, amount: float = 1.0) -> float:
        return _num(self.execute_command("INCRBYFLOAT", name, amount), float)

    def getrange(self, name: EncodableT, start: int, end: int) -> Any:
        return self._decode(self.execute_command("GETRANGE", name, start, end))

    def setrange(self, name: EncodableT, offset: int, value: EncodableT) -> int:
        return self.execute_command("SETRANGE", name, offset, value)

    def mset(self, mapping: Mapping[EncodableT, EncodableT]) -> bool:
        pieces: list[EncodableT] = ["MSET"]
        for key, value in mapping.items():
            pieces += [key, value]
        return self.execute_command(*pieces) == b"OK"

    def mget(self, keys: Union[EncodableT, Iterable[EncodableT]], *args: EncodableT) -> list:
        names = list(keys) if isinstance(keys, (list, tuple)) else [keys]
        names += list(args)
        return self._decode(self.execute_command("MGET", *names))

    # -- keys -------------------------------------------------------------------
    def delete(self, *names: EncodableT) -> int:
        return self.execute_command("DEL", *names)

    def exists(self, *names: EncodableT) -> int:
        return self.execute_command("EXISTS", *names)

    def expire(self, name: EncodableT, time: int) -> bool:
        return bool(self.execute_command("EXPIRE", name, time))

    def pexpire(self, name: EncodableT, time: int) -> bool:
        return bool(self.execute_command("PEXPIRE", name, time))

    def expireat(self, name: EncodableT, when: int) -> bool:
        return bool(self.execute_command("EXPIREAT", name, when))

    def pexpireat(self, name: EncodableT, when: int) -> bool:
        return bool(self.execute_command("PEXPIREAT", name, when))

    def ttl(self, name: EncodableT) -> int:
        return self.execute_command("TTL", name)

    def pttl(self, name: EncodableT) -> int:
        return self.execute_command("PTTL", name)

    def persist(self, name: EncodableT) -> bool:
        return bool(self.execute_command("PERSIST", name))

    def expiretime(self, name: EncodableT) -> int:
        return self.execute_command("EXPIRETIME", name)

    def pexpiretime(self, name: EncodableT) -> int:
        return self.execute_command("PEXPIRETIME", name)

    def type(self, name: EncodableT) -> str:
        return _as_str(self.execute_command("TYPE", name))

    # -- hashes -----------------------------------------------------------------
    def hset(
        self,
        name: EncodableT,
        key: Optional[EncodableT] = None,
        value: Optional[EncodableT] = None,
        mapping: Optional[Mapping[EncodableT, EncodableT]] = None,
    ) -> int:
        pieces: list[EncodableT] = ["HSET", name]
        if key is not None:
            pieces += [key, value]
        if mapping:
            for k, v in mapping.items():
                pieces += [k, v]
        if len(pieces) == 2:
            raise ValueError("hset needs a key/value pair or a mapping")
        return self.execute_command(*pieces)

    def hsetnx(self, name: EncodableT, key: EncodableT, value: EncodableT) -> bool:
        return bool(self.execute_command("HSETNX", name, key, value))

    def hget(self, name: EncodableT, key: EncodableT) -> Any:
        return self._decode(self.execute_command("HGET", name, key))

    def hmget(self, name: EncodableT, keys: Union[EncodableT, Iterable[EncodableT]],
              *args: EncodableT) -> list:
        fields = list(keys) if isinstance(keys, (list, tuple)) else [keys]
        fields += list(args)
        return self._decode(self.execute_command("HMGET", name, *fields))

    def hdel(self, name: EncodableT, *keys: EncodableT) -> int:
        return self.execute_command("HDEL", name, *keys)

    def hgetall(self, name: EncodableT) -> dict:
        flat = self.execute_command("HGETALL", name)
        it = iter(flat)
        return self._decode(dict(zip(it, it)))

    def hkeys(self, name: EncodableT) -> list:
        return self._decode(self.execute_command("HKEYS", name))

    def hvals(self, name: EncodableT) -> list:
        return self._decode(self.execute_command("HVALS", name))

    def hlen(self, name: EncodableT) -> int:
        return self.execute_command("HLEN", name)

    def hexists(self, name: EncodableT, key: EncodableT) -> bool:
        return bool(self.execute_command("HEXISTS", name, key))

    def hstrlen(self, name: EncodableT, key: EncodableT) -> int:
        return self.execute_command("HSTRLEN", name, key)

    def hincrby(self, name: EncodableT, key: EncodableT, amount: int = 1) -> int:
        return self.execute_command("HINCRBY", name, key, amount)

    # -- sorted sets ------------------------------------------------------------
    def zadd(
        self,
        name: EncodableT,
        mapping: Mapping[EncodableT, float],
        nx: bool = False,
        xx: bool = False,
        gt: bool = False,
        lt: bool = False,
        ch: bool = False,
        incr: bool = False,
    ) -> Any:
        if not mapping:
            raise ValueError("zadd needs a non-empty mapping")
        pieces: list[EncodableT] = ["ZADD", name]
        if nx:
            pieces.append("NX")
        if xx:
            pieces.append("XX")
        if gt:
            pieces.append("GT")
        if lt:
            pieces.append("LT")
        if ch:
            pieces.append("CH")
        if incr:
            pieces.append("INCR")
        for member, score in mapping.items():
            pieces += [score, member]  # redis order: score then member
        reply = self.execute_command(*pieces)
        return _num(reply, float) if incr else reply

    def zcard(self, name: EncodableT) -> int:
        return self.execute_command("ZCARD", name)

    def _zrange_reply(self, reply, withscores, score_cast_func):
        if not withscores:
            return self._decode(reply)
        it = iter(reply)
        pairs = [(m, score_cast_func(_as_str(s))) for m, s in zip(it, it)]
        return self._decode(pairs)

    def zrange(self, name: EncodableT, start: int, end: int, desc: bool = False,
               withscores: bool = False, score_cast_func=float) -> list:
        cmd = "ZREVRANGE" if desc else "ZRANGE"
        pieces: list[EncodableT] = [cmd, name, start, end]
        if withscores:
            pieces.append("WITHSCORES")
        return self._zrange_reply(self.execute_command(*pieces), withscores, score_cast_func)

    def zrevrange(self, name: EncodableT, start: int, end: int,
                  withscores: bool = False, score_cast_func=float) -> list:
        pieces: list[EncodableT] = ["ZREVRANGE", name, start, end]
        if withscores:
            pieces.append("WITHSCORES")
        return self._zrange_reply(self.execute_command(*pieces), withscores, score_cast_func)

    def zrank(self, name: EncodableT, value: EncodableT) -> Optional[int]:
        return self.execute_command("ZRANK", name, value)

    def zrevrank(self, name: EncodableT, value: EncodableT) -> Optional[int]:
        return self.execute_command("ZREVRANK", name, value)

    def zrem(self, name: EncodableT, *values: EncodableT) -> int:
        return self.execute_command("ZREM", name, *values)

    def zremrangebyscore(self, name: EncodableT, min: Any, max: Any) -> int:
        return self.execute_command("ZREMRANGEBYSCORE", name, min, max)

    def zscore(self, name: EncodableT, value: EncodableT) -> Optional[float]:
        return _num(self.execute_command("ZSCORE", name, value), float)

    # -- scripting --------------------------------------------------------------
    def eval(self, script: EncodableT, numkeys: int, *keys_and_args: EncodableT) -> Any:
        return self._decode(self.execute_command("EVAL", script, numkeys, *keys_and_args))

    def evalsha(self, sha: EncodableT, numkeys: int, *keys_and_args: EncodableT) -> Any:
        return self._decode(self.execute_command("EVALSHA", sha, numkeys, *keys_and_args))

    def script_load(self, script: EncodableT) -> Any:
        return _as_str(self.execute_command("SCRIPT", "LOAD", script))

    # -- GOBLIN.* native extensions (no redis-py equivalent) --------------------
    def cad(self, name: EncodableT, expected: EncodableT) -> int:
        """GOBLIN.CAD -- compare-and-delete a key."""
        return self.execute_command("GOBLIN.CAD", name, expected)

    def cas(self, name: EncodableT, expected: EncodableT, value: EncodableT) -> Any:
        """GOBLIN.CAS -- compare-and-set (keeps the TTL)."""
        return self._decode(self.execute_command("GOBLIN.CAS", name, expected, value))

    def caexpire(self, name: EncodableT, expected: EncodableT, ms: int) -> int:
        """GOBLIN.CAEXPIRE -- compare-and-renew a key's TTL."""
        return self.execute_command("GOBLIN.CAEXPIRE", name, expected, ms)

    def increx(self, name: EncodableT, seconds: int) -> int:
        """GOBLIN.INCREX -- increment, arming a TTL on the first write."""
        return self.execute_command("GOBLIN.INCREX", name, seconds)

    def incrbound(self, name: EncodableT, delta: int, maximum: int) -> int:
        """GOBLIN.INCRBOUND -- bounded increment (quota); -1 if it would exceed max."""
        return self.execute_command("GOBLIN.INCRBOUND", name, delta, maximum)

    def decrpos(self, name: EncodableT) -> int:
        """GOBLIN.DECRPOS -- decrement only while positive; -1 otherwise."""
        return self.execute_command("GOBLIN.DECRPOS", name)

    def hcad(self, name: EncodableT, field: EncodableT, expected: EncodableT) -> int:
        """GOBLIN.HCAD -- compare-and-delete a hash field."""
        return self.execute_command("GOBLIN.HCAD", name, field, expected)

    def hsetgt(self, name: EncodableT, field: EncodableT, value: float) -> int:
        """GOBLIN.HSETGT -- set a hash field only if greater (watermark)."""
        return self.execute_command("GOBLIN.HSETGT", name, field, value)

    def zwindow(self, name: EncodableT, now: float, window: float, limit: int,
                member: EncodableT) -> int:
        """GOBLIN.ZWINDOW -- sliding-window admit (1) or reject (0)."""
        return self.execute_command("GOBLIN.ZWINDOW", name, now, window, limit, member)

    def claim(self, claim_key: EncodableT, result_key: EncodableT, token: EncodableT,
              seconds: int) -> Any:
        """GOBLIN.CLAIM -- idempotency guard: "CLAIMED" or the prior result."""
        return self._decode(
            self.execute_command("GOBLIN.CLAIM", claim_key, result_key, token, seconds))


# redis-py exposes StrictRedis as an alias of Redis; keep the habit.
StrictRedis = Redis
