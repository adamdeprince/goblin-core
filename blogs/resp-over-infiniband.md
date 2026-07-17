# Redis commands between 16-year-old machines in 2.7 microseconds

**I built a Redis-compatible server whose network transport is approaching the
latency scale of a four-socket machine's internal fabric.**

Another physical server over my $80 InfiniBand card is only about **26% slower**
than talking to a CPU two NUMA hops away inside the same machine: 2.673 µs
instead of 2.12 µs, a difference of just 553 nanoseconds.

Redis semantics do not have to stop at a TCP socket. Goblin Core can carry
ordinary RESP2 or RESP3 between machines over a receiver-polled, one-sided
InfiniBand RDMA ring. The commands and replies are still RESP. `GET`, `HSET`,
`ZADD`, pipelines, and `HELLO 3` keep their normal wire representation; only
the transport underneath them changes.

Each connection owns a small registered request ring and reply ring. A sender
posts one inline reliable-connected RDMA write containing the RESP payload and
its sequence-word commit. The receiver polls that commit in local memory and
executes the command when the expected sequence appears. There is no socket
syscall, receive completion, interrupt, or RDMA-CM work in the established
message path. An explicit eight-byte RDMA read is needed only when the sender's
cached credits say the remote ring is full.

The result is a Redis-shaped request/reply path across two physical machines:

```bash
goblin-core --cpu 77 --rdma 10.88.88.1 6380 64kb
redis-cli-ring --rdma 10.88.88.1 6380 64kb SET price:IBM 247.31
redis-cli-ring --rdma 10.88.88.1 6380 64kb GET price:IBM
```

## Complete round-trip latency

This is a depth-one test, not throughput hidden behind a deep pipeline. Client
and server were pinned to HCA-local cores and exchanged 500,000 complete Goblin
`PING` request/reply round trips through a 64 KiB RDMA ring.

| Wire over the RDMA ring | Minimum | p50 | p90 | p99 | p99.9 | Mean | Mean rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| RESP2 | 2.564 µs | **2.673 µs** | 2.718 µs | 2.859 µs | 7.982 µs | 2.706 µs | 369,561 ops/s |
| SBE | 2.338 µs | **2.495 µs** | 2.525 µs | 3.170 µs | 5.799 µs | 2.526 µs | 395,863 ops/s |

RESP `SET` lands around 3.0–3.6 µs at p50 on the same fleet, and RESP `HSET`
around 3.3 µs. SBE is available on the identical transport when a typed client
is appropriate, but RESP is the important compatibility result: the fast
fabric does not require applications to adopt a different command language.

## Why are these numbers not lower?

Because the lab is built from roughly 16-year-old equipment. Both ends are
four-socket Dell PowerEdge R820 servers connected through Mellanox Connect-IB
adapters and a Mellanox SwitchX at 4x FDR10, 40 Gb/s. These are old
multi-socket rack systems, not current single-socket CPUs paired with modern
HCAs and switches. A 2.7 µs network round trip would not be remarkable on a
modern low-latency fabric; it matters here because of where this old hardware
started.

Raw reliable-connected RDMA-write round-trip latency on this fabric is already
about 1.0–1.5 µs. Goblin's complete SBE request, command dispatch, execution,
reply, and client decode sit roughly one microsecond above that primitive.

That is the useful result. Even on hardware this old, moving a normal RESP
conversation to another server costs only 553 ns more than moving between
distant sockets inside the server. Newer equipment should lower the fabric
floor; these measurements describe the old lab honestly rather than projecting
numbers from hardware we did not test.

The complete ring layout, credit protocol, NUMA placement rules, clients, and
reproduction commands are in [Polled RDMA rings](../docs/rdma-rings.md). Hardware
bring-up is documented separately in
[InfiniBand setup](../docs/infiniband-setup.md).
