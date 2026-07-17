# INFO

```
INFO [section]
```

Returns a bulk string of server and memory statistics in the standard Redis
`# Section` / `field:value` (CRLF-separated) format, so `redis-cli`, monitoring
agents, and benchmark harnesses read it unchanged. Goblin Core emits a deliberately
small subset — a `# Server` section and a `# Memory` section. The optional
`section` argument is accepted but ignored: `INFO` and `INFO memory` both return
the full payload.

## `# Server`

| field | value |
|---|---|
| `redis_version` | `7.4.0` — the wire/behaviour compatibility level |
| `redis_mode` | `standalone` |
| `list_implementation` | concrete backend selected for standard list commands (`pma` or `segmented`; default `segmented`) |
| `hash_implementation` | representation selected for hashes created by standard commands (`efficient` or `rt`) |
| `keyspace_index` | top-level key index (`swiss`, or incremental `linear` under `--real-time`) |

## `# Memory`

| field | meaning |
|---|---|
| `used_memory` | what Goblin's allocation layers hold: arena live + dead bytes, fixed index/table structures, full-hash heap state, and actual upstream blob-pool capacity |
| `used_memory_rss` | the OS resident set size (`ps`-equivalent) |
| `used_memory_peak` | currently mirrors `used_memory` — Goblin does not yet track a separate high-water mark |
| `mem_reclaimable_bytes` | the **dead** subset of `used_memory`: bytes orphaned by deletes/overwrites that a compaction would free |
| `hash_heap_allocated_bytes` | heap-resident state owned by promoted hashes; compact hash handles and blobs live in the keyspace slot/arena instead |
| `realtime_hash_index_pool_bytes` | committed bytes in the fixed, prefaulted arena shared by all RT hash field indexes; `0` until that arena is initialized |
| `realtime_keyspace_index_pool_bytes` | committed bytes in the separate fixed RT keyspace-index arena selected by `--real-time`; otherwise `0` |
| `blob_pool_requested_bytes` | live compact zset/list blob bytes requested through the shared blob allocator |
| `blob_pool_capacity_bytes` | bytes the blob allocator currently holds from its upstream allocator, including live direct blobs over 1 KiB; this amount is included once in `used_memory` |
| `blob_pool_fragmentation_bytes` | retained pool capacity beyond live requested blob bytes; unlike arena dead bytes, `GOBLIN.OPTIMIZE` does not reclaim it |
| `blob_pool_live_allocations` | number of live blobs in the pool |
| `blob_pool_upstream_allocations` | cumulative allocations the pool has requested from its upstream resource |
| `maxmemory` | `0` — no limit |
| `maxmemory_policy` | `noeviction` — Goblin never evicts |
| `mem_fragmentation_ratio` | **internal** fragmentation — see below |

`used_memory` is a real number, not the RSS: it comes from
[`GOBLIN.MEMORY`](goblin.md)-style accounting rolled up across every zset, hash,
list, and the keyspace. Each arena maintains its live/dead byte counts
incrementally, so the rollup reads counters rather than walking members or
reparsing blobs. The shared compact-blob pool is tracked at its upstream boundary,
which makes size-class rounding and retained slabs visible instead of silently
reporting only requested payload bytes.

## Fragmentation: a different meaning than Redis

`mem_fragmentation_ratio` is a Redis field, but Goblin Core measures the *opposite
end of the stack* with it. Same name, different question — worth understanding
before you alert on it.

### Redis — external fragmentation (`rss / used_memory`)

Redis asks its allocator (jemalloc) for the bytes it needs. To serve those
requests the allocator holds *more* than that resident — size-class rounding,
per-arena free lists it hasn't returned to the OS, and so on. So in Redis:

- `used_memory` is what the application **requested** (roughly the live data),
- `used_memory_rss` is what the OS actually backs,
- `mem_fragmentation_ratio = rss / used_memory` is the gap — overhead the allocator
  and OS impose **below** the application.

A ratio of `1.4` means jemalloc/OS holds ~40% more than the live data. Redis fights
this with jemalloc *active-defrag*, which relocates allocations in the background.
(A ratio **below** `1.0` is a separate alarm: part of the process has been swapped
to disk.)

### Goblin Core — internal fragmentation (`used_memory / compacted size`)

Goblin manages its own page-aligned `mmap` arenas for the bulk of its data, so it
**is** the allocator for what matters. `used_memory` also includes the actual
upstream capacity of the remaining compact-blob pool, not merely the live requests
inside it. Process code, stacks, shared libraries, and general heap metadata still
make RSS larger, so RSS remains the cross-engine benchmark number.

The fragmentation Goblin can actively compact lives **above** the allocator,
inside its own arenas. Those arenas are append-structured: a deleted or overwritten
member's bytes are *orphaned* — still mapped, no longer referenced — until a
compaction rewrites the arena. Those `mem_reclaimable_bytes` are the actionable
waste used by the ratio. Pool slack is visible separately as
`blob_pool_fragmentation_bytes` and is not treated as compactable:

```
mem_fragmentation_ratio = used_memory / (used_memory − mem_reclaimable_bytes)
```

- `1.00` — every allocated byte is live; nothing to reclaim.
- `1.50` — a third of the allocation is dead bytes a compaction would free.
- It never drops below `1.00` (dead bytes are never negative), so there is no
  "swapped out" reading to confuse it with.

The remedy differs, and deliberately: where Redis leans on the allocator's
probabilistic background defrag, Goblin gives you a **deterministic** one —
[`GOBLIN.OPTIMIZE key`](goblin.md) rewrites a key's arena tight. Full hashes
instead auto-evacuate fragmented arena chunks in bounded maintenance steps once
dead bytes exceed live (past a floor). `mem_fragmentation_ratio`
is precisely the signal for when that's worth doing.

### In one line

> Redis's ratio asks *"how much is my allocator wasting underneath me?"*
> Goblin's asks *"how much of what I've allocated is reclaimable dead weight I can
> compact away?"* — one is the layer below you that you don't control, the other is
> your own data layout, which you do.

### What this looks like

```
> INFO memory        # freshly loaded, tightly packed
used_memory:142778
used_memory_rss:3080192
mem_reclaimable_bytes:0
mem_fragmentation_ratio:1.00

# ... after deleting ~60% of a large zset ...
> INFO memory
used_memory:118018
mem_reclaimable_bytes:39000
mem_fragmentation_ratio:1.49      # a third of the allocation is now reclaimable

> GOBLIN.OPTIMIZE board
(integer) 58796                   # bytes reclaimed
> INFO memory
mem_reclaimable_bytes:0
mem_fragmentation_ratio:1.00      # compacted back to tight
```

## See also

- [`GOBLIN.OPTIMIZE`](goblin.md) — the deterministic compaction the ratio signals
  for; [`GOBLIN.MEMORY`](goblin.md) — per-key memory breakdown.
- [Goblin extension commands](goblin.md), and the memory
  [benchmarks](../../BENCHMARKS.md) (RSS is the headline there).
