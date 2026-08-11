# NVIDIA BlueField Pub/Sub edge

`goblin-core-bluefield` is a small RESP-facing edge intended to run on the Arm
cores of an NVIDIA BlueField DPU. The ordinary `goblin-core` process remains
the authoritative server on the host. The edge owns no keyspace: it handles
Pub/Sub locally and proxies every ordinary data command to the host.

```text
RESP2/RESP3 clients
        |
        v
goblin-core-bluefield (DPU)
  | local SUBSCRIBE / PSUBSCRIBE / PUBLISH fanout
  | one aggregate SBE subscription + one SBE publisher
  | one private RESP upstream session per ordinary client
        |
        v
goblin-core --enable-sbe (host, authoritative keyspace)
```

The client-facing protocol on the DPU is always RESP. SBE is confined to the
private, same-version DPU-to-host Pub/Sub links. Ordinary proxied commands stay
RESP internally so their exact reply shape and connection state do not need a
second typed translation layer.

## Pub/Sub behavior

- `SUBSCRIBE`, `PSUBSCRIBE`, and their unsubscribe forms update the DPU-local
  registry. The first local interest in a channel or pattern creates one
  aggregate subscription on the host. Later local subscribers update a weight
  instead of creating more host-to-DPU delivery streams.
- A publication originating on the host crosses the DPU link once, even when
  many local clients or matching patterns receive it. The DPU recreates the
  proper RESP2 arrays or RESP3 pushes locally.
- A top-level `PUBLISH` received by the DPU queues local deliveries and attempts
  their non-blocking socket writes before it submits the publication upstream.
  A slow local subscriber is attempted but cannot hold up the host publication.
- The aggregate and publisher links share a nonzero edge ID. The host excludes
  that edge's aggregate link from an edge-originated publication, preventing a
  PCIe round trip and duplicate local delivery.
- Subscription weights keep `PUBLISH` and host-side `PUBSUB NUMSUB` counts equal
  to the number of logical clients, while retaining one host-to-DPU message.
  Literal and pattern subscription weights are counted independently.

`PUBLISH` inside `MULTI` and publications produced by scripts execute on the
authoritative host. They return through the aggregate subscription when they
run, preserving transaction and script semantics; the local-first fast path is
only for a top-level `PUBLISH`.

`PUBSUB` introspection issued to the edge describes the edge-local registry.
Host-side introspection includes the weighted edge subscribers.

## Ordinary command forwarding

Each RESP client gets a lazy, non-blocking RESP connection to the host. That
one-to-one mapping preserves connection-scoped behavior such as `MULTI`/`EXEC`,
`WATCH`, blocking commands, and RESP2 versus RESP3 replies. The edge terminates
the connection commands `AUTH`, `HELLO`, `CLIENT`, `SELECT 0`, `PING`, `ECHO`,
and `QUIT` locally. It does not cache or execute data commands on the DPU.

## Build

