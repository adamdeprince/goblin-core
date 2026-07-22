# Goblin Core Documentation

Goblin Core is a compact, single-node server with Redis command semantics and
two independent protocol choices: RESP for compatibility and SBE for typed
binary clients. Both protocols work over TCP (optionally accelerated with
Cisco ExaSock or native NVIDIA XLIO Ultra), Unix-domain sockets, shared-memory
rings, and polled one-sided RDMA rings.

## Start here

- [Build, configure, and run Goblin Core](../README.md)
- [Install Goblin Core from source](../INSTALL.md)
- [Release history](../RELEASE-HISTORY.md)
- [Performance architecture](../PERFORMANCE_BRIEF.md)
- [Benchmark methodology and results](../BENCHMARKS.md)
- [Pub/Sub performance benchmark](../PUBSUB-BENCHMARK.md)
- [XLIO Ultra command latency](../XLIO-LATENCY.md)
- [List storage algorithms](../LISTS.md)
- [Real-time and memory-efficient hash indexes](real-time-hashes.md)
- [Firehose replication and Kafka recovery](replication.md)
- [Kafka write-log ingestion](kafka.md)
- [Authentication and trusted transports](authentication.md)
- [TCP listeners and native TLS](tls.md)
- [Hard memory ceiling and deterministic OOM](maxmemory.md)

## Protocols and transports

| Document | What it covers |
|---|---|
| [TCP listeners and TLS](tls.md) | Repeatable IPv4/IPv6 binds, the mandatory plaintext localhost companion, certificate configuration, and protocol/security boundaries. |
| [Shared-memory ring buffers](ring-buffers.md) | Ring creation, SQ/CQ layout, busy polling, reconnect behavior, sizing, HugeTLB, NUMA placement, and the C++ clients. |
| [Polled RDMA rings](rdma-rings.md) | RC queue-pair setup, sequence-word slots, cached credits, memory registration, mixed-target priority, and RESP/SBE clients. |
| [InfiniBand setup](infiniband-setup.md) | Adapter inventory, PSID-safe firmware updates, OpenSM, link validation, verbs/perftest acceptance checks, IPoIB, and the mixed ring/RDMA polling contract. |
| [ExaSock / Nexus SmartNIC](exasock.md) | Opt-in CMake flag, system ExaSock SDK (not vendored), `exasock` wrapper, RESP/SBE TCP clients, INFO fields. |
| [Native XLIO Ultra TCP](xlio.md) | Native RESP/SBE server and client, strict poll priority, pinned XLIO/DPCP sources, ConnectX-5 qualification, build recipe, snapshot constraints, and kernel-TCP interoperability. |
| [SBE protocol](sbe-protocol.md) | Handshake, framing, schema generation, message and reply types, compatibility rules, and typed-client usage. |

Protocol and transport are independent. RESP and opt-in SBE can each run over
TCP (native TLS, ExaSock, or [XLIO Ultra](xlio.md)), Unix-domain sockets, a
shared-memory ring, or a polled RDMA ring. XLIO preserves ordinary TCP on the
wire and remains compatible with a kernel TCP peer. Ordinary socket TCP is
always plaintext on `127.0.0.1` and requires TLS on non-loopback addresses. SBE
requires `--enable-sbe` and is intentionally unauthenticated; see
[Authentication](authentication.md) before exposing it.

## Replication, durability, and replay

[`GOBLIN.FIREHOSE`](replication.md) provides transport-neutral live replication
over TCP, UDS, shared-memory rings, or RDMA. A replica can load a native
snapshot, replay the durable RESP2 mutation log from [Kafka](kafka.md), buffer
the live firehose while it catches up, and open its listeners only after the
handoff is complete. Kafka owns the durable history; Goblin Core remains the
compact serving layer.

The [Thunder and Redpanda durability deployment record](thunder-redpanda-deployment.md)
preserves the host-specific first production-style lab setup separately from
the general [source installation guide](../INSTALL.md).

## Command reference

Commands that share one implementation family link to the corresponding family
reference. Goblin-specific atomic helpers and scripting commands have individual
pages.

### Server and keyspace

- [`AUTH`](commands/AUTH.md), [`HELLO`](commands/HELLO.md),
  [`CLIENT`](commands/CLIENT.md), [`COMMAND`](commands/COMMAND.md)
