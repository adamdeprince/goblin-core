# Goblin Core release history

Goblin Core releases are source releases: each version is a Git tag and its
corresponding source archive. The project is still pre-1.0, so the supported
Redis surface can expand substantially between minor versions.

For the current command surface, build instructions, and compatibility limits,
see the [project documentation](README.md). For changes after the latest tag,
see the [repository history](https://github.com/adamdeprince/goblin-core/commits/main/).

## Unreleased

Nothing yet.

## v0.9.0 — July 20, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.9.0)

The durability, replication, and compatibility release. Native Kafka replay
shipped one week ahead of its July 27 target.

- Added native external journaling and recovery through Kafka-compatible
  brokers using vendored Apache-2.0-compatible librdkafka. Only primaries
  produce; snapshots retain the replication lineage, logical offset, and exact
  acknowledged broker offset for inclusive, deduplicated recovery.
- Added transport-neutral `GOBLIN.FIREHOSE` live replication over TCP, UDS,
  shared-memory rings, and RDMA. Read-only replicas retain state across an
  upstream outage, reconnect automatically, bridge gaps through Kafka, expose
  explicit readiness and lag, and may feed downstream replicas.
- Validated snapshot-plus-Redpanda recovery on every persistent object type,
  then killed a primary and replica separately and exhaustively checked the
  surviving and rebuilt copies after each hard process death.
- Added libsodium-backed username/password authentication, an offline
  `goblin-core-auth` editor, `AUTH`, extended `HELLO`, client metadata commands,
  and explicit trusted-fabric authentication policy for SBE, rings, and RDMA.
- Added `MULTI`, `EXEC`, `DISCARD`, `WATCH`, and `UNWATCH` with page-rounded,
  preallocated per-client transaction buffers and atomic batched execution.
- Added list work-queue commands including `LMOVE`, `RPOPLPUSH`, `BLPOP`,
  `BRPOP`, `BLMOVE`, `LMPOP`, and `BLMPOP`, parking blocked clients without
  blocking server command execution.
- Added repeatable `--listen` IPv4/IPv6 endpoints and native nonblocking OpenSSL
  TLS for non-loopback ordinary TCP. Every configured TCP port retains a
  plaintext `127.0.0.1` companion; certificate/key configuration is shared by
  all external listeners, and replicas support authenticated,
  certificate-verified TLS connections.
- Added bounded `SCAN` and `HSCAN`, completing the cursor family alongside
  `SSCAN` and `ZSCAN`. Keyspace scans support `MATCH`, `COUNT`, and `TYPE`; hash
  scans support `MATCH`, `COUNT`, and `NOVALUES`, including qualified efficient
  and real-time hash command families.
- Added typed SBE scan requests and replies plus stable-traversal differential
  coverage against Redis for all four cursor commands.
- Completed the Redis leaderboard command surface with conditional and
  incrementing `ZADD`, `ZINCRBY`, indexed score ranges, `ZCOUNT`, `ZMSCORE`,
  `ZPOPMIN`/`ZPOPMAX`, `ZSCAN`, rank-range deletion, and union/intersection
  stores over RESP2, RESP3, and typed SBE.
- Added Redis differential coverage for option interactions, inclusive and
  exclusive bounds, infinities, reverse limits, tied scores, pops, scan
  traversal, wrong types, compact-to-full promotion, and every rank-cache mode.
- Added hash, string, keyspace, and operational compatibility commands,
  including hash float increments and scans, multi-key conditional writes,
  rename/copy/random access, `TIME`, `ROLE`, and an honest bounded `CONFIG GET`.
- Added a deterministic `--maxmemory` ceiling with pre-growth OOM rejection and
  no eviction policy.
- Added multi-listener Pub/Sub relays and cross-host delivery over ring, TCP,
  UDS, and RDMA transports.
- Statically linked the GNU C++ and GCC support runtimes by default so source
  builds remain deployable across hosts with older runtime installations.

## v0.8.0 — July 16, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.8.0)

The collections, real-time, and fabric release.

- Added Redis-compatible sets, including membership, cardinality, scan, move,
  random selection, and union/intersection/difference command families, with
  typed SBE support and compact arena-backed storage.
- Added index-addressable arrays with memory-oriented Classic and fixed-capacity
  real-time implementations. Qualified `GOBLIN.CLASSIC.AR*` and
  `GOBLIN.RT.AR*` commands can coexist, while `GOBLIN.RT.ARRESERVE` prefaults a
  declared serving budget and fails closed on exhaustion.
- Added real-time hashes and an optional real-time top-level keyspace using
  incremental linear hashing over 16-slot Swiss buckets. Growth and contraction
  advance by bounded physical-bucket steps instead of rebuilding a whole table.
- Added receiver-polled, one-sided InfiniBand RDMA rings carrying either RESP or
  SBE between hosts. The transport includes cached credits, explicit NUMA
  placement, C++ and Python clients, validation tools, and latency benchmarks.
- Added optional Cisco ExaSock acceleration for Nexus SmartNIC / ExaNIC
  hardware without vendoring its SDK. Ring, ExaSock, RDMA, and socket targets
  retain command-line order as their busy-poll priority.
- Added NUMA topology discovery by node, network interface, or InfiniBand device;
  conflicting CPU and transport locality now requires an explicit selection.
- Added typed SBE request pipelining with in-order reply readers and bounded
  streaming across rings smaller than the pipeline.
- Added XXH3-based field and key hashing plus fast_float integer parsing, with
  their licenses and notices included in the source release.
- Added native C++ collection benchmarks and published current SET, ARRAY, HSET,
  LIST, and sorted-set speed, tail-latency, and RSS artifacts from one benchmark
  methodology. The website now links the reports and the RESP-over-RDMA story.

## v0.7.0 — July 15, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.7.0)

The Pub/Sub and RESP3 release, delivered five days ahead of the July 20 target.

- Added Redis-compatible `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`,
  `PUNSUBSCRIBE`, `PUBLISH`, `PUBSUB CHANNELS`, `PUBSUB NUMSUB`, and
  `PUBSUB NUMPAT` commands.
- Added direct binary-safe literal-channel lookup and Redis-compatible glob
  routing for pattern subscriptions, with atomic disconnect cleanup.
- Added per-connection RESP2, RESP3, and SBE modes. `HELLO 2|3` negotiates the
  RESP wire while preserving the existing `GOBLINS!` SBE handshake.
- Added native RESP3 maps, doubles, nulls, scored pairs, and Pub/Sub push frames,
  including RESP3's ability to run ordinary commands while subscribed.
- Added typed SBE Pub/Sub request, acknowledgement, introspection, and delivery
  templates plus header-only client support over sockets and shared-memory rings.
- Added a bounded anonymous-`mmap` unsolicited-output FIFO per client. Queues are
  page-rounded, prefaulted, explicitly locked, and disconnect slow consumers
  instead of growing the heap or silently dropping messages.
- Added `--unsolicited-output-buffer-bytes` and process-wide
  `mlockall(MCL_CURRENT | MCL_FUTURE)`, with explicit locks retained for arenas,
  HugeTLB mappings, rings, and Pub/Sub FIFOs.
- Allowed `PUBLISH` through all six embedded scripting engines while rejecting
  subscription and connection-state commands from scripts.
- Added native C++ socket and SBE/ring Pub/Sub tests and a cross-engine benchmark
  covering end-to-end delivery, fanout, literal routing, pattern scans, and RSS.
  The checked-in [benchmark report](PUBSUB-BENCHMARK.md) uses 4 KiB rings and
  includes Redis 7.2.4, Redis 8.8, Valkey 9.1, Dragonfly, and mini-redis-go.

## v0.6.0 — July 13, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.6.0)

The list and memory-density release, delivered one week ahead of the July 20
target.

- Added Redis-compatible list commands for push, pop, index, range, set, trim,
  remove, and pivot-relative insert operations.
- Added two large-list engines behind a shared compact small-list format:
  adaptive PMA for fast deep indexing and middle mutation, and segmented
  listpacks for memory density and endpoint workloads.
- Made segmented listpacks the standard-command default. Added
  `--list-implementation pma|segmented` plus the `GOBLIN.PMA.*` and
  `GOBLIN.SEGMENTED.*` command families so both representations can coexist.
- Added true batched PMA insertion, endpoint-biased slack, bitmap rank/select,
  split 32-bit block/offset arena addresses, and automatic large-to-small
  demotion.
- Added independent `--list-max-density` and `--list-resize-growth` controls,
  defaulting to `0.97` and `2**0.25` respectively.
- Added bulk list snapshot restoration and Redis RDB import for plain, ziplist,
  quicklist, and quicklist2/listpack encodings.
- Added exact compact string encoding for strings, hash fields and values, and
  lists, with optional LZ4 compression and a product-wide disable switch.
- Added bounded hash compaction, keyspace-backed compact hashes, more complete
  memory accounting, and the native C++ HSET benchmark harness.
- Added NUMA-local arena placement and optional HugeTLB-backed rings and arena
  blocks, including compaction behavior that releases empty huge-page tails.
- Added shared-memory ring streaming, page preallocation, and mirror mapping.
- Added list memory reporting, compaction, Redis differential coverage, and the
  [list design document](LISTS.md) with a repeatable
  [cross-engine benchmark](LIST-BENCHMARK.md).

## v0.5.0 — July 11, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.5.0)

The low-latency and programmable-infrastructure release.

- Added Redis strings, a unified keyspace, compact TTL storage, and conditional
  `SET`/`EXPIRE` behavior.
- Added six independent scripting runtimes: PUC-Lua 5.1, Luau, Wren, Jim Tcl,
  MicroPython, and QuickJS, each with precompiled script caching.
- Added native atomic commands for locks, compare-and-set, rate limiting,
  quotas, reservations, idempotency, hash watermarks, and time-decay
  leaderboards.
- Added the shared-memory SQ/CQ ring transport and the SBE binary protocol.
  RESP and SBE both work over sockets and rings.
- Added a full-surface header-only SBE ring client and the `goblin_core`
  redis-py-compatible Python client.
- Added epoch-based ring recovery after an unclean client exit.
- Added compact listpack storage for small hashes.
- Published the Lichess leaderboard replay and ring round-trip latency results.

## v0.4.2 — July 9, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.4.2)

The packed-layout and benchmark-refresh release.

- Reworked sorted-set insert, remove, rescore, rank, and range hot paths.
- Replaced linear command matching with generated perfect-hash dispatch.
- Added growable page-aligned arenas and automatic `i16`/`i32`/`f64` score-width
  selection.
- Added the compact listpack representation for small sorted sets and pooled
  their allocations.
- Packed keyspace keys into an arena and reduced per-zset object overhead.
- Added Unix-domain socket support and broader write-path and connection-sweep
  benchmarks.
- Added Dragonfly and Loongson 3A6000 comparisons to the benchmark campaign.
- Added the real Lichess payload harness with final sorted-set verification.
- Fixed same-score insertion across score-index block boundaries.

## v0.4.0 — July 6, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.4.0)

The hash release.

- Added the Redis hash type and its initial command surface.
- Reused the packed arena and Swiss-table design for hash fields and values.
- Added configurable zset and hash arena chunk sizes.
- Added automatic arena compaction for update- and delete-heavy structures.
- Refreshed the documentation against one-host parity benchmarks and tightened
  performance claims to the measurements.

## v0.3.1 — July 5, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.3.1)

The operational persistence release.

- Moved `GOBLIN.SAVE` to a background copy-on-write child so the server keeps
  serving while a snapshot is written.
- Documented the explicit-snapshot, no-AOF persistence model.
- Added measured write-path tail-latency results.
- Fixed member-arena and score-index boundary overflows.
- Re-ran the four-engine allocator/configuration-parity benchmark campaign.

## v0.3.0 — July 5, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.3.0)

The first persistence and migration release.

- Added native `GOBLIN.SAVE`/`GOBLIN.LOAD` snapshots and `--load` at startup.
- Added section-framed snapshots with CRC32C validation.
- Added version-gated packed-index accelerators for fast same-build reloads.
- Added canonical-only `GOBLIN.SAVE ... NOACCEL` snapshots for upgrades and
  cross-machine migration.
- Added Redis RDB auto-detection and sorted-set import.
- Added full save, clear, and reload tests plus persistence benchmarks.

## v0.2.0 — July 3, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.2.0)

The first major memory-layout refinement.

- Reduced score-index block slack and packed member references into
  struct-of-arrays storage.
- Added SIMD Swiss-table group probes and range-output prefetching.
- Added tunable non-power-of-two member-index growth.
- Added `GOBLIN.OPTIMIZE` with configurable target density.
- Established the high-density default growth and optimization settings used by
  the subsequent memory benchmark campaign.
- Added reproducible Redis comparison tooling.

## v0.1.0 — July 2, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.1.0)

The first source release.

- Introduced the C++23 RESP server and compact, vector-backed sorted sets.
- Added the initial sorted-set command surface and differential testing against
  Redis.
- Added exact and block-hint rank-cache modes.
- Added the source-only CMake build, install rules, smoke benchmarks, and initial
  performance documentation.

---

Version numbers before 1.0 describe a growing supported subset, not full Redis
compatibility. Read the documentation for the exact surface and operational
tradeoffs of the version you deploy.
