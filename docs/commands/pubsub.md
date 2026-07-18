# Pub/Sub commands

Goblin Core provides Redis-compatible literal and pattern subscriptions over
RESP2, RESP3, and SBE. Protocol and transport are independent: Pub/Sub works over
TCP, Unix-domain sockets, shared-memory rings, and polled RDMA rings.

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

The header-only typed SBE clients expose `subscribe`, `unsubscribe`,
`psubscribe`, `punsubscribe`, `publish`, `pubsub_channels`, `pubsub_numsub`,
`pubsub_numpat`, `try_read_pubsub`, and `read_pubsub`. They queue asynchronous
pushes encountered while waiting for a synchronous command reply.

## Upstream rebroadcast

A Goblin Core server can subscribe to one other Goblin Core server over SBE and
rebroadcast every received channel/payload pair to its own local subscribers.
For a shared-memory ring:

```sh
# The upstream server owns this ring.
goblin-core --ring /run/goblin/upstream.ring 2mb

# The downstream server connects as an SBE PSUBSCRIBE client.
goblin-core --pubsub-listener-ring /run/goblin/upstream.ring
```

The equivalent cross-host listener uses a polled RDMA ring:

```sh
goblin-core --pubsub-listener-rdma 10.88.88.1 6380 2mb
```

SBE can also use an ordinary Unix-domain socket or TCP connection:

```sh
goblin-core --pubsub-listener-uds /run/goblin/upstream.sock
goblin-core --pubsub-listener-tcp goblin-upstream.internal 6379
```

All forms subscribe to `*` by default. Select a narrower Redis glob with:

```sh
goblin-core --pubsub-listener-ring /run/goblin/upstream.ring \
  --pubsub-listener-pattern 'orders:*'
```

The listener transport flags are mutually exclusive and startup fails if the
upstream connection or SBE subscription cannot be established. Ring and RDMA
listeners use the server's busy-poll loop; TCP and UDS listeners use nonblocking
sockets in that same loop. Rebroadcast is one-way: local publications are not
sent upstream. The relay carries the original binary-safe channel and payload
into the local registry, where ordinary literal/pattern matching, delivery
counts, and slow-consumer limits apply.

The upstream and downstream servers must be exactly the same Goblin Core version
because this link uses SBE. A different-version pairing has undefined behavior.
Use RESP when independent client/server upgrades are required. Do not configure
relay cycles: forwarded publications carry no origin marker, so a cycle can
rebroadcast the same message indefinitely.

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
