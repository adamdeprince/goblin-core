# Goblin SBE wire protocol

Goblin can speak two wire protocols on the same server. SBE must first be enabled
with `--enable-sbe`; it is then chosen per connection by the
**first 8 bytes** an endpoint writes over TCP, a Unix-domain socket, a
shared-memory ring, or a [polled RDMA ring](rdma-rings.md):

| first 8 bytes | protocol |
|---|---|
| `GOBLINS!` | the **SBE binary wire** described here |
| anything else | **RESP** (RESP2 initially; `HELLO 3` selects RESP3) |

Without `--enable-sbe`, every connection remains RESP and `GOBLINS!` is treated
as an ordinary unknown RESP command.

The magic is a **one-time connection handshake**: it is written once as the very first
bytes on the transport and consumed, and it does **not** prefix each command. After
it, the whole connection is SBE — bare length-prefixed frames (below).

**Why 8 bytes, not 4:** goblin has commands beginning `GOBLIN.` (`GOBLIN.MEMORY`,
`GOBLIN.CAD`, ...), so an *inline* RESP command starts with the bytes `GOBLIN.`. A
4-byte `GOBL` magic would misfire on goblin's own commands; `GOBLINS!` diverges from
`GOBLIN.` at byte 6 (`S` vs `.`), so it cannot collide. The server also decides RESP
the moment the prefix diverges, so a short inline command like `PING\r\n` is never
stalled waiting for a full 8-byte magic that will not come.

The SBE wire is a side-by-side alternative to RESP, not a re-encoding of it. It exists
for latency- and size-sensitive clients (e.g. HFT over the shared-memory ring): scores
and counts ride as **native `double` / `int64`** rather than ASCII, so neither side
parses or re-stringifies numbers, and the server dispatches straight out of the buffer.

The C++ typed client is compile-time-dispatched over its transport:
`SbeSocketClient` uses TCP or a Unix-domain socket, `SbeRingClient` uses the
co-located shared-memory ring, and `SbeRdmaClient` uses the cross-host one-sided
ring. They expose the same command API without a virtual call on the request
path.

**SBE is a lockstep protocol in Goblin Core. An SBE client and server must run
exactly the same Goblin Core version; behavior is undefined when their versions
differ. Use RESP2 or RESP3 when compatibility across independent upgrades is a
priority.**

**SBE has no authentication exchange.** It is a trusted-fabric protocol for
ring, RDMA, and otherwise isolated cluster links. Enabling it also permits SBE
negotiation on configured TCP and UDS listeners, so those listeners must remain
inside the same security boundary. Use RESP with `--auth-file` and a
[native TLS listener](tls.md) at an untrusted network edge.

### Latency over the ring

Measured by [`benchmarks/sbe_vs_resp_ring.cpp`](../benchmarks/sbe_vs_resp_ring.cpp) — both
protocols on one server, client and server pinned to separate cores (a quiet dedicated
AMD Ryzen Threadripper PRO 5995WX, 64C/128T, x86-64; a many-core workstation part, not a
single-thread speed champion — which makes the sub-microsecond round trips notable).
Figures are the **median** round-trip over a 3-second window (~1–5 M samples per op). SBE runs ~1.7–2.0× faster than RESP over the *same* ring; the delta is pure protocol —
no ASCII number parsing on either side, and the server dispatches on a jump table over the
template id.

| operation | SBE | RESP |
|---|---:|---:|
| `PING` | 0.13 µs | 0.26 µs |
| `ZADD`, 1 member | 0.21 µs | 0.43 µs |
| `ZADD`, 10 members | 0.93 µs | 1.60 µs |
| `HSET`, 1 field | 0.23 µs | 0.41 µs |
| `HSET`, 10 fields | 0.71 µs | 1.24 µs |

The single-element ops hold p99 within ~0.1 µs of the median. (The *mean* is noise on a
shared host — occasional preemption of the busy-poll pulls it several times above the
median — so the median is the figure to read.)

## Encoding