Build the authoritative server from the normal project on the host:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target goblin_core_server -j
```

The DPU has a separate, deliberately small build. It does not link the store,
scripting runtimes, Kafka, TLS, RDMA, or the generic Goblin client. It requires
a C++20 compiler plus the libsodium headers and library:

```sh
cmake -S bluefield -B build-bluefield -DCMAKE_BUILD_TYPE=Release
cmake --build build-bluefield -j
```

This produces `build-bluefield/goblin-core-bluefield`. On AArch64 the standalone
build defaults to Cortex-A72 code generation, release LTO, and statically linked
GNU C++ runtimes. Those defaults can be changed with
`GOBLIN_BLUEFIELD_TUNE_CORTEX_A72`, `GOBLIN_BLUEFIELD_LTO`, and
`GOBLIN_BLUEFIELD_STATIC_GNU_RUNTIME`. The compatibility headers under
`bluefield/compat` allow the native GCC 9 toolchain in older BlueField OS images
to build the edge.

For the native client-facing XLIO Ultra transport, build the pinned XLIO 3.61.2
and DPCP 1.1.61 runtime described in [Native XLIO Ultra TCP](xlio.md), then
enable the edge transport:

```sh
cmake -S bluefield -B build-bluefield-ultra \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_BLUEFIELD_ENABLE_XLIO=ON
cmake --build build-bluefield-ultra -j
```

The resulting executable retains `--listen` for kernel TCP and adds `--xlio`
for a directly polled Ultra listener. The upstream SBE and RESP relay sessions
remain ordinary non-blocking TCP because the management path is not the
client-facing latency boundary.

Installing this build also installs the Apache project license and notice, the
selected licenses for the vendored header dependencies, and the selected XLIO
and DPCP BSD license bundle under `share/doc`. Keep that directory with any
redistributed DPU binary or runtime image.

Build both binaries from the same commit. The private Pub/Sub control operations
and Goblin's SBE messages are lockstep interfaces; mixing releases is
unsupported. Client-facing compatibility does not have that constraint because
the DPU always speaks RESP.

The normal project can also build the edge and its socket integration test on a
development machine. BlueField support is off by default in the top-level
build, so enable it explicitly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_BUILD_BLUEFIELD=ON
cmake --build build --target goblin_core_server goblin_core_bluefield -j
ctest --test-dir build --output-on-failure -R bluefield
```

## Network and launch

Use two intentional paths in production:

- Bind `--xlio` to the DPU dataplane address reached by external clients. Use
  `--listen` only when deliberately selecting the kernel TCP listener.
- Bind the host's `--trusted-listen` and the edge's `--upstream` to an isolated
  DPU-to-host path. Prefer the BlueField dataplane/representor path for latency.

