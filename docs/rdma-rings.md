# Polled RDMA rings

Goblin Core can carry RESP or SBE between machines over a receiver-polled,
one-sided RDMA ring. The server handles it in the same strict-priority loop as a
local shared-memory ring: no receive completion, socket syscall, or interrupt is
part of the message path.

The transport is Linux-only and is built automatically when `libibverbs` and
`librdmacm` development files are installed. CMake reports
`RDMA polled-ring transport enabled` when it is present.

## Start a listener

`--rdma ADDRESS PORT SIZE` creates one RDMA-CM listener. The option is
repeatable and may be interleaved with `--ring` and `--exasock`:

```bash
goblin-core --cpu 77 \
  --ring /tmp/local-critical 64kb \
  --exasock 10.99.99.1 6379 \
  --rdma 10.88.88.1 6380 64kb \
  --ring /tmp/local-batch 1mb
```

Option order is poll order. The example scans the critical local ring, then the
ExaSock TCP target, then RDMA, then the batch ring, and services plain sockets
only when all polled targets are empty. Whenever a target makes progress, the
scan restarts at the first target. A continuously busy earlier target can
therefore starve every later target.

`SIZE` is the registered inbound-ring budget per peer and per direction. A
connection uses `floor(SIZE / 256)` slots plus a 128-byte header, rounded up to a
native page. At least two slots are required. Each accepted peer has an
independent RC queue pair and inbound mapping.

## Clients

The multi-transport redis-cli clone accepts `--rdma` with the same ring size as
the server-side target (interactive and `-f` pipe modes included):

```bash
redis-cli-ring --rdma 10.88.88.1 6380 64kb PING
redis-cli-ring --rdma 10.88.88.1 6380 64kb SET price:IBM 247.31
redis-cli-rdma 10.88.88.1 6380 64kb PING   # thin one-shot alias
```

The C++ RESP client is `goblin/core/rdma_client.hpp`:

```cpp
auto client = goblin::core::rdma::RdmaClient::open(
    "10.88.88.1", 6380, 64 * 1024);
auto reply = client->command({"GET", "price:IBM"});
```

For a deep pipeline, use `send_pipelined(args)`. If the request ring fills, it
drains ready replies before retrying, preventing the full-duplex deadlock where
the writer waits for request slots while the server waits for reply slots.

The typed client remains the same compile-time SBE implementation used by the
shared-memory ring, with a different concrete transport:

```cpp
auto client = goblin::core::SbeRdmaClient::open(
    std::string_view("10.88.88.1"), 6380, 64 * 1024);
double score = client->zscore("prices", "IBM").value();
```

Python (`-DGOBLIN_CORE_ENABLE_RDMA=ON` on the extension build):

```python
from goblin_core import RdmaRedis, HAS_RDMA
r = RdmaRedis("10.88.88.1", 6380, 64 * 1024, decode_responses=True)
r.ping()
```

RESP starts in RESP2 mode and accepts `HELLO 3`. When the server has
`--enable-sbe`, SBE begins with the existing `GOBLINS!` handshake. With an auth
file, RESP authenticates on RDMA unless `--no-auth-rdma` marks that fabric
trusted; SBE is always unauthenticated. No other protocol behavior is specific
to RDMA.

## Connection setup

RDMA-CM resolves the route and establishes one reliable-connected queue pair.
Each side registers one bidirectional control-and-inbound-ring region and passes
a 40-byte descriptor in CM private data:

- wire version and fixed geometry;
- virtual address and `rkey`;
- mapped byte length and slot count;
- a connection nonce reserved for later diagnostics.

Native InfiniBand may expose the full 56-byte CM private-data field even when an
application supplied 40 bytes. Goblin validates and decodes the fixed 40-byte
prefix and ignores only the provider-added tail.

The queue pair allocates no receive work requests. RDMA-CM is absent from the
established message path.

## Registered layout

All multibyte Goblin control words are little-endian.

| Offset | Bytes | Purpose |
|---:|---:|---|
| `0` | 8 | Last inbound sequence consumed by the local process. |
| `8..63` | 56 | Reserved; isolates the producer-read counter on one cache line. |
| `64` | 8 | Local destination for an explicit remote-credit RDMA read. |
| `72..127` | 56 | Reserved. |
| `128` | remainder | Fixed-stride message slots. |

Every slot is 256 bytes. Its 64-bit commit word is fixed at byte 248. A payload
of 1 to 192 bytes is right-aligned immediately before that word, so a two-byte
message issues a ten-byte RDMA write rather than writing the whole slot.

```text
slot + 0                         slot + 248        slot + 256
| reserved / prior bytes | payload | commit word |
                                      56-bit sequence | 8-bit length
```

