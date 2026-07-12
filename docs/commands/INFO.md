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
| `list_implementation` | concrete backend selected for standard list commands (`pma` today) |

## `# Memory`

| field | meaning |
|---|---|
| `used_memory` | what our allocation layers actually hold: every arena's live + dead bytes plus the fixed index/table structures, summed from O(1) counters (no scan) |
| `used_memory_rss` | the OS resident set size (`ps`-equivalent) |
| `used_memory_peak` | currently mirrors `used_memory` — Goblin does not yet track a separate high-water mark |
| `mem_reclaimable_bytes` | the **dead** subset of `used_memory`: bytes orphaned by deletes/overwrites that a compaction would free |
| `maxmemory` | `0` — no limit |
| `maxmemory_policy` | `noeviction` — Goblin never evicts |
| `mem_fragmentation_ratio` | **internal** fragmentation — see below |

`used_memory` is a real number, not the RSS: it comes from
[`GOBLIN.MEMORY`](goblin.md)-style accounting rolled up across every zset, hash,
list, and the keyspace. Because each arena maintains its live/dead byte counts
incrementally (a `+len` where bytes are written, a `−len` where a member is
orphaned), computing all of this is O(1) — nothing walks the data.

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
**is** the allocator for what matters. There is almost no jemalloc-style overhead
between `used_memory` and RSS — that leanness is the product's whole memory story,
and it means `used_memory_rss` sits just barely above `used_memory`. So Redis's
external ratio would be a near-constant ~`1.0` here: technically true, and useless.

The fragmentation that actually matters for Goblin lives **above** the allocator,
inside its own arenas. Those arenas are append-structured: a deleted or overwritten
member's bytes are *orphaned* — still mapped, no longer referenced — until a
compaction rewrites the arena. Those `mem_reclaimable_bytes` are the real,
actionable waste, so that is what the ratio reports:

```
mem_fragmentation_ratio = used_memory / (used_memory − mem_reclaimable_bytes)
```

- `1.00` — every allocated byte is live; nothing to reclaim.
- `1.50` — a third of the allocation is dead bytes a compaction would free.
- It never drops below `1.00` (dead bytes are never negative), so there is no
  "swapped out" reading to confuse it with.

The remedy differs, and deliberately: where Redis leans on the allocator's
probabilistic background defrag, Goblin gives you a **deterministic** one —
[`GOBLIN.OPTIMIZE key`](goblin.md) rewrites a key's arena tight, and arenas
auto-compact once dead bytes exceed live (past a floor). `mem_fragmentation_ratio`
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
