# PMA Lists: Adaptive Packed-Memory Array Design

Goblin Core's large-list algorithm class is an **adaptive packed-memory array
(PMA) with bitmap rank/select and endpoint-biased slack**. Small lists use a
single listpack allocation instead. Both representations preserve Redis list
order. PMA is one concrete list implementation; it has its own
`GOBLIN.PMA.*` command family rather than claiming the entire list namespace.

This is an ordered-sequence data structure, not a linked list. The PMA keeps
empty slots among compact arena references so inserts usually move a local run
of references. It does not move or copy the value bytes during PMA
redistribution.

## Representation

### Small lists: one listpack

A new list starts as one pooled blob. Each entry is:

```text
[value length: u16][value bytes]
```

The blob has an eight-byte header containing its byte length and element count.
Values are inline, so an empty or very small list creates no value arena and has
no per-element allocation. The default promotion threshold is 32 entries; the
list also promotes if its single blob would exceed 65,535 bytes.

When a full list shrinks back within both limits, it demotes to the one-blob
form. Promotion and demotion preserve order.

### Large lists: PMA plus value arena

The full representation separates order from bytes:

- The PMA stores references as three structure-of-arrays columns: a 32-bit arena
  block, a 32-bit in-block offset, and a 16-bit value length. That is 10 bytes
  per PMA slot before occupancy and rank metadata.
- A packed occupancy bitmap marks live PMA slots.
- A Fenwick tree counts occupied slots per 64-bit bitmap word. It provides
  logical-rank to physical-slot selection without storing a pointer or rank in
  every entry.
- The byte arena reuses Goblin Core's page-aware arena allocation machinery.
  Its active last block grows geometrically; full blocks are immutable, and a
  rebuild repacks live values and releases dead blocks/pages.

The split 32/32 address avoids a 4 GiB whole-arena ceiling. A value never
straddles an arena block. Goblin Core limits each key and value to 65,535 bytes,
so the two-byte length is sufficient.

## PMA Mutation Policy

One multi-value push appends all value bytes, then inserts the entire ordered
reference span with one PMA mutation. `RPUSH` preserves the supplied span;
`LPUSH` reverses it before insertion to match Redis semantics. A batch either
uses one contiguous endpoint gap, redistributes one sufficiently sparse window,
or performs one geometric rebuild. It never repeats those operations once per
value.

A single-value insert proceeds from cheapest to broadest work:

1. A head or tail operation consumes an adjacent endpoint gap when one exists.
2. A middle insert shifts toward the nearest gap within a bounded local run.
3. If neither side has nearby space, the smallest sufficiently sparse PMA
   window is redistributed.
4. If the whole PMA would cross its configured maximum density, it grows and is
   redistributed once.

Redistribution reserves half of the available slack explicitly for active
global endpoints. When both endpoints are active, their recent-activity weights
divide that reservation; the other half remains distributed among all gaps.
Middle operations decay the endpoint activity. Unlike a small endpoint weight
among `n+1` gaps, this quota remains meaningful on a 100,000-element list: at
the default density, a compacted tail-biased list reserves roughly 1,500
contiguous tail slots.

Erase clears one occupancy bit. The PMA shrinks only when the minimum required
capacity is two geometric resize steps below current capacity, then gives back
one step. This hysteresis prevents a push/pop pair at the density boundary from
rebuilding the whole PMA twice. Replacing or deleting a value marks its old
arena bytes reclaimable; automatic compaction runs once dead bytes are
substantial and dominate the arena, while `GOBLIN.OPTIMIZE` forces a tight
rebuild.

The PMA moves only the compact references. Arena compaction is the operation
that copies live value bytes and updates their 32/32 addresses.

## Independent Tuning Controls

Packing density and resize growth are deliberately separate policies:

- `--list-max-density FRACTION` controls the maximum occupied fraction of PMA
  slots. The default is `0.97`.
- `--list-resize-growth FACTOR` controls geometric PMA and active-arena growth.
  The default is `2**0.25`, approximately `1.189207115`.
- `--list-chunk-bytes BYTES` controls value-arena block size. It must be a power
  of two from 65,536 bytes through 4 GiB; the default is 1 MiB.
- `--list-implementation pma` selects the concrete implementation used by the
  standard Redis list command names. `pma` is the sole enum value and default
  today; future implementations will add values without changing the qualified
  command families.

Raising density saves reference-array memory but leaves less insertion slack.
Raising growth performs fewer resizes but reserves more memory after each one.
Changing one does not alter the other.

## Complexity

- Logical index lookup: `O(log(bitmap words))` rank/select, then one arena read.
- Head/tail insert with adjacent slack: `O(1)`.
- Other inserts: bounded shift or `O(window)` local redistribution; occasional
  `O(n)` geometric growth.
- Pop/erase: `O(log(bitmap words))`, with occasional `O(n)` hysteretic shrink.
- Range traversal: one `O(log(bitmap words))` select followed by a linear bitmap
  scan across the returned elements and the PMA gaps between them.
- Pivot/value scans: one select followed by a linear physical scan; matches do
  not pay a rank/select lookup for every element examined.
- Small-list operations: `O(n)` over one cache-friendly blob.

The implementation favors memory first, while keeping endpoint operations and
rank lookup fast enough for Redis list workloads.

The repeatable 100,000-element comparison, including load, middle indexing,
endpoint churn, RSS, per-key memory, and the density sweep, is in
[LIST-BENCHMARK.md](LIST-BENCHMARK.md).

## Persistence

Native snapshots store list values in canonical order. PMA slots, bitmap/Fenwick
metadata, endpoint bias, and arena addresses are rebuilt under the loading
server's current tuning. Redis RDB import accepts plain lists, ziplist lists,
quicklists, and quicklist2 nodes containing plain values or listpacks.

## Command Semantics

The PMA surface is `GOBLIN.PMA.LPUSH`, `GOBLIN.PMA.RPUSH`,
`GOBLIN.PMA.LPUSHX`, `GOBLIN.PMA.RPUSHX`, `GOBLIN.PMA.LPOP`,
`GOBLIN.PMA.RPOP`, `GOBLIN.PMA.LLEN`, `GOBLIN.PMA.LINDEX`,
`GOBLIN.PMA.LRANGE`, `GOBLIN.PMA.LSET`, `GOBLIN.PMA.LTRIM`,
`GOBLIN.PMA.LREM`, and `GOBLIN.PMA.LINSERT`. Standard names such as `LPUSH`
resolve through `--list-implementation`; they map to this same PMA family while
`pma` is selected.

Every command is atomic under Goblin Core's existing single-owner execution
path. Redis calls `BLPOP`, `BRPOP`, and related commands "blocking" because an
empty-list call parks the client until another command supplies a value. Those
commands require parked-client wakeups in the server event loop; "blocking" in
that name is unrelated to atomicity.