The sender constructs `commit = (sequence << 8) | length` and posts one inline
RC RDMA WRITE containing the payload followed by the commit. The maximum
200-byte write fits in one packet even at InfiniBand's minimum active MTU. The
receiver acquire-loads the fixed commit word; an exact expected sequence
publishes the preceding payload, an older sequence means empty, and a future
sequence is a protocol failure. Consuming a slot release-stores the sequence at
header offset zero.

Sequence numbers start at one and use 56 bits. Slot reuse is therefore immune
to a stale commit from an earlier circuit around the ring.

## Credits and completions

The sender caches the peer's consumed sequence. The normal path compares its
next sequence with that cache and posts only the inline RDMA WRITE. It does not
read a peer-owned cache line on each message.

Only when the cached view says the remote ring is full does the sender issue a
signaled eight-byte RDMA READ of the peer's consumed counter. If space has not
advanced, the caller remains backpressured. This is exact flow control: an
unread slot is never overwritten.

Writes are signaled every 32 work requests, or more frequently if the HCA grants
a smaller send queue. A signaled credit read also retires all preceding writes
on the ordered RC queue. Completion polling exists for reclamation and error
detection, not for receiving messages.

## Memory and placement

The registered mapping is anonymous, page-rounded, zeroed, and prefaulted before
registration. Goblin binds it to the selected NUMA slice and verifies every
page's placement. The server's process-wide `mlockall` policy covers future
mappings; memory registration also pins the pages for DMA.

`--numa TARGET` accepts a node (`1` or `node1`), an ordinary network interface
(`eth0`), or an InfiniBand device (`mlx5_0`). `--cpu N` remains the exact-core
form and implicitly selects that CPU's node. Without either option, Goblin maps
each specific socket and RDMA bind address back to its local interface. Automatic
selection happens only when every discovered interface is on the same node.
An endpoint whose locality cannot be resolved is also ambiguous on a multi-node
host and requires an explicit selection.

If, for example, the socket NIC is on node 0 and the InfiniBand HCA is on node 1,
startup fails instead of silently putting one hot path across the interconnect.
The diagnostic prints every observed endpoint plus all available NUMA nodes,
CPU lists, NICs, and InfiniBand devices. The operator must then choose a slice
with `--numa` or `--cpu`. That explicit selection places server execution,
shared rings, and server-side registered RDMA rings on the chosen node; a device
on another node is reported as remote.

`--numa-arena` additionally gives later arena allocation a soft preference for
the selected slice. It is separate because allowing a large keyspace to spill is
usually safer than binding all server memory strictly to one node.

The protection domain and per-connection `rkey` expose only this small mapping,
not the keyspace or arenas. This transport assumes a trusted RDMA fabric; it does
not add encryption or authentication.

## Lab fleet (hardware)

Latency numbers below come from a **small fleet** of identical servers used as
**server** and **client** (roles, not hostnames). Equipment matters more than
names:

| | |
| --- | --- |
| Chassis | **Dell PowerEdge R820** |
| CPUs | **4× Intel Xeon E5-4657L v2** @ 2.40 GHz |
| Topology | **4 sockets**, **12 cores/socket**, 2 threads/core (96 logical CPUs), **4 NUMA nodes** |
| Host fabric | QPI between sockets (ACPI distances 10 / 20 / 30 for local / 1 hop / 2 hops) |
| HCA | **Mellanox Connect-IB** (MCB191A-FCAT, `mlx5`, device ID `15b3:1011`) |
| Switch | **Mellanox SwitchX** |
| Link | 4× **FDR10**, **40 Gb/s**, MTU 4096 |

These are not top-bin desktop parts; they are multi-socket rack servers with a
generation-old InfiniBand fabric. That is intentional: the interesting claim is
that Goblin's polled RDMA path stays **close to this box's own inter-socket
fabric**, not that it beats a modern single-socket workstation's L3.

Bring-up detail (firmware, OpenSM, IPoIB addressing) is in
[InfiniBand setup](infiniband-setup.md).

## Validation

Cross-host RDMA was validated with the **client** driving the **server** on the
fleet above: one million pipelined RESP requests over a 64 KiB ring; a separate
100,000-request run with batches of 200 through a 4 KiB (16-slot) ring (credit
reads, wraparound, reply draining); RESP2, `HELLO 3`, typed SBE, fragmented
8 KiB SET/GET, and rapid reconnect cycles — without sequence, credit,
completion, CM, or server errors. The executable is
`goblin_core_rdma_roundtrip_test` (not default CTest; needs two fabric hosts).

A native 500,000-sample depth-one run (both processes on HCA-local cores)
measured complete Goblin `PING` request/reply round trips over the RDMA ring
(not bare verbs):

