# Goblin Core release history

Goblin Core releases are source releases: each version is a Git tag and its
corresponding source archive. The project is still pre-1.0, so the supported
Redis surface can expand substantially between minor versions.

For the current command surface, build instructions, and compatibility limits,
see the [project documentation](README.md). For changes after the latest tag,
see the [repository history](https://github.com/adamdeprince/goblin-core/commits/main/).

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
