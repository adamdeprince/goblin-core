# Fixed-width packed sorted sets

Packed sorted sets trade general string members for a fixed-width binary member
and score representation. Select one of six command prefixes:

| Prefix | Default-selector value | Member bytes | Score bytes |
|---|---|---:|---:|
| `GOBLIN.PACKED_INT32_FLOAT32` | `packed-int32-float32` | 4 | 4 |
| `GOBLIN.PACKED_INT32_FLOAT64` | `packed-int32-float64` | 4 | 8 |
| `GOBLIN.PACKED_INT64_FLOAT32` | `packed-int64-float32` | 8 | 4 |
| `GOBLIN.PACKED_INT64_FLOAT64` | `packed-int64-float64` | 8 | 8 |
| `GOBLIN.PACKED_UUID_FLOAT32` | `packed-uuid-float32` | 16 | 4 |
| `GOBLIN.PACKED_UUID_FLOAT64` | `packed-uuid-float64` | 16 | 8 |

Append a dot and any sorted-set command supported by Goblin Core. For example:

```text
GOBLIN.PACKED_INT32_FLOAT32.ZADD temperatures 21.5 1001 19.75 1002
GOBLIN.PACKED_INT32_FLOAT32.ZRANGE temperatures 0 -1 WITHSCORES
GOBLIN.PACKED_INT32_FLOAT32.ZSCORE temperatures 1001
```

Ordinary `Z*` commands create the standard string-member/binary64-score
representation by default. Select a packed default at startup when the entire
workload has one member and score shape:

```text
goblin-core --zset-implementation packed-int32-float32
```

`--zset-implementation standard` selects the original representation
explicitly. The selector affects only newly created keys and aggregate-store
destinations. Ordinary commands discover and continue using the representation
of an existing key, including a key restored from a snapshot.

The complete surface is `ZADD`, `ZINCRBY`, `ZCARD`, `ZCOUNT`, `ZRANGE`,
`ZRANGEBYSCORE`, `ZREVRANGE`, `ZREVRANGEBYSCORE`, `ZRANK`, `ZREVRANK`,
`ZSCORE`, `ZMSCORE`, `ZREM`, `ZREMRANGEBYSCORE`, `ZREMRANGEBYRANK`,
`ZINTERSTORE`, `ZUNIONSTORE`, `ZPOPMIN`, `ZPOPMAX`, and `ZSCAN`. Options and
reply shapes match the [ordinary sorted-set commands](commands/sorted-sets.md).

## Representation rules

Integer members must be canonicalizable signed decimal values in the selected
width. UUID members accept either 32 hexadecimal digits or the dashed 8-4-4-4-12
form, case-insensitively. Replies use canonical decimal integers and lowercase,
dashed UUIDs. Equal-score ties use numeric integer order or the UUID's 16-byte
lexicographic order, rather than the spelling of the input.

`FLOAT32` narrows every stored score to IEEE binary32; `FLOAT64` stores IEEE
binary64. NaN and finite values that overflow the selected representation are
rejected. Score replies expose the exact stored value, so a binary32 score such
as `0.1` may be rendered as `0.10000000149011612`.

A key is pinned to the representation that created it. Using another packed
prefix against that key returns `WRONGTYPE`; an ordinary `Z*` command uses the
key's pinned representation. Generic key commands still see the value as a
`zset`: `TYPE` returns `zset`, and TTL, rename, copy, delete, scan, snapshot, and
replication behavior is shared with other key types. A representation-qualified
aggregate store requires packed inputs of that same kind. An ordinary
`ZINTERSTORE` or `ZUNIONSTORE` accepts standard and packed inputs and writes its
destination in the configured default representation.

## Storage and merge behavior

The member-to-score direction is one Swiss table whose keys and scores are the
selected binary widths. The score-to-member direction is a high-fanout B+ tree.
Its leaves target 4 KiB of sorted score/member pairs, while branch and leaf
references are 32-bit indices into contiguous arenas rather than per-entry
pointers. Branches store full `(score, member)` fence keys and live subtree
counts, providing logarithmic score seeks and rank selection. Leaves are linked
for sequential range output.

Integer members use a 64-bit avalanche mixer before Swiss bucket selection,
so sequential, negative and high-bit integer IDs distribute across the table.
The UUID hash retains its existing mixing and output. These hash changes do
not add per-member storage or affect score/member ordering.

Single-member updates reuse their found Swiss score slot and publish the new
score only after the tree mutation succeeds. New members prepare a vacant
slot, reserving capacity before changing the tree, then commit without another
lookup or allocation. Bulk map preflight counts distinct absent members rather
than reserving a slot for every update.

Point removals also reuse the found Swiss slot, erasing it only after tree
mutation. Vacancy probing skips unnecessary tombstone checks, and table
copy/rehash destinations use empty-only probes. Live tree tuples use direct
score/key equality checks; their ordering comparator is unchanged. These
optimizations add no persistent or per-member storage.

