# The Lichess leaderboard benchmark — measuring the incumbents fairly

This benchmark drives every engine with a real, ZADD-heavy trace — a Lichess-style
chess leaderboard: players' ratings updated over and over, so it is dominated by
`ZADD` **re-scores** of existing members, with narrow chess-rating scores
(~1000–2000) packed into large multi-member `ZADD` commands. At full scale it is
~14.3 billion `ZADD`s over 21,598,007 distinct members. `benchmarks/feed_payload_uds.sh`
feeds the first *N* lines of that payload to each engine over a UNIX-domain socket
via `redis-cli` in normal (reply-per-command) mode, and records, per engine:

- feed wall-clock time (`redis-cli(s)`),
- the engine's **self-reported** resident memory, `used_memory_rss` from `INFO memory`,
- the **OS-measured** resident set size, `ps -o rss` on the server pid (`os_rss`).

The headline comparison is memory: goblin-core vs `redis-7.2.4`, `redis-8.8.0`,
`valkey-9.1.0`, and `dragonfly`. For that to be honest, every engine must report the
**same** RSS metric, sampled at the **same** instant the benchmark reads it.

## Why the redis cousins had to be patched

We found that Redis-family self-reporting used a cached RSS value derived from
`/proc/self/stat` field 24. Switching it to `/proc/self/statm` resident pages and
refreshing the value brought reported RSS into agreement with `ps` and with the
external benchmark harness.

goblin and dragonfly report a resident size that matches `ps` exactly. The redis
family (`redis-7.2.4`, `redis-8.8.0`, `valkey-9.1.0`) reported an `used_memory_rss`
**well below** what the OS said the process was actually using. Uncorrected, the
leaderboard would have credited redis/valkey with less memory than they really
consumed — an artificial advantage.

**The direction matters:** these fixes make the incumbents report *more* memory —
their true resident set — not less. They remove a handicap that flattered
redis/valkey, so the corrected comparison is if anything *tougher* on goblin, not
easier. The point was a fair measurement, not a favorable one.

Two independent bugs caused the under-reporting, and they compound. Both were fixed
in the **local benchmark builds only** (`~/bench/<engine>/` on the bench host); no
upstream/distributed binary is affected.

---

## Fix 1 — RSS was cached every ~100 ms, so it was stale after a fast feed

`INFO memory` emitted `used_memory_rss` from a **cached** field,
`server.cron_malloc_stats.process_rss`. That field is only refreshed by
`cronUpdateMemoryStats()`, which `serverCron()` calls inside a `run_with_period(100)`
block — i.e. roughly every 100 ms. A benchmark that feeds a burst of data and then
immediately reads `INFO` sees an RSS snapshot taken **before** the feed's allocations
landed. The faster the feed, the more stale the number.

**Fix (in `src/server.c`, at the top of the `# Memory` section of the INFO builder
`genRedisInfoString` / `genValkeyInfoString`):** re-sample RSS live on every call,
by writing the cached fields directly before `getMemoryOverheadData()` reads them:

```c
/* Refresh RSS live so INFO memory reports the current resident-set size
 * instead of the value serverCron caches only every ~100 ms. */
server.cron_malloc_stats.process_rss = zmalloc_get_rss();
server.cron_malloc_stats.zmalloc_used = zmalloc_used_memory();
```

`process_rss` and `zmalloc_used` are sampled **together** on purpose:
`getMemoryOverheadData()` derives `mem_fragmentation_ratio` (and
`mem_fragmentation_bytes`, `rss_overhead_*`) as a ratio of these two, so taking them
at the same instant keeps that ratio meaningful. (`redis-7.2.4` additionally
re-samples the allocator stats `allocator_allocated/active/resident`, which its older
`getMemoryOverheadData` also derives RSS fields from, so all of them stay consistent.)

**Why not just call `cronUpdateMemoryStats()` on demand?** Because its own RSS
sampling sits inside `run_with_period(100)`, gated on `server.cronloops` (which only
`serverCron` advances) and `server.hz`. At the default `hz=10` the gate happens to
fire every call, but if `hz` is raised — including automatically, when `dynamic-hz`
scales it up under client load, *exactly* the benchmark's condition — the on-demand
call becomes a no-op most of the time and would leave RSS stale. Writing the fields
directly is unconditional and `hz`-independent.

