# Libfabric RDM and AWS EFA

Goblin Core can carry its SBE command stream over a provider-neutral libfabric
`FI_EP_RDM` endpoint. Production deployments can select AWS EFA; the same code
can be qualified without an EFA instance through libfabric's TCP provider or,
on suitable local hardware, `verbs;ofi_rxm`.

This is a busy-polled transport. A libfabric target participates in the same
literal command-line priority order as shared-memory rings, native XLIO, and
polled RDMA rings. When a higher-priority target keeps making progress, it can
starve targets listed after it. Ordinary kernel sockets retain their separate
fairness-oriented pass.

The [100 Gb/s provider matrix](../EFA-LATENCY.md) compares RESP2 and SBE over
kernel TCP with both protocols over libfabric providers `tcp` and
`verbs;ofi_rxm`. It also exercises automatic small-frame injection and a
forced `fi_send`/completion-queue path. This is local ConnectX-5 hardware, not
AWS EFA hardware, and the report retains both the fast median and the tail.

## Build

Goblin vendors AWS's libfabric release under `third_party/libfabric` using the
BSD license option recorded in `NOTICE`. Build and install its static library
once:

```sh
./scripts/build-libfabric.sh "$HOME/.local/libfabric-goblin"
```

Then configure Goblin Core against that prefix:

```sh
cmake -S . -B build-efa -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ENABLE_LIBFABRIC=ON \
  -DGOBLIN_CORE_LIBFABRIC_ROOT="$HOME/.local/libfabric-goblin"
cmake --build build-efa --parallel
```

The helper enables the EFA, verbs/RxM, shared-memory, and TCP providers in one
static `libfabric.a`. EFA is included when its userspace headers and libraries
are present. Use the EFA installer supplied for the target EC2 image before
running the helper on an EFA host.

## Start a listener

`--efa ADDRESS PORT` is shorthand for an EFA provider target:

```sh
build-efa/goblin-core \
  --enable-sbe \
  --efa 10.0.1.20 6381 \
  --efa-heartbeat-timeout-ms 3000
```

`ADDRESS` selects the local interface and binds a short-lived TCP bootstrap
listener. `PORT` belongs to that bootstrap listener; Redis commands never use
the TCP connection. A connecting client reads the server's opaque libfabric
endpoint address and closes the socket, then the client hello and every command,
reply, heartbeat, and Pub/Sub push travel over `FI_EP_RDM`.

For qualification with another provider:

```sh
build-efa/goblin-core \
  --enable-sbe \
  --libfabric tcp 127.0.0.1 6381 \
  --efa-heartbeat-timeout-ms 600
```

Both options are repeatable. Their position among `--ring`, `--rdma`, and
`--xlio` options is their strict busy-poll priority. The heartbeat timeout
applies to every configured libfabric target despite the EFA-oriented option
name. A value of zero disables heartbeat expiry.

Small frames use `fi_inject()` by default when they fit the provider's inject
limit; larger frames use `fi_send()` and reclaim their registered transmit slot
from the TX completion queue. `--libfabric-force-send` disables the inject
shortcut for every libfabric target so all frames use `fi_send()` and TX
completions. This is useful when comparing providers under the exact same
`FI_EP_RDM` / `FI_MSG` / completion-queue contract:

```sh
build-efa/goblin-core \
  --enable-sbe \
  --libfabric 'verbs;ofi_rxm' 10.100.0.1 6381 \
  --libfabric-force-send
```

## C++ client

`SbeLibfabricClient` is the same compile-time-dispatched typed client API used
by the shared-memory and RDMA transports. `SbeEfaClient` is its EFA-oriented
alias:

```cpp
#include <goblin/core/sbe_ring_client.hpp>

std::string error;
auto client = goblin::core::SbeEfaClient::open(
    "efa", "10.0.1.20", 6381, std::chrono::seconds(5),
    128 * 1024, "10.0.1.21", &error);
if (!client || !client->ping()) {
  // handle connection failure
}
client->set("price:IBM", "128.03");
```

The client sends a transport PING automatically after one third of an otherwise
idle lease. Application traffic resets that timer. PONG frames are consumed
below the SBE layer, so heartbeats never appear as command replies.

## Ordering and client identity

One server `FI_EP_RDM` endpoint serves many logical clients. A client hello
carries a random 64-bit session identifier, its opaque endpoint address, its
Goblin Core version, and its requested reorder window. The server rejects an
incompatible Goblin version before creating the logical connection.

Every client-to-server frame has a per-client 64-bit sequence beginning at one.
Every server-to-client frame has a separate per-client 64-bit reply sequence.
Normal replies also echo the request sequence in `reply_to`; unsolicited
Pub/Sub and replication output uses zero.

The expected frame takes the fast path without reorder allocation. A future
sequence is sequestered until the missing frames arrive, then the contiguous
run is drained in order. An old sequence is discarded as a duplicate. The
default gap is bounded to 64 messages and 1 MiB; exceeding either bound closes
that logical session instead of allowing an unbounded network-controlled
allocation.

SBE command order and Pub/Sub delivery order therefore remain connection-local
even when the underlying reliable-datagram provider completes packets out of
order. The design does not impose ordering between independent clients.

## Compatibility and trust

The typed libfabric client uses Goblin's SBE protocol, while a raw libfabric
client may carry RESP2 inside Goblin's reliable-datagram envelope. The envelope
is version-specific in either case: client and server must run exactly the same
Goblin Core version. Use RESP over an ordinary socket transport when rolling
upgrades or independent client releases matter.

SBE has no authentication exchange. It belongs inside a trusted EFA, RDMA, or
host boundary whose edge performs authentication. `--no-auth-libfabric` also
marks RESP on a raw libfabric connection trusted, but it does not change SBE:
SBE is always trusted-fabric traffic.

## Qualification

The live test takes a provider, bootstrap address, and optional local source
address:

```sh
build-efa/goblin_core_libfabric_roundtrip_test \
  tcp 127.0.0.1 6381 - 2000 1200
```

It opens two logical clients, exercises PING and an 8 KiB SET/GET, pipelines
HSET/HGET at depth 64, verifies an unsolicited Pub/Sub delivery, idles both
clients, and confirms their automatic heartbeats preserved the sessions.

The cross-host matrix runner compares every locally available provider, wire
format, and send path, plus ordinary kernel TCP:

```sh
SAMPLES=200000 WARMUP=20000 \
  bash benchmarks/libfabric_provider_matrix.sh
```

Use `PROVIDERS`, `SEND_MODES`, and `WIRES` as comma-separated subsets when
qualifying one cell. The runner rejects a provider that cannot negotiate the
required `FI_EP_RDM`/`FI_MSG` contract instead of silently substituting one.
