# Goblin SBE wire protocol

Goblin speaks two wire protocols on the same server, chosen per connection by the
**first 8 bytes** an endpoint writes on a socket or a ring SQ:

| first 8 bytes | protocol |
|---|---|
| `GOBLINS!` | the **SBE binary wire** described here |
| anything else | **RESP** (Redis serialization), unchanged |

The magic is a **one-time connection handshake**: it is written once as the very first
bytes on the socket/ring and consumed, and it does **not** prefix each command. After
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

### Latency over the ring

Measured by [`benchmarks/sbe_vs_resp_ring.cpp`](../benchmarks/sbe_vs_resp_ring.cpp) — both
protocols on one server, client and server pinned to separate cores (host `naamah` — an
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

### Dispatch

`templateId` **is** the command discriminant. The server reads the header and switches
on `templateId` — the ids are dense, so the switch compiles to a jump table. There is
one message type per command; a request is decoded in place (zero-copy) and applied to
the store with no intermediate parse.

## Messages

### Reply messages (server → client), ids 1–15

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

Ids 9–15 are reserved for future reply types. SBE groups cap at 65535 elements
(`numInGroup` is uint16); a larger array reply returns `ErrorReply` rather than
truncating silently (huge ranges must paginate).

### Request messages (client → server), ids 16+

One per command, appended densely (ids 16–84). **The entire command surface is on the
wire** — strings, keyspace/TTL, hash, zset, the eleven `GOBLIN.*` natives, admin
(`OPTIMIZE`, `INFO`, `MEMORY`, `SAVE`, `LOAD`), and scripting — so a foreign client
never has to fall back to RESP. `EVAL`/`EVALSHA`/`SCRIPT` are three messages (`Eval`
79, `EvalSha` 80, `Script` 81) carrying a `language` byte (0 Lua … 5 QuickJS) that
selects the engine; a script's arbitrary, possibly nested RESP result comes back as
the flattened `RespValueReply` (id 10). `GOBLIN.MEMORY` (82) replies a `MapReply` (id
11) of stat pairs. `ZREVRANGE` is `ZRange` with `rev=1`.

The reply set is eleven: the eight core replies plus `NullableArrayReply` (id 9, for
`HMGET`/`MGET` per-element nils), `RespValueReply` (id 10, flattened script results),
and `MapReply` (id 11, key/value pairs).

## Backward-compatible extension

Goblin adds commands continuously, so the schema is **append-only** — this is the rule
that keeps a deployed client working when the server grows:

* **`templateId` is a permanent command id.** A new command takes the next unused id,
  appended. Never renumber, never reuse a retired id. `sbe/goblin_sbe.xml` is the ledger.
* **Fields within a message are append-only.** A new argument is appended (a fixed field
  after the block, or a group / var-data after the existing ones) tagged
  `sinceVersion="V"`. An SBE decoder reads the *wire's* `blockLength` and skips anything
  it does not know, so a new server and an old client interoperate untouched.
* **Bump the schema `version`** on each additive change.

This is the same additive-evolution model as Protocol Buffers.

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

The SBE wire is enabled automatically whenever `sbe/generated/` is present (it always
is — the codecs are checked in). There is no external dependency to find; a build
without the generated headers is simply RESP-only.