| Wire over RDMA ring | Minimum | p50 | p90 | p99 | p99.9 | Mean | Mean rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| SBE | 2.338 us | 2.495 us | 2.525 us | 3.170 us | 5.799 us | 2.526 us | 395,863 ops/s |
| RESP2 | 2.564 us | 2.673 us | 2.718 us | 2.859 us | 7.982 us | 2.706 us | 369,561 ops/s |

Raw RC RDMA-write RTT on the same fabric was about 1.0–1.5 µs; Goblin SBE sits
roughly a microsecond above that primitive. The probe is
`goblin_core_rdma_latency_benchmark`.

## Source map

- [`rdma_ring.hpp`](../include/goblin/core/rdma_ring.hpp): transport interface and
  fixed geometry (**payload is opaque bytes** — RESP or SBE live above this).
- [`rdma_ring.cpp`](../src/rdma_ring.cpp): CM setup, registration, queue pairs,
  sequence publication, and credits.
- [`rdma_client.hpp`](../include/goblin/core/rdma_client.hpp): RESP client over
  RDMA rings (ordinary RESP stream; only the transport changes).
- [`sbe_ring_client.hpp`](../include/goblin/core/sbe_ring_client.hpp): shared
  compile-time SBE client for local and RDMA rings.
- [`rdma_cli.cpp`](../src/rdma_cli.cpp): `redis-cli-rdma`.
- [`rdma_roundtrip_test.cpp`](../tests/rdma_roundtrip_test.cpp): cross-host stress
  and protocol validation.
- [`rdma_latency_benchmark.cpp`](../benchmarks/rdma_latency_benchmark.cpp): native
  depth-one RESP and SBE latency over RDMA (PING, SET, GET, HSET, HGET).
- [`ring_latency_benchmark.cpp`](../benchmarks/ring_latency_benchmark.cpp): the
  **same ops** over a local shared-memory ring.
- [`ring_numa_latency.sh`](../benchmarks/ring_numa_latency.sh): local ring with
  client at 0 / 1 / 2 NUMA hops (host fabric).
- [`fabric_vs_rdma_latency.sh`](../benchmarks/fabric_vs_rdma_latency.sh): NUMA hops
  **and** InfiniBand RDMA in one table.

## Machine fabric vs InfiniBand RDMA

**Protocol vs transport:** Goblin's application protocol is still **RESP or SBE**
(same as on a local ring or TCP). RDMA is only how those bytes move — receiver-
polled one-sided ring slots via RC WRITE — not a different command language.

**Claim:** Goblin's polled InfiniBand path is in the same league as an **R820
talking to itself across QPI** (the host fabric), for the same depth-one ops.
It is not “as fast as two threads on one die,” and it is not meant to be
compared only to same-node IPC.

### How to measure

```bash
# Full story: same-node, 1-hop, 2-hop local ring + client→server RDMA
SAMPLES=50000 WARMUP=5000 SKIP_BUILD=1 bash benchmarks/fabric_vs_rdma_latency.sh

# Pieces:
bash benchmarks/ring_numa_latency.sh       # local ring × NUMA hops (on one host)
bash benchmarks/rdma_thunder_butterfly.sh  # RDMA client ↔ server on the fleet
```

### Example (R820 fleet; 50k samples)

**Server** pinned to one NUMA node. Local-ring **client** moves across sockets;
RDMA **client** is a second R820 on the Mellanox SwitchX fabric (40 Gb/s).

| op | same-node p50 | 1-hop p50 | 2-hop p50 | **RDMA p50** | RDMA − 2-hop |
| --- | ---: | ---: | ---: | ---: | ---: |
| SBE PING | 0.47 µs | 1.51 µs | 1.94 µs | **~2.5 µs** | **~0.6 µs** |
| RESP PING | 0.68 µs | 1.71 µs | 2.12 µs | **~2.7 µs** | **~0.6 µs** |
| RESP SET | 0.84 µs | 1.89 µs | 2.30 µs | **~3.0–3.6 µs** | **~0.7–1.3 µs** |
| RESP HSET | 1.06 µs | 2.03 µs | 2.47 µs | **~3.3 µs** | **~0.8 µs** |

**How to read it**

| Column | Meaning |
| --- | --- |
| same-node | Best-case busy-polled shared memory on one socket. |
| 1-hop / 2-hop | What **this R820 already pays** when the peer is another socket (QPI). |
| RDMA | Cross-host polled InfiniBand (Goblin RDMA ring; RESP/SBE unchanged). |
| **RDMA − 2-hop** | How much farther IB + Goblin RDMA is than the host's **far** fabric hop. |

On this fleet, **RDMA sits about half a microsecond to one microsecond above
two-hop NUMA** for depth-one PING/HGET-class ops — next to the machine's own
fabric, not an order of magnitude beyond it. Same-node IPC (~0.5 µs) is ideal
local IPC, not a fair stand-in for “going across the box.”
