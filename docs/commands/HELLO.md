# HELLO

Select the RESP protocol version for this connection and return server metadata.

```text
HELLO [2|3 [AUTH username password] [SETNAME client-name]]
```

Every socket and RESP-over-ring connection starts in RESP2. `HELLO 3` switches that
connection to RESP3; `HELLO 2` switches it back. A bare `HELLO` reports metadata
using the connection's current version without changing it.

When an auth file is configured, `AUTH` verifies the connection as part of the
same exchange. `SETNAME` assigns the client name returned by `CLIENT GETNAME`.
The options may appear in either order, but the protocol version is required
when either option is present. Goblin validates the complete request before
changing protocol, identity, or name.

`HELLO` may be sent after other commands. The HELLO reply itself uses the selected
version, and the selection applies immediately to commands already queued behind it
in the same pipeline. Other connections are unaffected.

An unsupported version returns:

```text
-NOPROTO unsupported protocol version
```

and leaves the current version unchanged.

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

RESP negotiation does not affect SBE. When the server is started with
`--enable-sbe`, an endpoint whose initial bytes are `GOBLINS!` selects the SBE
wire for that connection. SBE is a trusted-fabric protocol and does not use
RESP authentication.
