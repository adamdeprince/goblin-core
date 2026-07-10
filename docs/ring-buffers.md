# Ring buffers — the shared-memory fast path

Goblin Core can take requests over **shared-memory ring buffers** instead of
sockets. A ring is the lowest-latency way to reach the server: there is no kernel,
no syscall, and no network stack on the request path — a co-located client writes
RESP bytes into a memory-mapped file and the server, busy-polling that memory,
picks them up and writes the reply back into the same file.

The rings use the **io_uring SQ/CQ layout**: each ring file carries a *submission
queue* (SQ, client → server, the request direction) and a *completion queue* (CQ,
server → client, the reply direction), coordinated by head/tail indices in a shared
header — exactly io_uring's discipline, adapted to carry variable-length RESP
messages instead of fixed 64-byte SQEs.

## Enabling rings

```
goblin-core --port 6379 --ring /tmp/a 4kb --ring /tmp/b 1mb --ring /tmp/c 64kb
```

Each `--ring <path> <size>` creates one single-writer/single-reader ring, backed by
the file at `<path>` (put it on `tmpfs` — `/tmp` or `/dev/shm` — so it never touches
a disk). `--ring` is repeatable; **the order is priority order** — the first ring is
the highest priority (see below).

`<size>` is the capacity of **each direction** (the SQ and the CQ each get it). It
accepts a plain byte count or a binary suffix, case-insensitive:

| Written | Bytes |
|---|---|
| `4096` | 4096 |
| `4kb` / `4k` | 4096 |
| `1mb` / `1m` | 1048576 |
| `2gb` / `2g` | 2147483648 |
| `512b` | 512 |

The size is rounded **up to a power of two that is at least the system page size**
(mmap requires page-aligned regions; the mask-based ring index requires a power of
two). So `4kb` is exactly 4 KiB on a 4 KiB-page host (x86, most ARM Linux) and
rounds up to 16 KiB on a 16 KiB-page host (Apple Silicon). `GOBLIN.*`-style tiny
rings are fine; a ring only needs to hold a request or two in flight.

## The I/O model changes gears

**Without `--ring`, the server is event-driven and idle-cheap**: it blocks in
`poll()` and wakes only when a socket is ready, so an idle server uses ~0% CPU.

**With `--ring`, the server busy-polls and pegs a core at 100% — by design.** Each
iteration it checks the rings in priority order:

1. If ring 1 has a message, process it and start over from ring 1.
2. Otherwise check ring 2, then ring 3, and so on for as many rings as you passed.
3. **Only when every ring is empty** does it do a pass of network I/O (accepting and
   serving ordinary socket clients), then issue a CPU relax hint and loop.

Because a non-empty high-priority ring always restarts the scan, **ring 1 can starve
ring 2, and ring 2 can starve ring 3.** That is intentional: put your
latency-critical client on the first ring and it will never wait behind the others.

The relax hint between idle spins is `PAUSE` on x86 (`_mm_pause()`), `YIELD` on ARM,
and a barrier on LoongArch. It lets a sibling hyperthread run and, more importantly,
lets the remote core that is writing your request actually land its stores in the
cache line you are spinning on.

Rings and sockets coexist: a ringed server still listens on its port / unix socket
and serves those clients whenever the rings are quiet, so ordinary `redis-cli` still
works — just at lower priority than the rings.

## Wire protocol

The payload is **standard RESP** — the same bytes a socket client sends. A client
writes a command as a RESP array of bulk strings into the SQ; the server writes the
RESP reply into the CQ. The server reassembles the SQ byte stream with its normal
RESP parser, so a single ring record may hold one command, several pipelined
commands, or part of a large one.

Framing: each record starts on a **64-byte cache-line boundary**, with a one-line
control header (the payload length) followed by the payload on the *next* cache line
— so a "line of redis messages" starts cache-aligned and may be longer than a cache
line. Records larger than half the ring are split across several; the client and
server reassemble the stream, so message size is not capped by the ring size. When a
record would run off the end of the ring a short WRAP filler skips to the start, so
every payload is a single contiguous span.

## Talking to a ring: `redis-cli-ring`

`redis-cli-ring` is a proof-of-concept client — the reference for how to drive a
ring from your own code.

```
# one-shot
redis-cli-ring /tmp/a PING
redis-cli-ring /tmp/a SET user:42 alice
redis-cli-ring /tmp/a GET user:42

# interactive (one command per line)
redis-cli-ring /tmp/a
> INCR counter
(integer) 1
```

## Talking to a ring: the header-only C++ client

The whole client is one header, `goblin/core/ring_client.hpp` — include it and you
have a ring client (this is also how the tests drive the server):

```cpp
#include "goblin/core/ring_client.hpp"
using goblin::core::ring::RingClient;

auto client = RingClient::open("/tmp/a");   // waits briefly for the server
if (!client) { /* server not up with --ring /tmp/a ... */ }

auto reply = client->command({"SET", "foo", "bar"});   // std::optional<std::string>
// reply == "+OK\r\n"
auto v = client->command({"GET", "foo"});              // "$3\r\nbar\r\n"

// Pipelining: submit many, then read the replies in order.
for (int i = 0; i < 100; ++i) client->send(std::vector<std::string_view>{"PING"});
for (int i = 0; i < 100; ++i) auto r = client->read_reply();
```

`command()` returns the raw RESP reply bytes; `send()` / `read_reply()` split the
request and response phases for pipelining. The ring core underneath
(`goblin/core/ring_buffer.hpp`) is also header-only if you want to build a client in
another language or embed the producer/consumer directly.

## Constraints and notes

- **One writer, one reader per ring.** A ring is SPSC by construction; point at most
  one client at a given ring file at a time. Run several `--ring`s for several
  clients.
- **The server creates the ring files; start it first.** The client opens an
  existing, initialized file (and retries briefly to absorb a startup race).
- **Rings are a co-located transport.** The client and server must share memory
  (same host); across machines, use the socket interface.
- **Portability.** Linux and macOS are supported. The ring index discipline and the
  relax hint cover x86, ARM, and LoongArch; the page-size rounding adapts to the
  host automatically.

## See also

- [`goblin/core/ring_buffer.hpp`](../include/goblin/core/ring_buffer.hpp) — the
  header-only ring core (layout, producer/consumer, size parsing).
- [`goblin/core/ring_client.hpp`](../include/goblin/core/ring_client.hpp) — the
  header-only client.
- [`src/ring_cli.cpp`](../src/ring_cli.cpp) — `redis-cli-ring`, a worked client.
