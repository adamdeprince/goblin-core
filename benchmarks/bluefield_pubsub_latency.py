#!/usr/bin/env python3
"""Validate a BlueField edge and measure client-observed Pub/Sub delivery latency."""

from __future__ import annotations

import argparse
import math
import socket
import statistics
import time
from dataclasses import dataclass
from typing import Any


def encode_command(*parts: str | bytes) -> bytes:
    encoded = [part.encode() if isinstance(part, str) else part for part in parts]
    out = bytearray(f"*{len(encoded)}\r\n".encode())
    for part in encoded:
        out.extend(f"${len(part)}\r\n".encode())
        out.extend(part)
        out.extend(b"\r\n")
    return bytes(out)


class RespConnection:
    def __init__(self, host: str, port: int) -> None:
        self.socket = socket.create_connection((host, port), timeout=5.0)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = bytearray()

    def close(self) -> None:
        self.socket.close()

    def send(self, *parts: str | bytes) -> None:
        self.socket.sendall(encode_command(*parts))

    def _fill(self, count: int) -> None:
        while len(self.buffer) < count:
            chunk = self.socket.recv(64 * 1024)
            if not chunk:
                raise RuntimeError("RESP peer closed the connection")
            self.buffer.extend(chunk)

    def _line(self) -> bytes:
        while True:
            end = self.buffer.find(b"\r\n")
            if end >= 0:
                line = bytes(self.buffer[:end])
                del self.buffer[: end + 2]
                return line
            self._fill(len(self.buffer) + 1)

    def read(self) -> Any:
        self._fill(1)
        marker = chr(self.buffer[0])
        del self.buffer[0]
        if marker == "+":
            return self._line().decode()
        if marker in ("-", "!"):
            raise RuntimeError(self._line().decode(errors="replace"))
        if marker == ":":
            return int(self._line())
        if marker == ",":
            return float(self._line())
        if marker == "#":
            return self._line() == b"t"
        if marker == "_":
            if self._line():
                raise RuntimeError("malformed RESP null")
            return None
        if marker in ("$", "="):
            length = int(self._line())
            if length < 0:
                return None
            self._fill(length + 2)
            value = bytes(self.buffer[:length])
            if self.buffer[length : length + 2] != b"\r\n":
                raise RuntimeError("malformed RESP bulk string")
            del self.buffer[: length + 2]
            return value
        if marker in ("*", ">", "~"):
            count = int(self._line())
            if count < 0:
                return None
            return [self.read() for _ in range(count)]
        if marker in ("%", "|"):
            count = int(self._line())
            return [(self.read(), self.read()) for _ in range(count)]
        raise RuntimeError(f"unsupported RESP marker {marker!r}")


def expected_message(channel: bytes, payload: bytes) -> list[Any]:
    return [b"message", channel, payload]


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


@dataclass
class Distribution:
    minimum: float
    median: float
    p90: float
    p99: float
    maximum: float
    mean: float

    @classmethod
    def from_ns(cls, values: list[int]) -> "Distribution":
        micros = [value / 1_000.0 for value in values]
        return cls(
            minimum=min(micros),
            median=statistics.median(micros),
            p90=percentile(micros, 0.90),
            p99=percentile(micros, 0.99),
            maximum=max(micros),
            mean=statistics.fmean(micros),
        )

    def render(self) -> str:
        return (
            f"min={self.minimum:.1f} p50={self.median:.1f} "
            f"p90={self.p90:.1f} p99={self.p99:.1f} "
            f"max={self.maximum:.1f} mean={self.mean:.1f} us"
        )


def run(args: argparse.Namespace) -> None:
    edge_subscriber = RespConnection(args.edge_host, args.edge_port)
    edge_publisher = RespConnection(args.edge_host, args.edge_port)
    host_subscriber = RespConnection(args.host_host, args.host_port)
    host_publisher = RespConnection(args.host_host, args.host_port)
    connections = [edge_subscriber, edge_publisher, host_subscriber, host_publisher]
    channel = args.channel.encode()
    payload = b"x" * args.payload_bytes
    try:
        edge_subscriber.send("SUBSCRIBE", channel)
        edge_ack = edge_subscriber.read()
        host_subscriber.send("SUBSCRIBE", channel)
        host_ack = host_subscriber.read()
        if edge_ack != [b"subscribe", channel, 1] or host_ack != [
            b"subscribe",
            channel,
            1,
        ]:
            raise RuntimeError(f"unexpected subscription acknowledgements: {edge_ack!r}, {host_ack!r}")

        host_publisher.send("PUBSUB", "NUMSUB", channel)
        if host_publisher.read() != [channel, 2]:
            raise RuntimeError("host did not account for the BlueField subscriber")

        local_samples: list[int] = []
        host_to_edge_samples: list[int] = []
        total = args.warmup + args.iterations
        for index in range(total):
            message_payload = payload + str(index).encode()
            started = time.perf_counter_ns()
            edge_publisher.send("PUBLISH", channel, message_payload)
            local_push = edge_subscriber.read()
            delivered = time.perf_counter_ns()
            host_push = host_subscriber.read()
            publish_count = edge_publisher.read()
            if local_push != expected_message(channel, message_payload):
                raise RuntimeError(f"unexpected local push: {local_push!r}")
            if host_push != expected_message(channel, message_payload):
                raise RuntimeError(f"unexpected host push: {host_push!r}")
            if publish_count != 2:
                raise RuntimeError(f"unexpected edge PUBLISH count: {publish_count!r}")
            if index >= args.warmup:
                local_samples.append(delivered - started)

        for index in range(total):
            message_payload = payload + b"h" + str(index).encode()
            started = time.perf_counter_ns()
            host_publisher.send("PUBLISH", channel, message_payload)
            edge_push = edge_subscriber.read()
            delivered = time.perf_counter_ns()
            host_push = host_subscriber.read()
            publish_count = host_publisher.read()
            if edge_push != expected_message(channel, message_payload):
                raise RuntimeError(f"unexpected host-to-edge push: {edge_push!r}")
            if host_push != expected_message(channel, message_payload):
                raise RuntimeError(f"unexpected host-local push: {host_push!r}")
            if publish_count != 2:
                raise RuntimeError(f"unexpected host PUBLISH count: {publish_count!r}")
            if index >= args.warmup:
                host_to_edge_samples.append(delivered - started)

        print(f"validated RESP forwarding and exact two-subscriber accounting on {channel.decode()!r}")
        print(f"edge PUBLISH -> edge subscriber: {Distribution.from_ns(local_samples).render()}")
        print(f"host PUBLISH -> edge subscriber: {Distribution.from_ns(host_to_edge_samples).render()}")
    finally:
        for connection in connections:
            connection.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--edge-host", required=True)
    parser.add_argument("--edge-port", type=int, default=6379)
    parser.add_argument("--host-host", required=True)
    parser.add_argument("--host-port", type=int, default=6379)
    parser.add_argument("--channel", default="benchmark:bluefield")
    parser.add_argument("--payload-bytes", type=int, default=32)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--iterations", type=int, default=1_000)
    args = parser.parse_args()
    if args.payload_bytes < 0 or args.warmup < 0 or args.iterations <= 0:
        parser.error("payload bytes and warmup must be non-negative; iterations must be positive")
    return args


if __name__ == "__main__":
    run(parse_args())
