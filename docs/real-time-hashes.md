# Real-time hash indexes

Goblin Core has two hash representations. They share the same packed field/value
arena, string encoder, LZ4 policy, command semantics, and 64 KiB field limit.
Only the field index and small-hash policy differ.

| Implementation | Index | Small hashes | Primary goal |
|---|---|---|---|
| efficient | compact listpack, then the dense Swiss member index | inline in the shared keyspace arena | minimum memory and maximum throughput |
| RT | linear hashing over small Swiss buckets | always indexed | bounded growth work and steadier write latency |

## Selecting an implementation

Standard hash commands create the implementation selected by:

```text
--hash-implementation efficient
--hash-implementation rt
```

The default is `efficient`. `--real-time` selects RT hashes and also replaces the
top-level keyspace's dense Swiss index with the same incremental linear-hash
algorithm. It does not change sorted-set or list storage.

Every standard hash command also has an implementation-qualified name:

```text
GOBLIN.RT.HSET key field value [field value ...]
GOBLIN.EFFICENT.HSET key field value [field value ...]
```

The same prefixes exist for `HSETNX`, `HGET`, `HMGET`, `HDEL`, `HGETALL`,
`HKEYS`, `HVALS`, `HLEN`, `HEXISTS`, `HSTRLEN`, and `HINCRBY`. The
`GOBLIN.EFFICENT` spelling is part of this command surface (historical; hashes only).
Array classic mode uses `GOBLIN.CLASSIC.AR*` — a different family.

A selector applies only when a command creates a key. Once created, a hash keeps
its implementation until it is deleted and recreated. Calling an efficient
alias on an RT hash, or an RT alias on an efficient hash, operates on the
existing representation and never hides an O(n) conversion inside a command.

## Algorithm

The RT index follows [Witold Litwin's linear hashing
scheme](https://www.cs.bu.edu/faculty/gkollios/cs562s18/linear-hashing.PDF).
Its state is a power-of-two level size and a split cursor. A lookup first computes
the bucket at the next level; when that bucket has not been created yet, it falls
back to the current-level bucket. Buckets split in cursor order, so growth appends
one bucket rather than allocating and rebuilding a second table.

Each logical bucket is a chain of 16-slot physical buckets. The hot plane is one
64-byte cache line containing all sixteen folded 32-bit XXH3 hashes. A SIMD probe
compares the complete hashes before Goblin reads the cold plane's packed member
IDs and field bytes. The cold plane also holds a 16-bit occupancy mask, count,
owner, and forward/backward 32-bit overflow links. Incremental split and merge
therefore never rehash field bytes, and returning an empty overflow bucket does
not scan its chain. The target is 13 live entries per primary bucket (~81%
primary load), reserving slack for overflow while a split catches up.

The first primary bucket lives inline in the index object. Subsequent primaries
occupy immutable extents of 4, 8, 16, 32, 64, and 128 buckets, followed by
repeated 64-bucket terminal extents. Six handles cover the complete tiered
prefix; four more inline handles cover the first terminal extents. Still-larger
indexes address terminal extent bases through flat 512-entry directory blocks.
A primary lookup is arithmetic plus one extent-base lookup; lookup depth does not
grow with the index. Extents are released only after reverse linear hashing has
drained their final logical bucket.

Hot and cold planes use a shared, prefaulted buddy arena whose physical
superblock is 256 buckets. Overflow buckets use order 0 and primary extents use
orders 2 through 7. A serving allocation performs at most eight bounded splits;
release performs at most eight bounded coalesces. Availability vectors reserve
their maximum capacity during arena construction, so these operations do not
call `malloc` or `mmap`, move live buckets, or clear unrelated storage. The hot
plane of a physical superblock is exactly 16 KiB. A 64-bucket terminal extent
occupies one 4 KiB quarter of that plane and reserves 9 KiB across both planes,
limiting unused terminal capacity while retaining natural locality on both 4 KiB
and 16 KiB-page systems. Extent reservation does not initialize all of its
buckets: each split initializes only the one primary becoming logical. Each index
cell accounts for 64 hot bytes plus its natural 80-byte cold record: 144 bytes
rather than a power-of-two envelope.

Directory tables and blocks come from separate shared, prefaulted pools and
return to free lists as an index contracts. They are needed only after the six
tiered and four inline terminal handles are exhausted.

A split is itself incremental. One mutating operation migrates at most one
physical 16-slot bucket. While a split is active, a lookup for the new high-half
bucket checks the destination and then the not-yet-migrated source. New writes go
straight to their final side. A single-field `HSET` performs this fixed unit of
maintenance for a structural insertion, and `HDEL` does the same for a structural
erase. Multi-field `HSET` pays one physical-group maintenance step for each
structural insertion, so a batch cannot leave the index under-split with a long
overflow chain.

Deletion contracts the same structure in reverse. When average occupancy falls
below the low-water mark, a reverse linear-hash merge moves at most one physical
16-slot bucket per mutation. Empty overflow buckets return to the shared arena
immediately, and a completed merge returns the final primary bucket and any
empty flat directory blocks and table. Shrinking therefore does not hide a
table-wide rebuild.

Field keys are hashed with XXH3 and folded to 32 bits before the linear bucket
address and fingerprint are taken. Patterned string keys therefore do not turn
weak low bits into long overflow chains.

## Operational tradeoffs

- RT hashes skip the compact listpack, so very small hashes still consume more
  memory than the efficient implementation. Tiered extents avoid making a small
  RT hash reserve even a 64-bucket terminal extent.
- Serving growth and shrink never perform a table-wide index rehash.
- `--rt-hash-index-bytes BYTES` sizes the shared fixed-record bucket-cell pool.
  The default is 16 MiB and values round up to 2 MiB slabs. Goblin derives and
  prefaults the buddy metadata plus enough flat-directory blocks and block tables
  for any partition of those cells across RT hashes; `INFO` reports the complete committed total as
  `realtime_hash_index_pool_bytes`. An insert that needs an unavailable bucket,
  contiguous primary extent, directory block, or table fails with an explicit
  instruction to increase the pool. Nothing silently grows on the serving path.
  The current one-million-field HSET benchmark uses a 20 MiB configured arena;
  its cached-hash cell population does not fit in the 16 MiB default.
- Select RT at startup with `--hash-implementation rt` or `--real-time` when
  bounded first-use latency matters. On an otherwise efficient server, the first
  qualified `GOBLIN.RT.*` command initializes the shared pool lazily.
- `GOBLIN.OPTIMIZE` compacts RT field/value bytes while leaving the stable RT
  index intact. Snapshot load and store rebinding may bulk-build an index because
  they are explicit non-serving paths.
- Snapshots remain canonical field/value streams and record each hash's selected
  implementation. RT snapshots deliberately omit the implementation-specific
  Swiss accelerator and rebuild the RT index on load. Version-2 snapshots,
  written before that per-key tag existed, use the server's configured default.

The bound above is an index bound, not yet a formal end-to-end hard real-time
guarantee. Field/value storage still uses growable packed arenas and vectors, and
socket scheduling, persistence, scripting, and explicit administrative work have
their own latency behavior. RT removes the field index's stop-the-world rehash
and amortized shrink; it does not claim that every server path has been proven
allocation-free or assigned a worst-case execution time.

The RT implementation is intended for workloads where the cost of an occasional
dense-table rehash matters more than the extra bucket slack. The efficient
implementation remains the memory-first choice.