## Fix 2 — RSS was read from the wrong `/proc` field (a few % low, systematically)

Under Fix 1 the redis numbers were current but still didn't match `ps`. The reason
is the RSS source itself. `zmalloc_get_rss()` (in `src/zmalloc.c`, Linux path) read
the resident size from **`/proc/self/stat` field 24**. On the benchmark kernel that
field is *systematically* a few percent below the process's real resident set. The
standard RSS — what `ps -o rss`, `/proc/self/status` `VmRSS`, and `/proc/self/statm`
field 2 all report, and what goblin and dragonfly use — is a different, higher
number.

Reproduced back-to-back on the same live process (isolated small build):

```
stat[24] = 51,304 KB   ←  redis INFO used_memory_rss
statm[2] = 53,476 KB   ←  ps -o rss, goblin, dragonfly
VmRSS    = 53,476 KB   ←  /proc/self/status
```

`redis INFO` matched `stat[24]` exactly — the parser was correct; it was simply
reading the low field. That small build showed ~2 MB / ~4%; at full leaderboard
scale the gap is **larger** (the field difference grows with the working set, and it
stacks on top of any residual staleness from Fix 1), which is why the discrepancy in
the real `*.output` files was well over 2 MB.

**Fix (in `src/zmalloc.c`, `zmalloc_get_rss()`, Linux path only):** read
`/proc/self/statm` field 2 (`resident`, in pages) and multiply by the page size,
instead of `/proc/self/stat` field 24:

```c
/* Read RSS from /proc/self/statm field 2 (resident, in pages) rather than
 * /proc/self/stat field 24, which runs a few percent below the kernel's real
 * VmRSS. statm[2] matches /proc/self/status VmRSS and `ps -o rss` -- the
 * standard resident size the other engines report. */
if ((fd = open("/proc/self/statm", O_RDONLY)) == -1) return 0;
/* ... read the line "size resident shared text lib data dt" (all pages) ... */
p = strchr(buf, ' ');            /* skip field 1 (total size) */
rss = strtoll(p + 1, &x, 10);    /* field 2 = resident */
rss *= sysconf(_SC_PAGESIZE);
```

Only the Linux branch changed; the macOS (`task_info`) and *BSD (`sysctl`) paths
already return the standard resident size and were left alone. Because
`zmalloc_get_rss()` backs **both** `used_memory_rss` and the fragmentation ratio,
this single change aligns all of the RSS reporting at once.

---

## Combined effect

The two bugs stacked: a stale snapshot **of** the low field. With both fixed, every
engine's `used_memory_rss` is now (a) sampled live the instant `INFO` is called and
(b) the same `VmRSS` metric `ps` reports — so the `INFO used_memory_rss` column and
the `os_rss` column agree, and the cross-engine memory comparison is apples-to-apples.

## What was *not* changed

- **goblin-core** and **dragonfly** — unchanged. Both already report a live resident
  size (goblin's `INFO` reads `/proc/self/statm` field 2 on each call) that matches
  `ps`.
- No engine's *behavior* was altered — only *what number `INFO` reports*. No
  allocator, no data path, no config.

## Files changed (benchmark builds only)

| engine | `src/server.c` (Fix 1, live refresh) | `src/zmalloc.c` (Fix 2, statm[2]) |
|---|---|---|
| redis-7.2.4 | `genRedisInfoString` — refresh `process_rss` + `zmalloc_used` + allocator stats | `zmalloc_get_rss` → `statm[2]` |
| redis-8.8.0 | `genRedisInfoString` — refresh `process_rss` + `zmalloc_used` | `zmalloc_get_rss` → `statm[2]` |
| valkey-9.1.0 | `genValkeyInfoString` — refresh `process_rss` + `zmalloc_used` | `zmalloc_get_rss` → `statm[2]` |

## Provenance note

The edits to each incumbent were applied by **isolated sub-agents** that read only
that engine's own source tree and rebuilt it in place. This keeps the incumbents'
source — some under licenses distinct from this project's — separate from the
goblin-core codebase: the changes live in the benchmark harness's copies, not here.
