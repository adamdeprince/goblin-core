# Aeron UDP and IPC

Goblin Core can carry RESP or its typed SBE protocol over
[Aeron](https://github.com/aeron-io/aeron) reliable UDP and IPC. Both modes use
Aeron response channels: a client publishes requests on one channel and stream,
and the server gives each incoming publication image a response publication
correlated to that client's response subscription. Independent clients therefore
retain independent Goblin connection state, reply order, authentication state,
transactions, and Pub/Sub subscriptions.

Aeron is an opt-in external dependency. Goblin links Aeron's static C client,
while an Aeron Media Driver runs as a separate local process. IPC requires the
server and client to use the same driver and directory. UDP uses a local driver
on each host; the server and each client connect only to their own driver.

## Build Aeron and Goblin Core

Aeron added IPC response channels in 1.49. Goblin requires 1.51 or newer for the
C client's cancelable asynchronous-registration lifecycle. The helper pins Aeron
1.51.0 and verifies its release commit:

```sh
./scripts/build-aeron.sh "$HOME/opt/aeron-1.51.0-$(uname -m)"

cmake -S . -B build-aeron -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ENABLE_AERON=ON \
  -DGOBLIN_CORE_AERON_ROOT="$HOME/opt/aeron-1.51.0-$(uname -m)"
cmake --build build-aeron --parallel
```

`GOBLIN_CORE_ENABLE_AERON` defaults to `OFF`. If Aeron is already installed in a
standard CMake prefix, omit `GOBLIN_CORE_AERON_ROOT` and make it discoverable
through `CMAKE_PREFIX_PATH`. An Aeron-enabled Goblin install exports the Aeron
dependency to downstream CMake consumers.

## Start the Media Driver

Run the C Media Driver before the server or clients. The static executable avoids
runtime shared-library search-path setup. Goblin's latency-oriented C++ clients
continuously poll their response subscriptions and configure their Aeron client
conductor with the `spin` idle strategy. Use Aeron's corresponding low-latency
profile for the external Media Driver so its agents do not amplify a short
application pause through exponential backoff:

```sh
AERON_PREFIX="$HOME/opt/aeron-1.51.0-$(uname -m)"
AERON_DIR=/run/user/$(id -u)/goblin-aeron

env \
  AERON_CONDUCTOR_IDLE_STRATEGY=spin \
  AERON_SENDER_IDLE_STRATEGY=noop \
  AERON_RECEIVER_IDLE_STRATEGY=noop \
  AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND=2 \
  "$AERON_PREFIX/bin/aeronmd_s" -Daeron.dir="$AERON_DIR"
```

Here `noop` means the sender or receiver immediately begins its next agent
iteration; it does not disable that agent. The conductor's `spin` strategy also
polls continuously but issues the architecture's processor-relax hint. These
settings reserve a core for each dedicated agent. For UDP, apply them to the
Media Driver on both hosts; IPC has one shared local driver. The C++ client
cannot change an already-running external driver's idle strategy.

The `--aeron-dir` argument on Goblin and `aeron_directory` client argument must
name that same directory. Omitting them selects Aeron's platform/user default.
Do not start two Media Drivers on one directory.

## Server targets

IPC needs only two signed 32-bit stream identifiers:

```sh
build-aeron/goblin-core \
  --enable-sbe \
  --aeron-dir "$AERON_DIR" \
  --aeron-ipc 1001 1002
```

UDP pairs a request endpoint/channel with a response-control endpoint/channel:

```sh
build-aeron/goblin-core \
  --enable-sbe \
  --aeron-dir "$AERON_DIR" \
  --aeron-udp 10.20.0.10:40123 2001 10.20.0.11:40124 2002
```

The complete forms are:

```text
--aeron-ipc REQUEST-STREAM RESPONSE-STREAM
--aeron-udp REQUEST-CHANNEL REQUEST-STREAM RESPONSE-CHANNEL RESPONSE-STREAM
--aeron-dir PATH
```

A bare UDP request value becomes `aeron:udp?endpoint=VALUE`; a bare response
value becomes `aeron:udp?control=VALUE`. Complete Aeron URIs are accepted, so an
operator can add `interface`, `mtu`, term-length, or other supported channel
parameters:

```sh
--aeron-udp \
  'aeron:udp?endpoint=10.20.0.10:40123|interface=10.20.0.10' 2001 \
  'aeron:udp?control=10.20.0.11:40124|interface=10.20.0.10' 2002
```

Goblin adds `control-mode=response` and the per-client
`response-correlation-id`; callers must not supply the correlation parameter.
One UDP target has one response-control channel. Clients sharing a Media Driver
can share that target; clients whose drivers listen at different response
addresses should use separately configured channel/stream pairs.

The options are repeatable and participate in literal busy-poll priority with
`--ring`, `--rdma`, `--libfabric`, `--exasock`, and `--xlio`. The first ready
target restarts the scan and can starve later targets. Any polled target keeps the
server core busy by design.

## C++ clients

The typed client is the same compile-time-dispatched API as every other SBE
transport:

```cpp
#include <goblin/core/aeron_transport.hpp>
#include <goblin/core/sbe_ring_client.hpp>

using namespace std::chrono_literals;

auto channels = goblin::core::aeron::ChannelConfig::udp(
    "10.20.0.10:40123", 2001, "10.20.0.11:40124", 2002);
std::string error;
auto client = goblin::core::SbeAeronClient::open(
    channels, 5s, 64 * 1024, "/run/user/1000/goblin-aeron", &error);
if (client) {
  client->set("price:IBM", "128.03");
}
```

`ChannelConfig::ipc(request_stream, response_stream)` selects IPC. For RESP2 or
RESP3 framing, use `goblin/core/aeron_client.hpp`:

```cpp
auto client = goblin::core::aeron::AeronClient::open(
    goblin::core::aeron::ChannelConfig::ipc(1001, 1002),
    5s, "/run/user/1000/goblin-aeron", &error);
auto pong = client->command({"PING"});
```

Aeron fragments messages larger than its frame payload and Goblin reassembles
them into one transport record before parsing. A message larger than the
publication's advertised maximum message length fails locally.

## Python wrapper

Build the nanobind extension against the same Aeron installation:

```sh
cmake -S python -B python/build-aeron \
  -DPython_EXECUTABLE="$(command -v python3)" \
  -DGOBLIN_CORE_ENABLE_AERON=ON \
  -DGOBLIN_CORE_AERON_ROOT="$AERON_PREFIX"
cmake --build python/build-aeron --parallel
```

The high-level clients retain the redis-py-shaped API:

```python
from goblin_core import AeronIpcRedis, AeronUdpRedis, HAS_AERON

ipc = AeronIpcRedis(
    request_stream_id=1001,
    response_stream_id=1002,
    aeron_directory="/run/user/1000/goblin-aeron",
)

udp = AeronUdpRedis(
    "10.20.0.10:40123",
    "10.20.0.11:40124",
    request_stream_id=2001,
    response_stream_id=2002,
    aeron_directory="/run/user/1000/goblin-aeron",
    decode_responses=True,
)
```

`HAS_AERON` is false in a normal extension build and true when the native Aeron
transport is present.

## Security and persistence

Aeron provides transport reliability, not encryption or peer authentication.
Keep UDP and IPC channels inside an isolated host/fabric boundary or add security
below/around that boundary. SBE never has an authentication exchange and still
requires `--enable-sbe`. With `--auth-file`, RESP over Aeron authenticates by
default; `--no-auth-aeron` explicitly trusts RESP on every configured Aeron
target.

The Aeron C client owns a conductor thread. Goblin therefore disables fork-based
`BGSAVE` and `GOBLIN.DUMPWORLD` when any Aeron target is configured. Synchronous
`SAVE` remains available.

## Local transport benchmark

The local latency matrix compares depth-one `PING`, `SET`, and `GET` round trips
for RESP and SBE over a shared-memory ring, Aeron IPC, Aeron loopback UDP, and a
Unix-domain socket. Every case gets a fresh server. IPC shares one Media Driver;
UDP uses separate local server/client drivers so it traverses the kernel
loopback path.

```sh
AERON_PREFIX="$HOME/opt/aeron-1.51.0-$(uname -m)" \
  bash benchmarks/local_transport_latency.sh
```

The launcher defaults to the physical-core layout on `naamah`. Override
`SERVER_CPU`, `CLIENT_CPU`, the driver CPU variables, or `SAMPLES`/`WARMUP` for
another host. It writes raw CSV, a median/p99 summary, per-process logs, and
machine/build metadata under `benchmark-results/` by default.

The launcher deliberately uses `DEDICATED` Media Driver threads with conductor
`spin`, sender/receiver `noop`, and at most two messages per network send. The
C++ benchmark client's Aeron conductor also spins. The continuously polled C++
client conductor and Media Driver agents each have a distinct physical core in
the default `naamah` layout. The four `AERON_*` environment variables remain
overridable for an explicit polling-policy comparison, and their effective
values are recorded in each run's metadata.

## Qualification

When `aeronmd_s` is discoverable, the main CTest suite launches separate private
server and client Media Drivers plus one Goblin server. It exercises IPC and UDP
with multiple correlated clients, RESP2/RESP3 and SBE, asynchronous Pub/Sub,
Aeron fragmentation, RESP authentication, pipelining, connection churn, and the
INFO capability:

```sh
ctest --test-dir build-aeron -R aeron_roundtrip --output-on-failure
```

The Python wrapper has a corresponding live test:

```sh
PYTHONPATH=python python3 python/tests/test_aeron_roundtrip.py \
  build-aeron/goblin-core "$AERON_PREFIX/bin/aeronmd_s"
```
