# goblin-core Transport and UDP Fast-Path Roadmap

Design intent for TODO / ongoing networking work.

## Overview

Two related long-term efforts:

1. A pluggable transport layer with six I/O driver backends behind one interface.
2. A connectionless UDP fast-path protocol for latency-critical, loss-tolerant
   operations.

These are not launch blockers. goblin ships continuously; this is roadmap work
to build incrementally. The primary near-term driver is the FPGA NIC hardware
already on hand: Cisco/Exablaze ExaNIC-class cards with about 9 MB of on-card
RAM.

## Part 1: Pluggable Transport Layer

### Core Principle

The engine core talks to exactly one transport interface and never knows which
driver is behind it. The contract is roughly:

- `poll()` / wait for events
- `recv(conn) -> bytes`
- `send(conn, bytes)`
- connection lifecycle: accept / close

Get this interface right before writing any driver. This is the load-bearing
decision. A leaky interface means every driver claws into the engine core; a
clean interface means each driver is a self-contained module.

### Two-World Split

The six drivers divide into two categories, and the interface must span both.

Stream world:

- epoll
- io_uring
- Solarflare/exasock

These are handed reliable ordered byte streams. TCP framing is done for us. The
RESP parser sees clean bytes.

Raw-packet world:

- AF_XDP
- DPDK
- raw libexanic

These are handed ethernet frames. There is no TCP. We must supply reliable
ordered delivery ourselves.

Consequence: the raw-packet drivers share one common userspace TCP stack
sub-layer. Build it once and reuse it across AF_XDP, DPDK, and libexanic. Do
not write TCP logic per driver. All six drivers ultimately present streams to
the engine.

### Driver Build Order

Chosen for learning curve and dependency reasons: one new hard concept per
step.

1. epoll

   Baseline and reference implementation. Defines the interface.

   Correctness oracle: every other driver must produce byte-identical behavior
   to epoll. Validate with the existing zset content-hash check:

   ```sh
   ZRANGE ... WITHSCORES | paste - - | LC_ALL=C sort | sha256sum
   ```

2. io_uring

   Still stream sockets, but with the new completion/submission queue model. No
   TCP stack is needed. Kernel-version gated; this bites the Loongson 4.19 box.

3. Solarflare/exasock and libexanic-as-sockets

   Vendor kernel-bypass, still sockets API, minimal engine change. This is the
   near-term FPGA-NIC target: Level 1, use the vendor userspace library without
   FPGA programming.

4. AF_XDP

   First raw-packet driver. Build the shared userspace TCP stack here. Expect
   this step to be larger than steps 1 through 3 combined.

5. DPDK

   Full bypass. Reuses the userspace TCP stack from AF_XDP. New plumbing:
   hugepages, PMD, NIC binding. Packet concepts are familiar by this point.

6. libexanic raw mode

   Deepest and most hardware-specific path: raw ExaNIC frames plus userspace
   TCP. Bridges toward eventual Level 2 FPGA offload, where the hot read path
   may someday move into silicon.

### Regression Discipline

Every driver must:

- validate against the epoll reference with the content-hash oracle
- confirm RSS is unaffected; transport should not change data footprint
- catch SIMD probe and packet-parse bugs as wrong contents through the hash
  check

## Part 2: UDP Fast-Path Protocol

### Rationale

HFT is loss-tolerant and latency-obsessed. UDP's value is no handshake and no
connection state: the first packet carries payload, and the server holds zero
per-connection state. For single-datagram idempotent operations this is
near-ideal and far cheaper than TCP setup.

This is a goblin extension, not RESP. RESP stays TCP-only.

### Protocol

The protocol is intentionally stupid simple and stateless.

Request:

```text
<token><space><redis command string>
```

The token is opaque, space-free, and client-chosen. It is used for matching
outstanding requests. Everything up to the first space is the token. Everything
after it is the command.

Response:

```text
<token><separator><answer>
```

The response is one UDP datagram to the sender's source IP:port. Echo the token
and answer only. Do not echo the command.

The server is stateless: packet in, parse, execute, packet out, forget. No
connection table, no sequence numbers, and nothing to clean up. This
statelessness is what makes it flyable on raw frames and eventually on the
FPGA.

### Contract and Semantics

All of this is the caller's responsibility and must be documented plainly:

- best effort
- no ordering guarantee
- no delivery guarantee
- no deduplication
- idempotency is the caller's problem
- MTU is the caller's problem

Absolute-value operations, such as `ZADD key <score> <member>` and reads, are
safe to resend. Non-idempotent operations such as `ZINCRBY` are not protected:
resending after a lost reply double-applies the operation. Rule: if `INCRBY`
semantics must be reliable, accept the loss, have a great network, or use TCP.

Requests and replies must stay under the MTU.

### Deliberate Design Decisions

Oversize reply handling:

If a reply will not fit in one datagram, do not fragment and do not silently
truncate. Silent truncation is dangerous because the client gets a partial
answer and may think it is complete.

Send an explicit "too big, use TCP" error marker in the reply, or drop the
response. An explicit marker is preferred so the client knows to fall back.

Open question: pick the error-marker format.

Trusted-network-only:

Source IP:port is spoofable, which makes this a reflection/amplification vector:
a small request can cause a larger reply toward a spoofed third party.

Do not add anti-spoofing; it would require state and kill the point. Instead,
document bluntly that the UDP fast path is for trusted or isolated networks
only, such as HFT cross-connects. Public-internet exposure turns the server into
an amplifier flooding some spoofed third party's network and can lead to ISP
disconnection and abuse reports.

## Synergy Between Transport and UDP

UDP over raw-packet drivers does not need the userspace TCP stack. UDP over raw
frames is just header parsing and payload handoff, with no state machine and no
retransmission.

The leanest, lowest-latency, least-code path through the entire driver stack is
UDP over libexanic: raw frames, no TCP stack, no handshake, single-datagram
request/reply. This is the eventual HFT apex path, and it is easier to build
than the reliable-TCP raw-packet drivers because it deletes the hard part.

UDP fast-path should route through raw-packet drivers while bypassing the
userspace-TCP sub-layer.

## Path Routing

The client or client library chooses per operation:

- hot single-datagram idempotent lookups: UDP
- everything else: reliable TCP / RESP path

Large replies, non-idempotent operations, transactions, and bulk workloads stay
on TCP.

## Near-Term io_uring Starting Point

The current server is a single `poll()` loop over TCP or Unix stream sockets,
with per-client RESP parser state, output buffers, and read backpressure.

For io_uring, preserve the stream-world contract:

- the engine continues to receive ordered bytes per connection
- RESP parsing and command execution remain shared
- output buffering and backpressure semantics stay byte-identical to the
  current reference path
- the driver boundary owns accept, recv, send, close, and readiness/completion
  mechanics

The first implementation target is not raw performance. It is a clean transport
interface plus an epoll/poll reference driver that keeps current behavior
unchanged, followed by an io_uring driver behind the same interface.