Each leaf is split into a sorted base and a small dirty tail. The tail contains
at most one record per changed member: another update overwrites that record,
and a removal records a NaN tombstone. A read reconciles only the leaves it
visits and never performs maintenance. When a leaf becomes sparse, neighboring
leaves redistribute their live entries or merge, preventing score migrations
from accumulating empty blocks.

The command-line option `--packed-zset-merge-exponent K` sets the per-leaf
threshold to `ceil(leaf_capacity ** K)`, with `K` in `[0, 1]` and a default of
`0.5`. Thus `0` folds every mutation into its leaf's sorted base, `0.5` uses a
square-root-sized local tail, and `1` permits a tail as large as the leaf's
sorted capacity. The setting is server policy rather than snapshot data, so a
restored key uses the receiving server's configured exponent.

Leaf compaction removes tombstones and shadowed base records, sorts the live
dirty records, and performs a backward merge into the leaf's reserved storage.
The first update or deletion of a base tuple marks its stable slot in a per-leaf
invalidation bitmap; later updates reuse the dirty record. Merges and reads
therefore skip obsolete base tuples without searching the dirty tail for each
one. INT32/FLOAT32 uses 64 bitmap bytes per leaf-arena slot. First invalidation
adds one binary search of the old `(score, member)` tuple in that leaf.

Compaction visits the bitmap's set bits and moves surviving runs, preserving
the unchanged prefix. The backward merge locates runs with exponential probes
and moves them in blocks. Deletion-only compactions need no addition merge;
ordered additions beyond the surviving base append directly. Bounded stack
scratch constructs only live dirty tuples, and neighboring leaves redistribute
by transferring their boundary run without a two-leaf temporary array. For a
leaf capacity `B` and `D` dirty records, compaction is `O(B + D log D)`.

It never allocates an order-sized temporary vector. `GOBLIN.OPTIMIZE key` forces
all dirty leaves to compact, while `GOBLIN.MEMORY key` reports the
representation, exponent, per-leaf threshold, and aggregate sorted/dirty entry
counts.

## Score run-length encoding

Sorted-base score compression is enabled by default for all six packed
representations. Select a layout with:

```text
goblin-core --zset-implementation packed-int32-float32
```

Use `--no-packed-zset-score-rle` to disable compression, or
`--packed-zset-score-rle` to explicitly enable it. The option applies to keys created by
representation-qualified commands, copies, aggregate stores, and snapshot
restores. Ordinary sorted sets retain their existing storage. Like the merge
exponent, this is server policy: snapshots retain the member/score type and
logical contents, and use the receiving server's compression setting.

Each compressed-layout leaf stores its sorted member IDs separately from its
score stream. Four or more adjacent equal scores become `[NaN, count, score]`.
All three fields occupy one score-width word; the count is a binary `uint32`
for FLOAT32 or `uint64` for FLOAT64. Shorter runs remain literal scores. NaN
markers are internal and never appear in replies. Dirty tails keep their
ordinary tuples and NaN deletion markers in separate storage.

Logical leaf capacities and merge thresholds stay the same. A checkpoint every
32 base positions bounds score decoding for binary-search probes; sequential
reads decode each run once. Compaction rebuilds one leaf of logical entries;
redistribution handles at most two leaves. All scratch space is bounded by the
fixed leaf capacity. Runs end at leaf boundaries, and rank counts and
invalidation bits still count members.

Score buffers grow before a mutation is committed and can shrink as scores
become more repetitive. If memory pressure prevents optional redistribution
or shrinking, the existing leaves remain valid and maintenance can be retried
by later writes. The separately allocated buffers and checkpoints add overhead
for diverse scores; compression benefits workloads with many ties and adds
encoding work during compaction. The opt-out retains the raw score layout for
workloads where that tradeoff is preferable.

In the [full Wikimedia comparison](../benchmarks/wikimedia_history_2026_08/rle-full-comparison-20260922.md),
all 12 RLE on/off variants passed verification at 80,798,328 members. RLE reduced
final RSS by 8.08–36.52% across the six layouts. INT32/FLOAT32 used 1.626 GiB
instead of 2.089 GiB; measured replay-time differences were below 1% in one
concurrent trial per variant.

`GOBLIN.MEMORY key` additionally reports `score_rle` (0 or 1),
`compressed_leaf_count` (leaves currently containing encoded runs), and
`sorted_score_bytes` (used score-stream bytes, excluding reserved capacity,
members, checkpoints, and dirty records). `total_allocated_bytes` includes all
of those allocations and is the appropriate counter for comparing footprints.

Native snapshots serialize canonical logical entries, not append history, and
restore the same packed representation. Representation-qualified names are part
of the RESP command surface. The unqualified typed SBE sorted-set templates also
follow `--zset-implementation`; the SBE schema does not define separate
representation-qualified templates.
