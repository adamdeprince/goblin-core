# Goblin Core Lists: PMA and Segmented Listpacks

Goblin Core implements ordered Redis string lists with two large-list algorithm
classes:

- **Adaptive PMA**: a packed-memory array with bitmap rank/select,
  endpoint-biased slack, and a separate byte arena.
- **Segmented listpacks**: bounded inline-value leaves with a Fenwick count
  index for rank-to-leaf selection.

Both begin with the same single-allocation compact representation. The standard
Redis list names select one large-list implementation through
`--list-implementation`; the `GOBLIN.PMA.*` and `GOBLIN.SEGMENTED.*` command
families address either implementation explicitly. This lets both
representations coexist in one server and be measured under identical command
semantics.

## Shared Small-List Representation

A new list starts as one pooled listpack blob. Its eight-byte header is:

```text
[entries bytes: u32][entry count: u16][reserved: u16]
```

Each entry combines its length and storage form:

```text
0x00..0x7e        encoded payload, 0..126 bytes
0x80..0xfe        raw payload, 0..126 bytes; redundant 0xff omitted
0x7f  [length:u16] encoded payload, extended length
0xff  [length:u16] raw payload, extended length
```

The common raw 16-byte value therefore occupies 17 bytes. Entries have no
pointer, allocator header, alignment padding, or back-length. Indexing scans one
small contiguous blob. A command-sized batch builds the blob once instead of
copying its growing prefix for every argument.

The default promotion threshold is 32 entries. A list also promotes when the
single blob reaches its 65,535-byte encoded limit. When a large list shrinks
within both limits, it demotes back to this representation.

Each `List` object is 32 bytes and points to shared immutable configuration.
Only a promoted list allocates a large-representation state object.

## Adaptive PMA

The PMA separates logical order from value bytes. Empty slots distributed among
references make most inserts a local movement of compact references; value
bytes do not move during PMA redistribution.

### Reference Width

PMA references begin in a six-byte structure-of-arrays form:

```text
[tagged logical arena address: u32][encoded length: u16]
```

The high address bit records that a raw value's redundant `0xff` prefix was
omitted. The remaining 31 bits address the first 2 GiB of the list's arena.
When a list crosses that boundary, its references promote once to:

```text
[tagged arena block: u32][in-block offset: u32][encoded length: u16]
```

That wide form is ten bytes per PMA slot and preserves 32-bit block and offset
fields. Lists that fit below 2 GiB, including the benchmarked 100,000-element
workload, retain the six-byte form.

A packed occupancy bitmap marks live slots. A Fenwick tree counts occupied
slots per 64-bit bitmap word, providing logical-rank selection without a pointer
or rank in every entry.

### Value Arena

The byte arena uses Goblin Core's page-aware block allocator. A value never
straddles a block. Raw values omit their `0xff` encoder prefix; views synthesize
that byte without storing it. Specialized integer/UUID encodings and LZ4 values
retain their complete encoded payload.

The active last block grows geometrically. Full blocks are immutable. Explicit
optimization repacks live values, drops dead blocks, and shrinks the active tail
to its page-rounded live size. A HugeTLB tail is copied into ordinary pages when
it becomes the compactable tail, so optimization does not retain an otherwise
mostly empty huge page.

### PMA Mutation Policy

One multi-value push appends all value bytes and inserts the complete reference
span with one PMA mutation. `RPUSH` preserves argument order; `LPUSH` reverses
the span to match Redis semantics.

A mutation proceeds from cheapest to broadest work:

1. Consume contiguous endpoint slack when available.
2. Shift toward a nearby empty slot for a single middle insert.
3. Redistribute the smallest sufficiently sparse local window.
4. Grow geometrically and redistribute the full PMA.

Existing slack is always usable, even when occupancy is at the configured
density watermark. This avoids a full grow/shrink cycle for one transient
insert into a freshly compacted list.

Redistribution reserves half of available slack for recently active global
endpoints; the rest remains distributed. Erase clears one occupancy bit. PMA
capacity shrinks only after two geometric steps of hysteresis, then returns one
step, preventing a push/pop pair at a boundary from rebuilding the array twice.

Replacing or deleting a value marks its old arena bytes reclaimable. Automatic
compaction runs when dead bytes are both substantial and dominant;
`GOBLIN.OPTIMIZE` forces a tight rebuild.

## Segmented Listpacks

