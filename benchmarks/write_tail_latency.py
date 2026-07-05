#!/usr/bin/env python3
"""Write-path tail-latency probe (see BENCHMARKS.md, "Write-Path Tail Latency").

Issues N individually-timed ZADDs over one connection to grow a single sorted
set from empty. There is no pipelining, so every round trip is one latency
sample and an occasional index rehash shows up as a spike. Prints
p50/p90/p99/p99.9/p99.99/max in microseconds plus a count of millisecond-scale
spikes. Works against Goblin Core or Redis:

    python3 write_tail_latency.py --host 127.0.0.1 --port 6379 --n 1000000

Pin the server and this client to separate cores for clean numbers, e.g.
`taskset -c 0 <server>` and `taskset -c 1 python3 write_tail_latency.py ...`.
"""
import argparse
import gc
import socket
import time


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6379)
    ap.add_argument("--n", type=int, default=1_000_000)
    ap.add_argument("--key", default="tail")
    args = ap.parse_args()

    s = socket.create_connection((args.host, args.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    key = args.key.encode()
    lat = [0.0] * args.n
    gc.disable()
    send, recv, clock = s.sendall, s.recv, time.perf_counter
    for i in range(args.n):
        m = b"m:%d" % i
        sc = b"%d" % (i % 1000)
        cmd = b"*4\r\n$4\r\nZADD\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n" % (
            len(key), key, len(sc), sc, len(m), m)
        t0 = clock()
        send(cmd)
        r = recv(32)
        while not r.endswith(b"\r\n"):
            c = recv(32)
            if not c:
                raise SystemExit("connection closed at op %d" % i)
            r += c
        lat[i] = (clock() - t0) * 1e6

    lat.sort()

    def pct(p: float) -> float:
        return lat[min(args.n - 1, int(args.n * p))]

    print(
        "n=%d p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f p99.99=%.1f max=%.1f us"
        % (args.n, pct(0.5), pct(0.9), pct(0.99), pct(0.999), pct(0.9999), lat[-1])
    )
    print(
        "ops>1ms=%d ops>5ms=%d"
        % (sum(1 for x in lat if x > 1000), sum(1 for x in lat if x > 5000))
    )


if __name__ == "__main__":
    main()
