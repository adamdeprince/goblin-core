# Pub/Sub commands

Goblin Core provides Redis-compatible literal and pattern subscriptions over
RESP2, RESP3, and SBE. Protocol and transport are independent: Pub/Sub works over
TCP, Unix-domain sockets, and shared-memory rings.

## Command summary

| Command | Result |
|---|---|
| `SUBSCRIBE channel [channel ...]` | Subscribe the connection to exact, binary-safe channel names. |
| `UNSUBSCRIBE [channel ...]` | Remove named literal subscriptions, or all literal subscriptions when no names are supplied. |
| `PSUBSCRIBE pattern [pattern ...]` | Subscribe using Redis glob patterns. |
| `PUNSUBSCRIBE [pattern ...]` | Remove named pattern subscriptions, or all pattern subscriptions when no names are supplied. |
| `PUBLISH channel message` | Queue a message for every matching literal and pattern subscription; return the delivery count. |
| `PUBSUB CHANNELS [pattern]` | Return active literal channels, optionally filtered by a glob. |
| `PUBSUB NUMSUB [channel ...]` | Return each requested channel and its literal subscriber count. |
| `PUBSUB NUMPAT` | Return the number of unique active patterns. |

Subscribing to the same name twice acknowledges both arguments but does not add a
second subscription. A connection subscribed to both a literal channel and a
matching pattern receives both `message` and `pmessage`; `PUBLISH` counts both
deliveries, matching Redis behavior.

Patterns operate on bytes and support `*`, `?`, character sets such as `[abc]`,
ranges such as `[a-z]`, negated sets such as `[^0-9]`, and backslash escapes.
Literal publication uses a direct hash-table lookup. Pattern publication scans
the active unique patterns and then traverses the subscriber set for each match.

## RESP delivery

RESP2 uses ordinary arrays for acknowledgements and messages. A subscription
acknowledgement has the shape `["subscribe", channel, subscription-count]`, a
literal delivery is `["message", channel, payload]`, and a pattern delivery is
`["pmessage", pattern, channel, payload]`.

While a RESP2 connection has subscriptions, it may run `SUBSCRIBE`,
`PSUBSCRIBE`, `UNSUBSCRIBE`, `PUNSUBSCRIBE`, and `PING`. Other commands return an
error until the subscription count reaches zero.

RESP3 represents the same acknowledgement or delivery as a push frame (`>`)
rather than an array (`*`). RESP3 clients may continue issuing ordinary commands
while subscribed. Regular replies and pushes retain their production order, so a
self-publication is delivered before the reply from the `PUBLISH` command that
caused it.

## SBE delivery

SBE requests use templates 96 through 101. Subscription acknowledgements and
messages use the asynchronous `PubSubPush` reply (template 12), whose `kind`
field distinguishes `message`, `pmessage`, subscribe, and unsubscribe events.
`PUBSUB NUMSUB` uses `PubSubNumSubReply` (template 13); the other introspection
forms reuse `ArrayReply` and `IntReply`.

The header-only `SbeRingClient` exposes `subscribe`, `unsubscribe`, `psubscribe`,
`punsubscribe`, `publish`, `pubsub_channels`, `pubsub_numsub`, `pubsub_numpat`,
`try_read_pubsub`, and `read_pubsub`. It queues asynchronous pushes encountered
while waiting for a synchronous command reply.

## Slow consumers

Each client owns one anonymous `mmap`-backed FIFO for serialized unsolicited
output. Every page is write-prefaulted and explicitly locked with `mlock` before
the client enters service, so the first publication does not pay a minor fault
and the queue cannot be reclaimed or swapped after a successful lock. The
default capacity is one native page. Configure it with:

```sh
goblin-core --unsolicited-output-buffer-bytes 65536
```

The requested positive byte count is always rounded up to a whole number of
native pages. This is a byte limit, not a message-count limit. Queue records carry
the already-serialized RESP or SBE frame, so draining does not allocate or encode
the message again.

If one complete delivery cannot fit, Goblin removes that client from every
literal and pattern subscriber set and disconnects it rather than silently
dropping a message. A socket is closed immediately. A ring endpoint stops
accepting work until its client reconnects with a new ring epoch. Increase the
buffer for workloads whose legitimate bursts or individual messages exceed one
page.

## Disconnects and scripts

Disconnect cleanup scans both subscription tables, removes the stable client
pointer from every subscriber set, and deletes channel or pattern entries whose
sets become empty. The server is single-threaded, so subscription changes,
publication, and cleanup are atomic with command execution.

`PUBLISH` is permitted through `redis.call` in all six embedded scripting
engines. Subscription and introspection commands are rejected inside scripts
because they mutate or inspect live connection state.