Messages use [Simple Binary Encoding](https://github.com/aeron-io/simple-binary-encoding)
(SBE), the FIX/Real-Logic codec. The schema is [`sbe/goblin_sbe.xml`](../sbe/goblin_sbe.xml);
the header-only C++ codecs under `sbe/generated/` are produced from it by SbeTool (a
maintainer step, see below) and checked in. SBE C++ has **no runtime library** — an
ordinary build just compiles the generated headers.

### Framing

An SBE message is not self-delimiting (its groups and variable-length data make the
size dynamic), so every message on the stream is prefixed with its total byte length:

```
[ uint32 little-endian total-message-bytes ][ SBE message: 8-byte header + body ]
```

This is the moral equivalent of the FIX Simple Open Framing Header, kept minimal (the
endpoint is already known to be SBE from the magic). The 8-byte SBE header carries
`blockLength`, `templateId`, `schemaId`, `version`.

On the shared-memory path, `SbeRingClient` places that length prefix and the complete
SBE message in one ring record. The server dispatches directly from the record; the
client rejects a frame larger than the ring's contiguous-message capacity before
publishing it. This is distinct from RESP's stream-oriented ring client, which may
split a large byte stream across records.

### Pipelining

SBE replies are ordered on a connection, so pipelining does not add a request id to
the wire. The typed client exposes `enqueue_sbe<Message>()` for the complete generated
command surface, convenient `enqueue_h*()` writers for hashes, and typed ordered
readers such as `read_pipeline_int()` and `read_pipeline_bulk_or_nil()`.

If enqueue fills the SQ, the client drains complete CQ records into its local reply
accumulator before retrying. The server can therefore continue producing replies even
when the pipeline is deeper than either ring. A synchronous command is rejected while
pipelined replies remain outstanding; asynchronous Pub/Sub pushes are separated and
queued while ordinary replies continue to be read in order.

### Dispatch

`templateId` **is** the command discriminant. The server reads the header and switches
on `templateId` — the ids are dense, so the switch compiles to a jump table. There is
one message type per command; a request is decoded in place (zero-copy) and applied to
the store with no intermediate parse.

## Messages

### Reply messages (server → client), ids 1–15 and 125

A small generic set, reused by every command. New commands almost never add a reply
type — they pick one of these:

| id | message | shape |
|---|---|---|
| 1 | `NilReply` | null bulk (absent key/member) |
| 2 | `StatusReply` | simple string (`OK`, `PONG`) |
| 3 | `ErrorReply` | `code` + `message` |
| 4 | `IntReply` | `int64` |
| 5 | `DoubleReply` | native `double` (e.g. `ZSCORE`) |
| 6 | `BulkReply` | one bulk string *(reserved; lands with `GET`)* |
| 7 | `ArrayReply` | group of bulk strings (e.g. `ZRANGE`) |
| 8 | `ScoredArrayReply` | group of `(native double score, member)` (`ZRANGE WITHSCORES`) |
| 9 | `NullableArrayReply` | group of present/null bulk values (`HMGET`, `MGET`) |
| 10 | `RespValueReply` | flattened arbitrary script result |
| 11 | `MapReply` | string key/value pairs (`GOBLIN.MEMORY`) |
| 12 | `PubSubPush` | asynchronous delivery or subscription acknowledgement |
| 13 | `PubSubNumSubReply` | channel/subscriber-count pairs |
| 14 | `NullableDoubleArrayReply` | ordered present/null native doubles (`ZMSCORE`) |
| 15 | `ScoredScanReply` | next cursor plus `(native double score, member)` entries (`ZSCAN`) |
| 125 | `StringScanReply` | next cursor plus flat strings (`SCAN`, `HSCAN`) |

SBE groups cap at 65535 elements
(`numInGroup` is uint16); a larger array reply returns `ErrorReply` rather than
truncating silently (huge ranges must paginate).

### Request messages (client → server), ids 16–124 and 126+

One per command, appended densely (ids 16–124). **The entire command surface is on the
wire** — strings, keyspace/TTL, hash, zset, the eleven `GOBLIN.*` natives, admin
(`OPTIMIZE`, `INFO`, `MEMORY`, `SAVE`, `LOAD`), lists, Pub/Sub, and scripting — so a
foreign client never has to fall back to RESP. `EVAL`/`EVALSHA`/`SCRIPT` are three messages (`Eval`
79, `EvalSha` 80, `Script` 81) carrying a `language` byte (0 Lua … 5 QuickJS) that
selects the engine; a script's arbitrary, possibly nested RESP result comes back as
the flattened `RespValueReply` (id 10). `GOBLIN.MEMORY` (82) replies a `MapReply` (id
11) of stat pairs. List commands occupy ids 85–95, Pub/Sub occupies ids 96–101,
sets occupy ids 102–118, the expanded sorted-set requests occupy ids 119–124,
and bounded key/hash iteration occupies ids 126–127. `ZREVRANGE` is `ZRange`
with `rev=1`.

The reply set is sixteen: the eight core replies plus `NullableArrayReply` (id 9,
for `HMGET`/`MGET` per-element nils), `RespValueReply` (id 10, flattened script
results), `MapReply` (id 11, key/value pairs), the two Pub/Sub replies, nullable
native scores (id 14), scored scan pages (id 15), and flat string scan pages
(id 125).

### Iteration messages

`Scan` (126) carries an opaque cursor, work count, and optional glob and key-type
filters. `HScan` (127) carries an opaque cursor, work count, key, optional field
glob, and the `NOVALUES` selection. Both return `StringScanReply` (125), whose
group is keys for `SCAN`, alternating fields and values for ordinary `HSCAN`,
or fields for `HSCAN NOVALUES`.

The pre-existing `SScan` (113) returns a flat cursor/member `ArrayReply`;
`ZScan` (124) returns `ScoredScanReply`. The newer scan replies preserve native
cursors and sorted-set scores; the older `SScan` shape carries its cursor as the
first array string.

### Sorted-set messages

`ZAdd` (17) uses its `flags` byte for the complete conditional surface: `0x01`
NX, `0x02` XX, `0x04` GT, `0x08` LT, `0x10` CH, and `0x20` INCR. INCR requires
one member and replies with `DoubleReply` or `NilReply`; other forms reply with
`IntReply`.

`ZIncrBy` (119) carries one native-double increment. `ZRangeByScore` (120)
carries native-double bounds plus exclusivity, reverse, score-return, offset,
and limit fields. `ZCount` (121), `ZMScore` (122), `ZPop` (123), and `ZScan`
(124) complete the same operation set as RESP without turning scores or cursors
into text. `ZScan` replies with `ScoredScanReply`; its cursor remains opaque to
clients.

### Pub/Sub messages

`Subscribe` (96), `Unsubscribe` (97), `PSubscribe` (98), and `PUnsubscribe` (99)
carry groups of channel names or patterns. `Publish` (100) carries a channel and
payload. `PubSub` (101) carries an operation byte (`0` channels, `1` numsub, `2`
numpat) plus its arguments.

`PubSubPush` is asynchronous. Its `kind` is `0` for a literal message, `1` for a
pattern message, `2`/`3` for literal/pattern subscribe acknowledgement, and
`4`/`5` for literal/pattern unsubscribe acknowledgement. The typed client queues
pushes that arrive while it waits for a synchronous reply, including the
self-publish ordering where deliveries precede the `Publish` `IntReply`.

The server keeps a page-backed unsolicited-output FIFO per SBE connection, just
as it does for RESP. See the [Pub/Sub command reference](commands/pubsub.md) for
the byte limit and slow-consumer disconnect policy.

## Version contract

The schema remains append-only as an engineering discipline: template ids are
never renumbered or reused, fields are appended rather than reordered, and the
schema version advances with changes. This keeps generated-code changes
reviewable and prevents accidental reinterpretation within a release.

It is not a cross-version compatibility promise. Goblin Core does not test or
support an SBE client from one release against a server from another release.
They must have exactly matching Goblin Core versions. Use RESP when deployments
need rolling upgrades or long-lived clients that upgrade independently.

## Adding a command (the recipe)

1. **Schema** — append a `<sbe:message>` to `sbe/goblin_sbe.xml` with the next free
   `templateId`; reuse an existing reply message.
2. **Regenerate** the codecs (maintainer step):
   ```
   cmake --build <dir> --target goblin_core_regen_sbe
   # or: java -Dsbe.target.language=Cpp -Dsbe.output.dir=sbe/generated \
   #          -jar sbe-all.jar sbe/goblin_sbe.xml
   ```
   Commit the regenerated `sbe/generated/`.
3. **Handler** — add a `case` in `src/sbe_dispatch.cpp`: `wrapForDecode` → read native
   fields → `Store` call → `reply<...>`.
4. **Test** — add a line to `tests/sbe_dispatch_test.cpp`.

## Building

The SBE wire is compiled whenever `sbe/generated/` is present (it normally is --
the codecs are checked in). There is no external dependency to find; a build
without the generated headers is RESP-only. A compiled server still requires the
runtime `--enable-sbe` flag before it accepts the magic handshake.
