# INFO

```
INFO [section]
```

Returns a bulk string of server and memory statistics in the standard Redis
`# Section` / `field:value` (CRLF-separated) format, so `redis-cli`, monitoring
agents, and benchmark harnesses read it unchanged. Goblin Core emits a deliberately
small subset: `# Server`, `# Replication`, `# Kafka`, and `# Memory`. The optional
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

## `# Replication`

| field | meaning |
|---|---|
| `role` | `master`, or `slave` when an upstream firehose is configured |
| `master_replid` | the 128-bit replication lineage ID, rendered as 32 hexadecimal digits |
| `master_repl_offset` | highest logical mutation offset represented by this server |
| `kafka_acknowledged_offset` | last broker offset acknowledged by Kafka and eligible for inclusive snapshot recovery; `-1` until one is known |
| `goblin_ready` | `1` for a primary or live replica; `0` while a replica is connecting, replaying, or degraded |
| `master_link_status` | replica upstream link: `up` only after a complete handoff, otherwise `down` |
| `master_sync_in_progress` | `1` while Kafka replay or buffered-firehose handoff is active |
| `replica_state` | `connecting`, `reconnecting`, `replaying_kafka`, `buffering_firehose`, `live`, or `degraded` |
| `slave_repl_offset` | replica's locally applied logical offset |
| `upstream_repl_offset` | latest offset observed in an upstream hello or firehose batch |
| `replica_lag` | known upstream-minus-local logical offset; `-1` when disconnected before a newer upstream offset is known |
| `replica_reconnect_attempts` | connection attempts made after startup lost or could not establish the source |
| `replica_successful_reconnects` | reconnects that completed a safe handoff and returned to live state |
| `master_last_io_seconds_ago` | seconds since a successful upstream handshake or frame; `-1` before either occurs |
| `replica_last_error` | most recent connection, lineage, replay, buffering, or apply failure; empty while live |

The logical and Kafka offsets are different coordinate systems. See [Firehose
replication and Kafka recovery](../replication.md) before using them in restart
automation. The replica-only fields are omitted on a primary except for
`goblin_ready`, which is always present.

## `# Kafka`

| field | meaning |
|---|---|
| `kafka_journal_enabled` | `1` when the server is producing its write journal to Kafka; otherwise `0` |
| `kafka_ack_mode` | `queued`, `broker`, or `off`; broker mode withholds successful client replies and firehose delivery until Kafka acknowledges the corresponding write |
| `kafka_acknowledged_logical_offset` | highest Goblin replication offset whose Kafka record has been acknowledged by the broker |
| `kafka_pending_records` | produced records still awaiting a broker delivery result |
| `kafka_pending_bytes` | command and key bytes retained for those pending records |
| `kafka_oldest_pending_age_ms` | age of the oldest unacknowledged record |
| `kafka_retained_batch_bytes` | mutation payload retained until a broker acknowledgement makes its firehose batch releasable |
| `kafka_input_backpressured` | `1` while retained Kafka payload has reached `--kafka-pending-bytes` and command input is paused |

The pending-byte ceiling bounds journal payload retained by the server, not
librdkafka's own queue. A single atomic command, transaction, or script may cross
the ceiling; Goblin then stops reading additional commands until acknowledgements
release enough memory. See [Kafka write log and recovery](../kafka.md) for the
acknowledgement contract and its retry implications.

## `# Memory`

| field | meaning |
|---|---|
| `used_memory` | committed data-store capacity: arenas, fixed index/table structures, object state, TTL tables, and actual upstream blob-pool capacity |
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
| `maxmemory` | configured `--maxmemory` byte ceiling; `0` means unlimited |
| `maxmemory_policy` | `noeviction` — capacity-growing writes return `OOM` instead of evicting |
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
- [Hard memory ceiling](../maxmemory.md) — `--maxmemory`, accounting scope, and
  deterministic RESP/SBE OOM replies.
- [Goblin extension commands](goblin.md), and the memory
  [benchmarks](../../BENCHMARKS.md) (RSS is the headline there).
