# HELLO

Select the RESP protocol version for this connection and return server metadata.

```text
HELLO [2|3]
```

Every socket and RESP-over-ring connection starts in RESP2. `HELLO 3` switches that
connection to RESP3; `HELLO 2` switches it back. A bare `HELLO` reports metadata
using the connection's current version without changing it.

`HELLO` may be sent after other commands. The HELLO reply itself uses the selected
version, and the selection applies immediately to commands already queued behind it
in the same pipeline. Other connections are unaffected.

An unsupported version returns:

```text
-NOPROTO unsupported protocol version
```

and leaves the current version unchanged. Goblin Core does not implement Redis
authentication or client names, so the optional Redis `AUTH` and `SETNAME` HELLO
arguments are not accepted.

## Reply

The reply contains `server`, `version`, `proto`, `id`, `mode`, `role`, and
`modules`. RESP3 encodes these fields as a map; RESP2 encodes the same alternating
key/value fields as an array.

Goblin emits native RESP3 shapes for replies whose RESP2 representation loses type
information:

| Command or value | RESP2 | RESP3 |
|---|---|---|
| Missing value/member | null bulk string | null |
| `HGETALL` | alternating array | map |
| `GOBLIN.MEMORY` | alternating array | map |
| `ZSCORE`, `INCRBYFLOAT` | bulk string | double |
| `ZRANGE ... WITHSCORES` | flat member/score array | array of member/double pairs |

Simple strings, errors, integers, bulk strings, and ordinary arrays are valid in
both versions and retain their existing wire forms.

## SBE

RESP negotiation does not affect SBE. An endpoint whose initial bytes are
`GOBLINS!` still selects the SBE wire for that connection; SBE framing and message
types are unchanged.