RShim `tmfifo_net0` is useful for installation and functional bring-up. It is a
management tunnel over the SoC management interface, and should not be treated
as a latency baseline. NVIDIA documents both the
[RShim management interface](https://docs.nvidia.com/networking/display/bluefielddpuosv470/Host-side%2BInterface%2BConfiguration)
and the [DPU-mode representors used between Arm applications and host PFs](https://docs.nvidia.com/networking/display/bluefielddpubspv420/kernel%2Brepresentors%2Bmodel).
A typical single-DPU management setup is:

```text
host tmfifo_net0: 192.168.100.1/24
DPU  tmfifo_net0: 192.168.100.2/24
```

Start the authoritative host server with SBE enabled and an explicitly trusted
plaintext listener only on that isolated link:

```sh
goblin-core \
  --enable-sbe \
  --trusted-listen HOST_TRANSIT_IP:6379 \
  --numa HOST_BLUEFIELD_PF \
  --numa-arena \
  --uds-listen /run/goblin-core/redis.sock
```

Then start the native Ultra edge on the DPU:

```sh
sudo env \
  XLIO_SPEC=latency \
  XLIO_MEM_ALLOC_TYPE=ANON \
  LD_LIBRARY_PATH=/opt/goblin-xlio/lib \
  LD_PRELOAD=/opt/goblin-xlio/lib/libxlio.so \
  goblin-core-bluefield \
  --xlio DPU_DATAPLANE_IP 6379 \
  --upstream HOST_TRANSIT_IP 6379 \
  --edge-id 1 \
  --cpu 2
```

Use `--listen DPU_DATAPLANE_IP 6379` instead when testing the kernel TCP
listener. `XLIO_MEM_ALLOC_TYPE=ANON` is required on the current lab DPU because
it has no reserved HugeTLB pages; a production image can instead reserve huge
pages before starting XLIO.

Every simultaneously connected DPU must use a unique edge ID. If `--edge-id`
is omitted, the process generates a nonzero ID at startup.

The complete edge options are:

```text
--listen ADDRESS PORT
--xlio ADDRESS PORT        (XLIO-enabled builds)
--upstream HOST PORT
--auth-file PATH
--upstream-auth-user USER
--upstream-auth-password-file PATH
--unsolicited-output-buffer-bytes BYTES
--max-output-buffer-bytes BYTES
--edge-id NONZERO-U64
--cpu N
--backlog N
```

The SBE links do not authenticate. Keep the host trusted listener restricted to
the point-to-point DPU fabric and enforce that boundary with routing and firewall
rules. Use `--auth-file` for client-facing RESP authentication. If the host also
requires RESP authentication, give the edge its upstream username and a
root-readable password file; those credentials apply to ordinary proxy sessions,
not to SBE.

For service supervision, run both processes with `Restart=on-failure`. The edge
fails startup if either SBE link cannot register, and exits on a fatal control or
relay error so the supervisor can reconnect it cleanly. DPU-local subscription
state is intentionally ephemeral and clients must resubscribe after a restart.

## Latency deployment notes

- The edge uses one busy-spinning event-loop core and does not sleep or poll in
  the steady state. Reserve an Arm core and select it with `--cpu`; expect that
  process to show approximately 100% utilization on the selected core.
- Keep the edge core out of general scheduler and IRQ work. Pin the load
  generator or client application to a different core when measuring it.
- The edge enables `TCP_NODELAY` and Linux `TCP_QUICKACK` on its TCP sessions.
  Client applications should also disable Nagle buffering when sending small
  Pub/Sub messages.
- Per-client unsolicited output rings are prefaulted and best-effort locked.
  Give the service enough `LimitMEMLOCK` for
  `connections * --unsolicited-output-buffer-bytes` to keep slow page faults out
  of publication fanout.
- Place the host process and memory on the NUMA node local to the BlueField host
  PF. `--numa DEVICE --numa-arena` does this when Linux exposes the device's NUMA
  node; use an explicit local `--cpu` or NUMA node when it does not.
- The hot publication path encodes one RESP2 and/or RESP3 frame per publication
  and attempts a direct non-blocking write when a receiving client has no older
  output. A short write queues only the unsent suffix in the fixed ring; an
  `EAGAIN` queues the whole frame. All local writes are attempted before one SBE
  publication is submitted to the host. A host publication crosses back once
  and fans out to every matching DPU client.

The native probe can be built and run directly on the DPU:

```sh
cmake -S bluefield -B build-bluefield -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_BLUEFIELD_BUILD_BENCHMARKS=ON
cmake --build build-bluefield -j
taskset -c 3 build-bluefield/goblin-bluefield-local-latency \
  DPU_DATAPLANE_IP 6379 1000 100
```

It measures client-observed time from writing `PUBLISH` on one TCP connection to
receiving the local subscriber frame on another. For end-to-end validation,
including exact subscriber accounting in both directions, use:

```sh
python3 benchmarks/bluefield_pubsub_latency.py \
  --edge-host DPU_DATAPLANE_IP --edge-port 6379 \
  --host-host HOST_TRANSIT_IP --host-port 6379
```

An XLIO-enabled root build also exposes matched kernel and XLIO Ultra Pub/Sub
modes in the existing latency probe. Both modes use two RESP2 connections and
the same payloads, timer, validation, CPU binding, warmup, and sample count. The
DPU edge transport is selected independently at launch. The current results use
the native XLIO Ultra listener (`--xlio`) on the DPU for both client modes:

```sh
numactl --cpunodebind=1 --membind=1 taskset -c 5 \
  build-xlio/goblin_core_xlio_latency_benchmark tcp-pubsub \
  DPU_DATAPLANE_IP 6379 bluefield-kernel 5000 500 CLIENT_IP

sudo env XLIO_MEM_ALLOC_TYPE=ANON XLIO_TRACELEVEL=WARNING \
  LD_LIBRARY_PATH="$XLIO_PREFIX/lib" LD_PRELOAD="$XLIO_PREFIX/lib/libxlio.so" \
  numactl --cpunodebind=1 --membind=1 taskset -c 5 \
  build-xlio/goblin_core_xlio_latency_benchmark xlio-pubsub \
  DPU_DATAPLANE_IP 6379 bluefield-xlio 5000 500 CLIENT_IP
```

The timer stops when the local subscriber receives and validates the RESP
message. The probe then validates the upstream `PUBLISH` reply before starting
the next sample, so host forwarding cannot be omitted without passing
unnoticed, but its reply latency is outside the local-fanout measurement.

### Userspace TCP on the DPU

BlueField exposes a network function directly to the Arm subsystem, so an Arm
application does not have to use kernel TCP. The current edge calls the XLIO
3.61.2 Ultra API directly for listener creation, accept, receive, send, and
polling. `LD_PRELOAD` supplies the XLIO runtime, but client-facing I/O does not
pass through its POSIX socket interposer. Clients still speak ordinary RESP over
TCP:

```sh
sudo env XLIO_SPEC=latency XLIO_MEM_ALLOC_TYPE=ANON \
  LD_LIBRARY_PATH=/opt/goblin-xlio/lib \
  LD_PRELOAD=/opt/goblin-xlio/lib/libxlio.so \
  goblin-core-bluefield \
    --xlio DPU_DATAPLANE_IP 6379 \
    --upstream HOST_TRANSIT_IP 6379 \
    --cpu 2
```

This uses the same physical BlueField port and cable. The private 3.61.2 runtime
and DPCP 1.1.61 are used instead of the older XLIO package in the DPU image.
DPDK supplies packet I/O rather than a complete Redis-compatible TCP stack;
using it directly would require a separate userspace TCP implementation and a
substantially larger integration.

On August 11, 2026, a ConnectX-5 client host was connected directly to
BlueField port p0 at 100 Gb/s with RS-FEC. The native Ultra edge processed
Pub/Sub on BlueField Arm CPU 2, and the client ran on CPU 5 local to its
ConnectX-5. With exactly one DPU edge process running, the current 5,000-sample
client comparison is:

| Client transport | p50 | p90 | p99 | p99.9 | Mean |
| --- | ---: | ---: | ---: | ---: | ---: |
| Kernel TCP | 107.8 us | 108.7 us | 116.5 us | 153.7 us | 108.1 us |
| Native XLIO Ultra | 12.5 us | 14.4 us | 19.3 us | 24.4 us | 12.9 us |

The XLIO client is the same native transport used in the earlier
ConnectX-5-to-ConnectX-5 experiment. The current DPU build writes an
uncontended subscriber frame directly and retains the fixed ring for partial
writes and backpressure. In the matched 100,000-sample `PING` control, kernel
TCP measured 27.6 us p50 and 32.6 us p99; XLIO Ultra measured 10.6 us p50 and
11.3 us p99. For the all-XLIO path, local Pub/Sub adds about 1.9 us to the
median for the second connection, RESP parsing, lookup, encoding, and fanout.

The concise current-state report is the
[BlueField DPU-side Pub/Sub benchmark](../BLUEFIELD-BENCHMARK.md). Its raw rows
and deployment metadata are in
[`benchmarks/bluefield_pubsub_2026-08-11.csv`](../benchmarks/bluefield_pubsub_2026-08-11.csv),
[`benchmarks/bluefield_ping_2026-08-11.csv`](../benchmarks/bluefield_ping_2026-08-11.csv),
and
[`benchmarks/bluefield_pubsub_2026-08-11-metadata.txt`](../benchmarks/bluefield_pubsub_2026-08-11-metadata.txt).
Cross-direction forwarding, RESP2/RESP3, literal/pattern fanout, partial-write
fallback, and exact subscriber accounting passed. Post-run PHY counters on
both ends showed zero CRC errors, corrected bits, and RX/TX discards.

## Smoke test

Subscribe through the DPU:

```sh
redis-cli -h DPU_DATAPLANE_IP SUBSCRIBE prices
```

Publish through the same DPU and then through a host-side UDS client:

```sh
redis-cli -h DPU_DATAPLANE_IP PUBLISH prices edge
redis-cli -s /run/goblin-core/redis.sock PUBLISH prices host
```

Both publications should arrive once. With multiple DPU subscribers, the host
publication still crosses the host-to-DPU link once, while its integer reply
counts every receiving client.