The segmented implementation stores values inline in leaves of at most 128
entries. A Fenwick tree stores one count per leaf. Rank lookup selects a leaf in
`O(log leaves)`, then scans at most one compact leaf.

Pipelined pushes build complete leaves with one allocation each. Middle
overflows split into balanced siblings. Endpoint overflows keep interior leaves
full and put slack at the active edge, so stack and queue churn does not
accumulate half-full leaves. Deletes merge adjacent leaves when their combined
entries fit. Large values can reach the 64 KiB byte ceiling before the entry
ceiling; that path packs the largest fitting range into each leaf.

This representation removes the PMA's per-element arena reference and keeps
the values that participate in a lookup together. Its tradeoff is copying one
bounded leaf on mutation. It is designed for memory-dense lists and locality;
the PMA remains the stronger choice for frequent large middle mutations.

The shared string encoder applies inside both implementations. Per-value LZ4
is enabled by `--use-lz4`; `--disable-encoding` stores client-supplied bytes
verbatim in both representations.

## Tuning

- `--list-implementation pma|segmented` selects the backend for standard Redis
  list command names. The default is `segmented`.
- `--list-max-density FRACTION` controls maximum PMA occupancy. The default is
  `0.97`.
- `--list-resize-growth FACTOR` controls geometric PMA and arena-tail growth.
  The default is `2**0.25`, approximately `1.189207115`.
- `--list-chunk-bytes BYTES` controls PMA value-arena block size. It must be a
  power of two from 65,536 bytes through 4 GiB; the default is 2 MiB.

Packing density and resize growth are independent. Raising density saves
reference-array memory but leaves less insertion slack. Raising growth reduces
resize frequency but reserves more memory after growth. Segmented lists do not
use PMA density or arena chunk settings.

## Complexity

| Operation | Adaptive PMA | Segmented listpacks |
|---|---|---|
| Index lookup | `O(log bitmap words)` | `O(log leaves + leaf scan)` |
| Head/tail insert | `O(1)` with slack; occasional rebuild | one bounded leaf copy; occasional split |
| Middle insert | bounded shift/window; occasional `O(n)` grow | one bounded leaf copy; occasional split |
| Pop/erase | `O(log bitmap words)`; occasional shrink | rank select plus one bounded leaf copy/merge |
| Range traversal | rank select plus physical scan | rank select plus sequential leaf scans |
| Value/pivot scan | linear physical scan | linear leaf scan |

Both implementations are memory-first, while preserving Redis ordering and
atomic command execution in Goblin Core's single-owner path.

## Persistence

Native snapshots always store values in canonical list order, not either
implementation's internal addresses. Default `ACCEL` snapshots add a versioned
two-byte marker when every value is already in the raw representation. In that
common case the canonical bytes are the final payload: loading copies them
directly into listpack leaves or the PMA arena and builds the rank structure
once. Specialized integer/UUID/LZ4 values, `NOACCEL`, an encoding-policy
mismatch, or an accelerator-version mismatch use the canonical one-build path;
they still do not replay one mutation per value. Redis RDB import uses that same
bulk construction path for plain lists, ziplist lists, quicklists, and
quicklist2 nodes containing plain values or listpacks.

## Commands

The implemented suffixes are `LPUSH`, `RPUSH`, `LPUSHX`, `RPUSHX`, `LPOP`,
`RPOP`, `LMOVE`, `RPOPLPUSH`, `BLPOP`, `BRPOP`, `BLMOVE`, `LMPOP`, `BLMPOP`,
`LLEN`, `LINDEX`, `LRANGE`, `LSET`, `LTRIM`, `LREM`, and `LINSERT`.
Prefix any suffix with `GOBLIN.PMA.` or `GOBLIN.SEGMENTED.` to select a concrete
implementation for a new key. Standard names resolve through
`--list-implementation`.

Redis calls `BLPOP`, `BRPOP`, and related operations "blocking" because an
empty-list request parks the client until a producer supplies a value. Goblin
Core parks only that connection: other clients continue to execute, commands
remain atomic, waiters wake FIFO, and a multi-key request checks keys in the
client's declared order. `MULTI`/`EXEC` and scripts use the non-blocking form so
an atomic block can never suspend the server.

The repeatable 100,000-element comparison, including RESP population,
rank-sensitive reads, middle and endpoint mutations, RSS, and per-key allocation, is in
[LIST-BENCHMARK.md](LIST-BENCHMARK.md).
