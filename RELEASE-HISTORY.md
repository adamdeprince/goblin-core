# Goblin Core release history

Goblin Core releases are source releases: each version is a Git tag and its
corresponding source archive. The project is still pre-1.0, so the supported
Redis surface can expand substantially between minor versions.

For the current command surface, build instructions, and compatibility limits,
see the [project documentation](README.md). For changes after the latest tag,
see the [repository history](https://github.com/adamdeprince/goblin-core/commits/main/).

## Unreleased

Nothing yet.

## v0.10.6 — September 9, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.6)

The space-efficient packed sorted-set release.

- Added six fixed-width packed sorted-set representations for int32, int64, and
  UUID members with binary32 or binary64 scores. Members stay binary in a Swiss
  table and an arena-indexed B+ tree with bounded per-leaf dirty tails. See
  [fixed-width packed sorted sets](docs/packed-zsets.md).
- Published the verified
  [1.48-billion-increment Wikimedia replay](benchmarks/wikimedia_history_2026_08/full-comparison-20260909.md).
  At 80,798,328 members, packed INT32/FLOAT32 used 2.10 GiB of final process RSS:
  53.6–72.2% less than Redis, Valkey, and Dragonfly, and 39.2% less than standard
  Goblin. It also finished ahead of every incumbent in this single concurrent
  trial. The report includes the full-state verification and raw evidence;
  final RSS is not peak memory.
- Added `--zset-implementation` so ordinary sorted-set commands can create any
  packed representation by default while existing and restored keys remain
  representation-pinned. The six `GOBLIN.PACKED_*.*` command families also allow
  per-key selection. Integer members use numeric tie ordering, UUIDs use
  binary lexicographic order, and FLOAT32 scores use binary32 precision; these
  specializations are opt-in, with `standard` remaining the default.
- Added `--packed-zset-merge-exponent` in `[0, 1]`, defaulting to `0.5`, to set
  the local dirty-tail threshold to `ceil(leaf_capacity ** k)`. Only mutations
  perform maintenance; reads never force a merge.
- Optimized packed leaf compaction with base-slot invalidation bitmaps,
  initialized-live-only scratch and run-based block moves; leaf redistribution
  now transfers boundary entries directly. Merge thresholds and tie ordering
  are unchanged.
- Fixed packed integer hash distribution for Swiss bucket selection without
  adding member storage. Packed updates now reuse score slots, prepare new
  insertions before tree mutation, and reserve bulk map capacity for distinct
  new members. Allocation-failure, collision and deep-routing regressions cover
  the unchanged ordering and merge policy. A measured shared-tree-path prototype
  was not retained because it did not provide a broad performance benefit.
- Reduced Swiss vacancy-probe work, removed the second Swiss lookup from
  packed point removals, and simplified live packed-tree tuple equality.
  Ordering, merge thresholds and per-member storage remain unchanged.
- Fixed standard sorted-set member positioning to binary-search the complete
  `(score, lexicographic member)` tuple across blocks, avoiding linear walks
  through large equal-score runs. Rescoring now reuses its known positions and
  reserves before moving entries; allocation failure restores the member
  snapshot without a reverse rescore. Also bounded rounded block capacities.

## v0.10.5 — August 20, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.5)

The Aeron transport release.

- Added optional [Aeron UDP and IPC](docs/aeron.md) transports. RESP and typed
  SBE share correlated response channels through an external Aeron 1.51+ Media
  Driver; matching C++ and redis-py-shaped Python clients are included.
- Published the
  [local transport latency matrix](blogs/aeron-local-transport-latency.md) on
  naamah: SBE over Aeron IPC measured 341 ns p50 PING, versus 13.9 µs p50 over
  Aeron UDP loopback.

## v0.10.4 — August 11, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.4)

The opt-in NVIDIA BlueField Pub/Sub edge release.

- Added a standalone RESP2/RESP3 edge for NVIDIA BlueField DPUs. Subscriptions
  register locally, DPU-originated publications fan out locally first, and
  ordinary commands remain authoritative on the host Goblin Core server.
- Added weighted aggregate host subscriptions and stable edge IDs so each host
  publication crosses the host-to-DPU path once without duplicate return
  delivery or inaccurate Redis-compatible subscriber counts.
- Added a directly polled XLIO Ultra listener with direct non-blocking fanout,
  fixed prefaulted output rings, CPU pinning, and standard RESP on the wire.
- Published the
  [direct 100 Gb/s BlueField DPU-side benchmark](BLUEFIELD-BENCHMARK.md):
  native XLIO Pub/Sub measured 12.5 microseconds p50 and 19.3 microseconds p99,
  versus 107.8 and 116.5 microseconds with a kernel TCP client against the same
  native Ultra DPU-resident edge.
- Kept BlueField out of ordinary builds by default. Enable the edge executable
  and integration test with `-DGOBLIN_CORE_BUILD_BLUEFIELD=ON`, or use the
  independent `bluefield/` AArch64 build.
- Included end-to-end tests, deployment documentation, and the complete
  Apache-compatible binary notice bundle for the edge and its selected
  XLIO/DPCP dependencies.

## v0.10.3 — August 11, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.3)

The NVIDIA BlueField Pub/Sub edge release.

- Added a standalone RESP2/RESP3 edge for NVIDIA BlueField DPUs. It registers
  literal and pattern subscriptions locally, delivers a DPU-originated
  publication to local subscribers before forwarding it, and proxies ordinary
  commands to the authoritative Goblin Core process on the host.
- Aggregated local interest into weighted host subscriptions so a host
  publication crosses the host-to-DPU path once, independent of local fanout.
  Stable edge IDs suppress the return path for DPU-originated publications
  without weakening Redis-compatible subscriber counts.
- Added a directly polled XLIO Ultra listener on the DPU. Standard RESP remains
  on the wire, while direct non-blocking fanout, fixed prefaulted output rings,
  CPU pinning, and strict polling keep the client-facing path in userspace.
- Published the
  [direct 100 Gb/s BlueField DPU-side benchmark](BLUEFIELD-BENCHMARK.md).
  With the same native XLIO client used for ConnectX-5 qualification, local
  Pub/Sub measured 12.5 microseconds p50 and 19.3 microseconds p99, versus
  107.8 and 116.5 microseconds with a kernel TCP client against the same native
  Ultra DPU-resident edge.
- Added the standalone AArch64 build, native latency probe, end-to-end socket
  tests, deployment documentation, and complete Apache-compatible binary
  notice bundle for the edge and its selected XLIO/DPCP dependencies.
- Published an independent native AWS EFA qualification run and added a
  reproducible rootless Podman/OCI source-build path.

## v0.10.2 — July 26, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.2)

The provider-neutral fabric and network-path qualification release.

- Added a busy-polled libfabric `FI_EP_RDM` transport carrying RESP2 or typed
  SBE through `FI_MSG`, `fi_send`/`fi_recv`, and completion queues. Production
  deployments can select AWS EFA with `--efa`; `--libfabric` selects providers
  such as `tcp` and `verbs;ofi_rxm` for local qualification.
- Added independent 64-bit request and reply sequences for every logical
  client, bounded out-of-order sequestering, exact-version handshakes, and
  automatic PING heartbeats. One endpoint serves multiple clients without
  weakening per-client command, reply, or Pub/Sub ordering.
- Integrated libfabric targets into Goblin's literal strict-priority poll
  order, added explicit trusted-fabric authentication policy, and provided
  `--libfabric-force-send` to exercise the full transmit completion path
  instead of eligible small-frame injection.
- Vendored AWS libfabric `2.4.0amzn5.0` under its permissive BSD option and
  added a reproducible static-library build helper, C++ client support,
  transport tests, installation guidance, and operational documentation.
- Published a 16-million-operation provider matrix over a direct 100 Gb/s
  ConnectX-5 link. SBE over `verbs;ofi_rxm` averaged 6.39 microseconds at p50
  and 156,498 sequential round trips per second across eight operations,
  versus 90.33 microseconds and 11,170 for RESP2 over kernel TCP.
- Removed a libfabric tail-latency defect by polling the TCP bootstrap listener
  before calling `accept()`. The immediate SBE/RDM control reduced average
  p99.99 from 857.47 to 29.47 microseconds; the independent full matrix
  reproduced it at 30.83 microseconds.

## v0.10.1 — July 25, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.1)

The snapshot-correctness and operational-hardening release.

- Restored distinct Redis persistence semantics: `SAVE`/`GOBLIN.SAVE` block
  until the snapshot has been fsynced and atomically installed, while
  `BGSAVE`/`GOBLIN.BGSAVE` fork and return immediately. Both forms now default
  to `dump.gcsn` and are available through RESP, typed SBE, and the Python
  client.
- Suspended keyspace, hash, list, set, array, and sorted-set arena compaction
  while a `BGSAVE` or `GOBLIN.DUMPWORLD` child holds a copy-on-write view.
  Ordinary writes and new arena blocks continue, avoiding whole-arena COW
  amplification without stopping the server. HugeTLB arenas and fork-unsafe
  transports reject `BGSAVE` and retain synchronous `SAVE`.
- Reworked no-argument literal-channel `UNSUBSCRIBE` to release the reverse
  subscription index in one batch instead of repeatedly erasing names, keeping
  large broken-client cleanup bounded by the subscription-table work itself.
- Added a C++/SBE market-feed Pub/Sub replay suite for wildcard and large
  literal subscription tables, with simdjson channel extraction, one-MiB ring
  support, delivery validation, and explicit subscribe/unsubscribe timing.
- Added a tested source installation guide with platform prerequisites, build
  profiles, CMake option reference, installation, and an installed-binary smoke
  test. The host-specific Thunder/Redpanda deployment record now lives under
  `docs/`.
- Added `redis-cli-ring` to the installed program set.
- Hardened ring-client startup against observing a newly created backing file
  before the server has sized it, avoiding a possible `SIGBUS` during an
  immediate client/server launch.

## v0.10.0 — July 22, 2026

[Source tag](https://github.com/adamdeprince/goblin-core/releases/tag/v0.10.0)

The broker-acknowledged durability, streamed-bootstrap, and accelerated-TCP
release.

- Added selectable Kafka acknowledgement semantics. `--kafka-ack-mode broker`
  withholds client replies and firehose batches until every record in the
  atomic mutation batch is broker-acknowledged; `queued` retains the original
  lower-latency behavior. Pending payloads have a configurable bounded
  backpressure watermark and `INFO` exposes delivery progress.
- Added `GOBLIN.DUMPWORLD`, which forks at one point in time and returns the
  entire database as a RESP3 streamed blob in native GCSN snapshot format. A
  receiving node can install the stream directly for new-replica bootstrap
  without blocking writes on the primary.
- Added native NVIDIA XLIO Ultra TCP listeners and clients for RESP and SBE.
  XLIO targets participate in literal strict poll priority alongside rings,
  RDMA, and ExaSock while remaining compatible with ordinary kernel TCP peers.
  On the NUMA-bound 100 Gb/s ConnectX-5 test pair, native XLIO delivered
  4.98-8.37 microsecond median round trips across seven Redis-shaped commands.

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