- [`SELECT`](commands/SELECT.md), [`QUIT`](commands/QUIT.md),
  [`PING`](../README.md#current-commands), [`ECHO`](../README.md#current-commands)
- [`INFO`](commands/INFO.md)
- [`TIME`](commands/operational.md#time),
  [`ROLE`](commands/operational.md#role),
  [`CONFIG GET`](commands/operational.md#config-get)
- [`MULTI`](commands/transactions.md#multi-and-exec),
  [`EXEC`](commands/transactions.md#multi-and-exec),
  [`DISCARD`](commands/transactions.md#discard),
  [`WATCH`](commands/transactions.md#watch-and-unwatch),
  [`UNWATCH`](commands/transactions.md#watch-and-unwatch)
- [`DEL`](commands/keys.md#del), [`EXISTS`](commands/keys.md#exists),
  [`TYPE`](commands/keys.md#type), [`DBSIZE`](commands/keys.md#dbsize),
  [`RANDOMKEY`](commands/keys.md#randomkey), [`TOUCH`](commands/keys.md#touch)
- [`RENAME`](commands/keys.md#rename--renamenx),
  [`RENAMENX`](commands/keys.md#rename--renamenx),
  [`COPY`](commands/keys.md#copy), [`SCAN`](commands/iteration.md#scan)

The [bounded-iteration reference](commands/iteration.md) documents the shared
cursor, filtering, and mutation contract for `SCAN`, `HSCAN`, `SSCAN`, and
`ZSCAN`.

The [transaction reference](commands/transactions.md) documents atomic queued
execution, optimistic `WATCH`, runtime versus queue-time errors, and the fixed
page-backed per-client queue.

### Strings and counters

- [`SET`](commands/strings.md#set), [`SETNX`](commands/strings.md#setnx),
  [`GET`](commands/strings.md#get), [`GETEX`](commands/strings.md#getex),
  [`GETSET`](commands/strings.md#getset),
  [`GETDEL`](commands/strings.md#getdel)
- [`STRLEN`](commands/strings.md#strlen), [`APPEND`](commands/strings.md#append),
  [`GETRANGE`](commands/strings.md#getrange),
  [`SETRANGE`](commands/strings.md#setrange)
- [`INCR`](commands/strings.md#incr-decr-incrby-decrby),
  [`DECR`](commands/strings.md#incr-decr-incrby-decrby),
  [`INCRBY`](commands/strings.md#incr-decr-incrby-decrby),
  [`DECRBY`](commands/strings.md#incr-decr-incrby-decrby),
  [`INCRBYFLOAT`](commands/strings.md#incrbyfloat)
- [`MSET`](commands/strings.md#mset), [`MSETNX`](commands/strings.md#msetnx),
  [`MGET`](commands/strings.md#mget)

### Expiration

- [`EXPIRE`](commands/ttl.md#expire-pexpire),
  [`PEXPIRE`](commands/ttl.md#expire-pexpire),
  [`EXPIREAT`](commands/ttl.md#expireat-pexpireat),
  [`PEXPIREAT`](commands/ttl.md#expireat-pexpireat)
- [`TTL`](commands/ttl.md#ttl-pttl), [`PTTL`](commands/ttl.md#ttl-pttl),
  [`EXPIRETIME`](commands/ttl.md#expiretime-pexpiretime),
  [`PEXPIRETIME`](commands/ttl.md#expiretime-pexpiretime),
  [`PERSIST`](commands/ttl.md#persist)

### Sorted sets

The [sorted-set command reference](commands/sorted-sets.md) covers conditional
updates, score bounds, rank and score ranges, endpoint pops, scans, and storage.

- [`ZADD`](commands/sorted-sets.md#zadd-and-zincrby),
  [`ZINCRBY`](commands/sorted-sets.md#zadd-and-zincrby),
  [`ZCARD`](commands/sorted-sets.md#command-surface)
- [`ZSCORE`](commands/sorted-sets.md#command-surface),
  [`ZMSCORE`](commands/sorted-sets.md#multi-score-reads-and-endpoint-pops),
  [`ZRANK`](commands/sorted-sets.md#rank-and-score-ranges),
  [`ZREVRANK`](commands/sorted-sets.md#rank-and-score-ranges)
- [`ZRANGE`](commands/sorted-sets.md#rank-and-score-ranges),
  [`ZRANGEBYSCORE`](commands/sorted-sets.md#rank-and-score-ranges),
  [`ZREVRANGEBYSCORE`](commands/sorted-sets.md#rank-and-score-ranges),
  [`ZCOUNT`](commands/sorted-sets.md#rank-and-score-ranges)
- [`ZREM`](commands/sorted-sets.md#command-surface),
  [`ZREMRANGEBYRANK`](commands/sorted-sets.md#rank-removal-and-aggregate-stores),
  [`ZINTERSTORE`](commands/sorted-sets.md#rank-removal-and-aggregate-stores),
  [`ZUNIONSTORE`](commands/sorted-sets.md#rank-removal-and-aggregate-stores),
  [`ZPOPMIN`](commands/sorted-sets.md#multi-score-reads-and-endpoint-pops),
  [`ZPOPMAX`](commands/sorted-sets.md#multi-score-reads-and-endpoint-pops),
  [`ZSCAN`](commands/sorted-sets.md#incremental-scan),
  [`ZREMRANGEBYSCORE`](commands/ZREMRANGEBYSCORE.md)

### Hashes

The [hash command reference](commands/hashes.md) covers behavior and replies.
The [hash implementation guide](real-time-hashes.md) covers the efficient and
incremental RT indexes, qualified command families, and `--real-time` mode.

- [`HSET`](commands/hashes.md), [`HMSET`](commands/hashes.md#hmset),
  [`HSETNX`](commands/hashes.md), [`HGET`](commands/hashes.md),
  [`HMGET`](commands/hashes.md)
- [`HDEL`](commands/hashes.md), [`HEXISTS`](commands/hashes.md),
  [`HLEN`](commands/hashes.md), [`HSTRLEN`](commands/hashes.md)
- [`HGETALL`](commands/hashes.md), [`HKEYS`](commands/hashes.md),
  [`HVALS`](commands/hashes.md), [`HINCRBY`](commands/hashes.md),
  [`HINCRBYFLOAT`](commands/hashes.md#hincrbyfloat),
  [`HRANDFIELD`](commands/hashes.md#hrandfield),
  [`HSCAN`](commands/iteration.md#hscan)

### Bounded iteration

- [`SCAN`](commands/iteration.md#scan),
  [`HSCAN`](commands/iteration.md#hscan),
  [`SSCAN`](commands/iteration.md#sscan),
  [`ZSCAN`](commands/iteration.md#zscan)

### Lists

The [list command reference](commands/lists.md) covers both the default
segmented backend and the adaptive-PMA backend. The
[local work-queue benchmark](../LIST-WORK-QUEUE-BENCHMARK.md) compares the new
commands with the existing list surface over one RESP2/UDS path.

- [`LPUSH`](commands/lists.md), [`RPUSH`](commands/lists.md),
  [`LPUSHX`](commands/lists.md), [`RPUSHX`](commands/lists.md)
- [`LPOP`](commands/lists.md), [`RPOP`](commands/lists.md),
  [`LMOVE`](commands/lists.md), [`RPOPLPUSH`](commands/lists.md),
  [`LLEN`](commands/lists.md), [`LINDEX`](commands/lists.md),
  [`LRANGE`](commands/lists.md)
- [`BLPOP`](commands/lists.md), [`BRPOP`](commands/lists.md),
  [`BLMOVE`](commands/lists.md), [`LMPOP`](commands/lists.md),
  [`BLMPOP`](commands/lists.md)
- [`LSET`](commands/lists.md), [`LTRIM`](commands/lists.md),
  [`LREM`](commands/lists.md), [`LINSERT`](commands/lists.md)

Every list command is also available under `GOBLIN.PMA.*` and
`GOBLIN.SEGMENTED.*`, allowing clients and benchmarks to select a concrete
implementation without changing the standard aliases.

### Pub/Sub

The [Pub/Sub command reference](commands/pubsub.md) covers RESP2 arrays, RESP3
pushes, typed SBE delivery, glob matching, disconnect cleanup, and the
page-backed slow-consumer queue.

- [`SUBSCRIBE`](commands/pubsub.md), [`UNSUBSCRIBE`](commands/pubsub.md),
  [`PSUBSCRIBE`](commands/pubsub.md), [`PUNSUBSCRIBE`](commands/pubsub.md)
- [`PUBLISH`](commands/pubsub.md), [`PUBSUB CHANNELS`](commands/pubsub.md),
  [`PUBSUB NUMSUB`](commands/pubsub.md), [`PUBSUB NUMPAT`](commands/pubsub.md)

## Native atomic helpers

These commands replace multi-command Lua idioms with one atomic C++ operation.

| Command | Native operation |
|---|---|
| [`GOBLIN.CAD`](commands/GOBLIN.CAD.md) | Compare-and-delete a string key. |
| [`GOBLIN.CAEXPIRE`](commands/GOBLIN.CAEXPIRE.md) | Renew a TTL only while the expected token still owns the key. |
| [`GOBLIN.CAS`](commands/GOBLIN.CAS.md) | Compare-and-set while preserving the existing TTL. |
| [`GOBLIN.INCREX`](commands/GOBLIN.INCREX.md) | Fixed-window counter with expiry armed on the first write. |
| [`GOBLIN.ZWINDOW`](commands/GOBLIN.ZWINDOW.md) | Sliding-window limiter, self-healing mutex, or counting semaphore. |
| [`GOBLIN.INCRBOUND`](commands/GOBLIN.INCRBOUND.md) | Increment only while the result stays within a ceiling. |
| [`GOBLIN.DECRPOS`](commands/GOBLIN.DECRPOS.md) | Decrement only while a positive balance remains. |
| [`GOBLIN.HCAD`](commands/GOBLIN.HCAD.md) | Compare-and-delete a hash field. |
| [`GOBLIN.HSETGT`](commands/GOBLIN.HSETGT.md) | Set a hash field only when the new numeric value is greater. |
| [`GOBLIN.CLAIM`](commands/GOBLIN.CLAIM.md) | Claim work once with an expiring lease, or return its prior result. |
| [`GOBLIN.TD_LEADERBOARD_RESCORE`](commands/GOBLIN.TD_LEADERBOARD_RESCORE.md) | Stream a timestamp leaderboard through linear, exponential, or step decay and return its top k. |

Operational extensions are collected in the
[`GOBLIN.*` command reference](commands/goblin.md):
[`GOBLIN.MEMORY`](commands/goblin.md#goblin-memory),
[`GOBLIN.OPTIMIZE`](commands/goblin.md#goblin-optimize),
[`GOBLIN.SAVE`](commands/goblin.md#goblin-save),
[`GOBLIN.DUMPWORLD`](commands/goblin.md#goblin-dumpworld), and
[`GOBLIN.LOAD`](commands/goblin.md#goblin-load).
The same-version server stream is documented separately as
[`GOBLIN.FIREHOSE`](commands/operational.md#goblinfirehose).

## Scripting

Each runtime has an independent VM and compiled-script cache. They share the
same keyspace and atomic command execution model. See the
[scripting overview](commands/README.md) for argument, sandbox, cache, and return
value rules.

| Runtime | Commands |
|---|---|
| PUC-Lua 5.1 | [`EVAL`](commands/EVAL.md), [`EVALSHA`](commands/EVALSHA.md), [`SCRIPT`](commands/SCRIPT.md) |
| Luau | [`LUAU.EVAL`](commands/LUAU.EVAL.md), [`LUAU.EVALSHA`](commands/LUAU.EVALSHA.md), [`LUAU.SCRIPT`](commands/LUAU.SCRIPT.md) |
| Wren | [`WREN.EVAL`](commands/WREN.EVAL.md), [`WREN.EVALSHA`](commands/WREN.EVALSHA.md), [`WREN.SCRIPT`](commands/WREN.SCRIPT.md) |
| Jim Tcl | [`TCL.EVAL`](commands/TCL.EVAL.md), [`TCL.EVALSHA`](commands/TCL.EVALSHA.md), [`TCL.SCRIPT`](commands/TCL.SCRIPT.md) |
| MicroPython | [`UPYTHON.EVAL`](commands/UPYTHON.EVAL.md), [`UPYTHON.EVALSHA`](commands/UPYTHON.EVALSHA.md), [`UPYTHON.SCRIPT`](commands/UPYTHON.SCRIPT.md) |
| QuickJS | [`QUICKJS.EVAL`](commands/QUICKJS.EVAL.md), [`QUICKJS.EVALSHA`](commands/QUICKJS.EVALSHA.md), [`QUICKJS.SCRIPT`](commands/QUICKJS.SCRIPT.md) |
