# Benchmarks

Goblin Core versus Redis 7.2.4, Redis 8.8, Valkey 9.1, and Dragonfly, with an
emphasis on memory. Every number below was measured under **allocator and
configuration parity** (see [Methodology](#methodology)) so the only variable is
the engine — and, for Goblin Core, the data structure.

| Engine | Version | Serving threads | Allocator |
| --- | --- | ---: | --- |
| Goblin Core | `0.4.2` | 1 | glibc + `malloc_trim` |
| Redis | `7.2.4` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Redis | `8.8.0` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Valkey | `9.1.0` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Dragonfly | `v1.39.4` (`bc82e529`) | 1 (`--proactor_threads=1`) | mimalloc `2.2.4` |

Each engine runs the allocator it ships with and is tuned for: the three
Redis-family engines on their bundled jemalloc 5.3.0 under a
[shared config](redis-parity.conf), Goblin Core on its native glibc +
`malloc_trim` (see [Allocators](#allocators)), Dragonfly on its bundled mimalloc.
Dragonfly is multi-threaded by design; it is run with a single proactor
(`--proactor_threads=1`) so it is measured on the same one-core budget as the
single-threaded engines, and it manages its own CPU affinity, so it is tested as
shipped. Host: see [Host](#host). Server and client on separate cores.

The one-core budget is deliberate parity, not an attempt to show
Dragonfly at its maximum scale. Goblin Core's intended deployment is a
modest-memory, single-core machine where compact structures can avoid
paying for a larger server. The quiet, dedicated benchmark host
available to the developer happens to be a 64-core Threadripper
monster workstation because it provides clean isolation: the server
and client get separate, otherwise idle cores, while the remaining
cores do not contribute to the measured server. The large test rig
makes the measurement quieter; it does not turn this into a 64-core
result.

License note: **Redis 7.2.4** is BSD-3-Clause (the last open-source Redis) and is
the format Goblin Core imports; **Redis 8.8** is RSALv2/SSPL (source-available);
**Valkey 9.1** is BSD-3-Clause (the community fork); **Dragonfly** is BSL 1.1
(source-available).

## Host

| | |
| --- | --- |
| CPU | AMD Ryzen Threadripper PRO **5995WX** (64 cores × 2 threads) |
| Logical CPUs | 128 |
| Max clock | ~4.6 GHz (boost) |
| OS | Ubuntu Linux (`7.0.0-22-generic`) |
| Toolchain | GCC **16.1.0** |

Quiet, dedicated box; benchmarks run with server and client pinned to separate
cores.

## Memory (the headline)

RSS per member after a load-then-`GOBLIN.OPTIMIZE` sequence, across a size sweep.
Goblin Core is flat at ~`51` bytes/member and holds a sorted set in
**roughly half** the resident set of legacy Redis, and under every modern engine.
The score generator is the scattered 32-bit integer shape used by
`benchmarks/zset_memory.py`; because roughly half the values exceed signed i32,
Goblin Core stores these sets at `f64` score width. Narrower `i16`/`i32` score
workloads are not required for the result below.

**RSS bytes/member:**

| members | Goblin (glibc) | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 250K | `52.9` | `103.8` | `79.2` | `85.0` | `56.8` |
| 500K | `52.0` | `107.7` | `82.8` | `84.9` | `55.5` |
| 1M | `51.4` | `109.7` | `82.7` | `84.5` | `55.1` |
| 2M | `51.2` | `106.5` | `80.4` | `84.4` | `54.8` |
| 4M | `51.0` | `103.2` | `78.2` | `84.3` | `54.6` |

**Goblin Core RSS as a fraction of each engine:** ~`47–49%` of Redis 7.2.4,
~`62–67%` of Redis 8.8, ~`60–62%` of Valkey 9.1, and ~`93–94%` of Dragonfly —
stable at every size. So Goblin Core is **roughly half** of legacy Redis,
~`35%` leaner than Redis 8.8 at 4M, and ~`39%` leaner than Valkey 9.1. Dragonfly
is the closest competitor: its dashtable holds a sorted set in ~`55` B/member —
well under the Redis family — but Goblin Core is still about `6–7%` lower RSS.

**Fragmentation (RSS / used_memory):** Goblin Core `1.0–1.4`, Redis/Valkey
`1.0–1.5`, both tightening toward `1.0` at scale. Dragonfly's ratio is inflated at
small N by mimalloc's pre-reserved arenas (~`2.7` at 250K) but amortizes to ~`1.1`
by 4M — which is why the per-member table above, measured as an RSS *delta* over
the empty-server baseline, is the fair read of its footprint. Goblin Core's
`used_memory` — its actual structure bytes — holds at ~`48.4` B/member; `malloc_trim`
after `OPTIMIZE` keeps the resident set within a hair of it.

The advantage is structural — a handful of big contiguous allocations (the
member-bytes arena, the Swiss index, the score-index blocks, the struct-of-arrays
offset/length/score columns) instead of a skiplist node + dict entry + string per
member. It does not depend on the allocator (see below).

## Allocators

A memory comparison is only fair if the allocator is held constant, because an
engine's footprint depends heavily on it. So the three Redis-family engines run
their bundled **jemalloc 5.3.0** (identical version), Goblin Core its default
**glibc + `malloc_trim`**, and Dragonfly its bundled **mimalloc 2.2.4** — each
engine on the allocator it ships with and is tuned for.

That tuning matters: `GOBLIN.OPTIMIZE` frees and reallocates the packed indexes
and hands the freed pages back to the OS with `malloc_trim`, so Goblin Core's RSS
(~`51` B/member) tracks the bytes its structures actually request (`used_memory`
~`48.4`). The footprint is Goblin Core's own, not the allocator's: `used_memory` is
allocator-independent, and it is the structural number behind Goblin Core's
roughly-half footprint.

## Throughput

Pipelined `redis-benchmark -P 16 -c 1` on the x86 host —
best of three runs, all Redis-family engines on jemalloc 5.3.0 under the shared
parity config, Goblin Core on its default config (rank cache off, so `ZRANK` is
O(log n) on both sides). The score field is `__rand_int__` with `-r 1000000`,
so this throughput sweep exercises Goblin Core's signed `i32` score width.
Goblin Core leads every sorted-set operation:

| operation | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| ZADD (write) | `392K` | `235K` | `232K` | `235K` | `251K` |
| ZSCORE (point read) | `581K` | `480K` | `470K` | `485K` | `406K` |
| ZRANK | `476K` | `332K` | `340K` | `353K` | `320K` |
| ZRANGE (16 elems) | `433K` | `360K` | `365K` | `339K` | `257K` |
| ZSCORE depth-1 latency (µs) | `21.3` | `21.9` | `23.0` | `22.4` | `22.9` |

The standout is `ZADD` at **~`+56–69%`** over the field (`392K` vs `232–251K`) —
the immediate per-reply `write()` plus the packed Swiss member index make inserts
cheap — and `ZRANK` at **`+35–49%`**. Point reads (`ZSCORE`) lead by `+20–43%` and
16-element `ZRANGE` by `+19–68%`. Depth-1 latency is a near-tie at ~`21–23` µs
(redis-benchmark's own round-trip floor).

At one core Dragonfly is the fastest of the rest on `ZADD` (`251K`, above the Redis
family's ~`232–235K`) but the slowest on reads — `ZSCORE` `406K` and `ZRANGE` `257K`
are the lowest here. That is the expected profile of an engine built to shard
across many proactor threads, measured on one.

## Latency

Goblin Core writes each reply the instant it is ready; Redis and Valkey defer
replies to the event-loop boundary. That saves one event-loop hop per reply — a
small, consistent edge, with no penalty at high fan-out.

**Depth-1 round trip** — a lean C client (`write_tail_latency --ping`), one
connection, no pipelining, server and client pinned to separate cores, median of
1M PINGs:

| | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| PING p50 (µs) | `15.8` | `16.7` | `19.3` | `19.0` | `18.2` |
| PING p99 (µs) | `19.9` | `21.1` | `25.4` | `25.2` | `23.3` |

Goblin Core is fastest — ~`1` µs over Redis 7.2.4 and ~`3` µs (≈`15%`) over the
modern engines — the single deferred event-loop hop its immediate `write()`
avoids. Dragonfly sits in the middle (`18.2` µs), ahead of Redis 8.8/Valkey but
behind Goblin Core and legacy Redis. A localhost round trip is syscall-floor
dominated, so this is a modest edge, not a rout — but a consistent one.

**PING throughput / p50 by client count** (`redis-benchmark -P 1`; the absolute
p50 here carries redis-benchmark's own client overhead, so read it for *scaling*,
not the floor above):

| clients | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | `50K` / `23µs` | `48K` / `23µs` | `41K` / `23µs` | `40K` / `23µs` | `44K` / `23µs` |
| 50 | `103K` / `255µs` | `99K` / `263µs` | `91K` / `287µs` | `86K` / `295µs` | `89K` / `295µs` |
| 500 | `99K` / `2.57ms` | `91K` / `2.75ms` | `82K` / `3.05ms` | `85K` / `2.94ms` | `86K` / `2.90ms` |

Goblin Core leads at every concurrency and widens at 500 saturating clients
(`99K` ops / `2.57` ms versus the field's `82–91K` / `2.75–3.05` ms) — no overload
penalty. Dragonfly and the modern Redis engines land within a few percent of each
other at fan-out; the immediate-write design keeps Goblin Core ahead throughout.

## Scripting: compare-and-delete

The most-copied Redis script is the [Redlock](https://redis.io/docs/latest/develop/use/patterns/distributed-locks/)
lock release — delete a key only if it still holds your token — which Goblin Core
also ships as the native [`GOBLIN.CAD`](docs/commands/GOBLIN.CAD.md) and as the
idiom in each of its six [embedded interpreters](docs/commands/README.md). Every
scripted form is `SCRIPT LOAD`-compiled once and run by `EVALSHA`, so this
measures execution of the precompiled script, not compilation.

`benchmarks/cad_benchmark.py`, one connection, median of 9 rounds — four numbers
per row: **sequential** (one request at a time — the per-op round trip) and
**pipelined** (256 in flight — round trip amortized), each over **TCP** loopback
and a **UDS** (Unix domain socket). Every row is Goblin Core's own implementation,
so this is a *within-engine* comparison, not the cross-engine parity setup above —
and a *relative* per-op measurement: one Python connection is client-bound, so
read across a row, not absolute throughput (for that use a C load generator as in
[Throughput](#throughput)). Every implementation is verified to agree (a matching
token deletes and replies `1`, a mismatch replies `0`) before timing. µs/op,
lower is better.

| implementation | RTT | TCP seq | UDS seq | TCP pipe (256) | UDS pipe (256) |
| --- | ---: | ---: | ---: | ---: | ---: |
| **`GOBLIN.CAD`** (native C++) | 1 | `19.7` | `9.9` | **`1.5`** | **`1.4`** |
| `WREN.EVAL` (Wren) | 1 | `23.2` | `10.8` | `2.5` | `2.5` |
| `TCL.EVAL` (Jim Tcl) | 1 | `22.9` | `10.6` | `2.6` | `2.6` |
| `UPYTHON.EVAL` (MicroPython) | 1 | `23.1` | `10.9` | `2.8` | `2.7` |
| `LUAU.EVAL` (Luau) | 1 | `22.7` | `10.8` | `2.7` | `2.8` |
| `QUICKJS.EVAL` (JavaScript) | 1 | `22.6` | `11.7` | `3.2` | `3.1` |
| `EVAL` (PUC-Lua 5.1) | 1 | `22.6` | `11.7` | `3.2` | `3.2` |
| `GET` + `DEL` (client-side, racy) | 2 | `40.1` | `20.3` | — | — |

Three things fall out. **The Unix socket roughly halves sequential latency** —
~`10` µs/op vs ~`20` over TCP loopback — by removing the loopback round-trip
overhead, so a co-located lock client should prefer it. **Pipelined, the transport
stops mattering** (TCP ≈ UDS): with 256 in flight the round trip is amortized and
server-side cost dominates. **The naive `GET`+`DEL` is ~2× any atomic form on both
transports** (`40`/`20` µs TCP, `20`/`10` µs UDS) — its second round trip — and it
is also *racy*, since another client can change the key between the read and the
delete, the very bug the atomic forms exist to avoid. Across all four columns the
native **`GOBLIN.CAD` is the cheapest**: ~2× under the interpreters pipelined
(`1.4` µs vs `2.5–3.2`), and ahead even one op at a time. Precompilation keeps the
scripted forms this close — without it each `EVALSHA` would re-parse and re-compile
the script, ~`960` µs per call for MicroPython alone.

The full language comparison — this table plus a heavier leaderboard-rescore
workload, where the six interpreters separate by ~6× — is in
**[BENCHMARK-LANGUAGES.md](BENCHMARK-LANGUAGES.md)**.

## Scripting: native idioms vs Lua

Compare-and-delete is one of a family of Redis idioms that Goblin Core ships as
native `GOBLIN.*` commands — each one exists precisely because Redis's own answer
is a Lua script. The native form calls the store's C++ primitives directly (no
interpreter, no re-entry into the command processor); the Lua form runs the same
logic through the embedded PUC-Lua 5.1 VM via `redis.call`. This table pits each
native command against the exact Lua idiom it replaces (verbatim from the
[command docs](docs/commands/goblin.md)), with the script `SCRIPT LOAD`-compiled
once so the Lua numbers are `EVALSHA` execution, not compilation.

`benchmarks/idiom_native_vs_lua.py`, one connection, median of 5 rounds, all over
a **Unix domain socket**. Two numbers per form: **sequential** (one request at a
time — the per-op round trip) and **pipelined** (256 in flight — round trip
amortized, so server-side cost dominates). Native and Lua are verified to return
the same reply for the same input before timing. The **`×` columns are the
native/Lua throughput ratio** — the host-independent result; the absolute µs/op
are from the local development machine (macOS, one client-bound connection), so
read the ratios, not the absolute throughput. Lower µs is better.

| command | idiom it replaces (`redis.call`s) | native seq | Lua seq | seq **×** | native p256 | Lua p256 | pipe **×** |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [`GOBLIN.CAD`](docs/commands/GOBLIN.CAD.md) | `get` → `del` | `10.7` | `12.4` | `1.16×` | `1.45` | `3.23` | **`2.22×`** |
| [`GOBLIN.CAEXPIRE`](docs/commands/GOBLIN.CAEXPIRE.md) | `get` → `pexpire` | `18.5` | `23.0` | `1.25×` | `8.47` | `11.8` | `1.39×` |
| [`GOBLIN.CAS`](docs/commands/GOBLIN.CAS.md) | `get` → `set KEEPTTL` | `11.7` | `16.7` | `1.43×` | `3.18` | `7.04` | **`2.21×`** |
| [`GOBLIN.ZWINDOW`](docs/commands/GOBLIN.ZWINDOW.md) | `zremrangebyscore` → `zcard` → `zadd` → `expire` | `18.2` | `29.3` | **`1.61×`** | `8.63` | `17.9` | **`2.07×`** |
| [`GOBLIN.INCRBOUND`](docs/commands/GOBLIN.INCRBOUND.md) | `get` → `incrby` | `13.1` | `20.0` | `1.52×` | `4.69` | `9.94` | **`2.12×`** |
| [`GOBLIN.DECRPOS`](docs/commands/GOBLIN.DECRPOS.md) | `get` → `decr` | `12.7` | `19.2` | `1.52×` | `4.16` | `9.16` | **`2.20×`** |
| [`GOBLIN.HCAD`](docs/commands/GOBLIN.HCAD.md) | `hget` → `hdel` | `19.0` | `25.3` | `1.33×` | `9.08` | `14.1` | `1.55×` |
| [`GOBLIN.HSETGT`](docs/commands/GOBLIN.HSETGT.md) | `hget` → `hset` | `13.5` | `20.3` | `1.50×` | `4.67` | `9.97` | **`2.14×`** |
| [`GOBLIN.CLAIM`](docs/commands/GOBLIN.CLAIM.md) | `set NX EX` → `get` | `31.3` | `38.0` | `1.21×` | `19.4` | `26.7` | `1.38×` |

**The native command wins on every idiom, and the gap widens when pipelined** —
where the round trip is amortized and pure server-side cost is left. On the
compare-then-write string ops (`CAD`, `CAS`, `INCRBOUND`, `DECRPOS`, `HSETGT`) the
native form does ~`2.1–2.2×` the throughput of the precompiled Lua: it fuses the
read, the compare, and the conditional write into one op — the compare is
allocation-free (an in-place `view_equals`), and the write reuses the slot the read
already found — where the script pays VM dispatch and a `redis.call` boundary per
step. `ZWINDOW` shows the biggest sequential win (`1.61×`): the Lua makes **four**
`redis.call`s, while the native command holds the sorted set across evict → count →
add in a **single keyspace lookup**. The two commands that lean on an existing TTL
walk (`CAEXPIRE`, `CLAIM`) narrow to ~`1.4×` pipelined — the expiry bookkeeping is
the same work either way, so less of the per-op cost is interpreter overhead the
native form can shed.

## Persistence

Same 1M-member dataset loaded into each engine; each saves and loads its own
format. "load" is startup-to-ready minus the empty-start baseline.

| engine | save | file | load |
| --- | ---: | ---: | ---: |
| **Goblin Core** | `0.243s` | `37.0` MB | **`0.150s`** |
| Redis 7.2.4 | `0.358s` | `24.8` MB | `0.326s` |
| Redis 8.8 | `0.345s` | `24.8` MB | `0.377s` |
| Valkey 9.1 | `0.236s` | `24.8` MB | `0.348s` |
| Dragonfly | `0.258s` | `24.8` MB | `0.304s` |

Goblin Core **loads ~2–2.5× faster** than the field — its default snapshot dumps
the packed indexes (the "accelerator") and `memcpy`s them back rather than
rebuilding; Dragonfly loads next-fastest (`0.304s`). Goblin Core's save is also a
**non-blocking background `fork()`** — the command returns immediately and the
server keeps serving while the child writes the file; the others' `SAVE` blocks.
The cost is a **~1.5× larger file** (the accelerator) — Redis, Valkey, and
Dragonfly all land at ~`24.8` MB (Dragonfly's LZ4 `.dfs` matches here only because
these members are unique and incompressible); `GOBLIN.SAVE <path> NOACCEL` drops
Goblin's file to near that size at the cost of a rebuild-on-load. Goblin Core also
imports a Redis RDB (`--load dump.rdb`, Redis 2.6–7.2.x) for migration.

## Write-Path Tail Latency

The memory win comes from a Swiss-table member index that grows by reindexing: an
amortized O(1) insert with an occasional synchronous O(n) rehash. That rehash is
a pause, visible in the extreme tail of write latency. 1M individually-timed
`ZADD`s growing one set (same C client, server/client pinned), microseconds:

| ZADD (µs) | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Goblin Core | `16.6` | `21.3` | `23.7` | `31,810` |
| Redis 7.2.4 | `20.6` | `28.3` | `43.4` | `4,186` |
| Redis 8.8 | `21.3` | `29.2` | `42.7` | `748` |
| Valkey 9.1 | `20.7` | `29.1` | `41.8` | `543` |
| Dragonfly | `20.9` | `30.4` | `46.8` | `51,954` |

Goblin Core is faster through p99.9 — ahead at p50, p99, and p99.9 by a clear
margin — but its tail max is the Swiss-table rehash: the largest write (the final
~1M-member reindex) is ~`32` ms. Dragonfly's tail is fatter still (~`52` ms max),
from its own background fiber scheduling; the Redis family, by contrast, holds a
sub-millisecond-to-few-ms max. So Goblin Core wins the body of the distribution
and pays for it only at the extreme tail — rare (beyond p99.99) and the price of
the packed layout; `GOBLIN.OPTIMIZE` performs the reindex on demand, moving it out
of the serving path.

## Hashes

Goblin Core also implements a Redis `HASH` (field→value), built on the same tuned
Swiss table and packed arena as the sorted set — no scores, no ordering, so it is
leaner still (an 8-byte struct-of-arrays reference per field instead of 14). The
memory comparison uses the same methodology as [Memory](#memory-the-headline)
above: N field→value pairs loaded into one hash with `HSET`, then
`GOBLIN.OPTIMIZE`; all Redis-family engines on jemalloc 5.3.0 under the
[shared parity config](redis-parity.conf), which pins
`hash-max-listpack-entries`/`-value` so every hash past 128 entries uses the
hashtable encoding (the fair comparison). Fields and values are 16 bytes each.

**RSS bytes/field:**

| fields | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 250K | `46.1` | `85.9` | `55.6` | `64.7` | `78.3` |
| 500K | `45.6` | `87.8` | `59.3` | `64.6` | `77.1` |
| 1M | `45.4` | `87.8` | `59.2` | `64.2` | `76.8` |
| 2M | `45.3` | `85.1` | `56.9` | `64.0` | `76.5` |
| 4M | `45.2` | `82.4` | `55.7` | `63.9` | `76.4` |

Goblin Core is flat at ~`45` bytes/field and leanest at every size: ~`53%` of
Redis 7.2.4 (**roughly half**), ~`21%` leaner than Redis 8.8, ~`29%` leaner than
Valkey 9.1, ~`41%` leaner than Dragonfly. Engine-reported `used_memory`
corroborates RSS (Goblin `45.2`, Redis 7.2.4 `80.6`, Redis 8.8 `54.3`, Valkey
`67.1`, Dragonfly `76.1` at 4M), so the advantage is structural, not an allocator
artifact.

Note the reversal from sorted sets: Dragonfly, the leanest of the rest on zsets,
is the **second-heaviest** on hashes (~`76` B/field, above Redis 8.8 and Valkey) —
its dashtable is tuned for the sorted-set case and its hash overhead does not
amortize the same way. The margin over *modern* Redis is smaller than the sorted
set's ~40%, and honestly so: a hashtable-encoded Redis hash carries no skiplist
and no scores, so it is already leaner than a Redis sorted set. Goblin Core still
wins because its per-field overhead — the 8-byte reference
(`offset u32 + field_len u16 + value_len u16`) plus a Swiss slot, over one packed
field+value arena — is about half Redis 8.8's (a dict entry plus two `sds`
strings). The ratio tracks field/value size: with the 32-byte content here the
fixed overhead is ~40% of the total, so smaller values widen the lead and larger
ones narrow it (see [Hash value-size sensitivity](#hash-value-size-sensitivity)).

**Throughput and depth-1 latency** — redis-benchmark, one pipelined connection at
depth 16 (throughput) and depth 1 (latency), big hash ~1M fields, HGETALL over a
20-field object, best of three runs on the x86 host:

| metric | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| HSET ops/s | `563K` | `480K` | `454K` | `451K` | `357K` |
| HGET ops/s | `591K` | `501K` | `480K` | `498K` | `393K` |
| HGETALL ops/s | `276K` | `212K` | `213K` | `172K` | `171K` |
| HGET depth-1 latency (µs) | `21.1` | `22.4` | `23.0` | `22.7` | `22.6` |

Goblin Core leads every hash throughput metric — `+17–58%` on HSET, `+18–50%` on
HGET, `+30–61%` on HGETALL. Dragonfly is the slowest of the field on hashes at one
core (`357K` HSET, `393K` HGET), consistent with its heavier hash representation.
Depth-1 HGET latency is a near-tie at ~`21–23` µs: at pipeline depth 1
redis-benchmark's own round-trip overhead dominates, compressing the
immediate-write edge that the leaner PING probe surfaces (see [Latency](#latency)).

### Hash value-size sensitivity

The memory ratio tracks field/value size, because Goblin Core's per-field
overhead is *constant* while Redis's is larger and grows. RSS bytes/field at 1M
fields, 16-byte field, varying the value:

| value | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8 B | `37.4` | `80.0` | `55.6` | `56.9` | `60.8` |
| 16 B | `45.4` | `87.8` | `59.2` | `64.2` | `76.8` |
| 64 B | `93.4` | `144.1` | `119.2` | `121.1` | `124.9` |

Subtract the content (field + value) and Goblin Core's overhead is a flat
~`13.4` bytes/field at every value size — the 8-byte reference plus an amortized
Swiss slot — versus Redis 8.8's ~`27–38` and Dragonfly's ~`37–45` (both grow with
the value: a dict/dashtable entry, string headers, and allocator rounding). So the
lead is widest on small values (~`32%` leaner than Redis 8.8, ~`38%` than Dragonfly
at 8 B) and narrows as the shared content dominates (~`22%` and ~`25%` at 64 B).
Goblin Core stays ahead of every engine at every size.

## Command parsing

Command dispatch is a gperf perfect hash: the name is upper-cased into a 16-byte
buffer and looked up in O(1), replacing the former 25-way linear `equals_ci` chain
where a late command (`GOBLIN.LOAD`) paid for every earlier comparison.
`parse_command` is now flat regardless of which command arrives — ~`23–28` ns/op
(`goblin_core_microbench --category resp_parse`, `dispatch`), independent of the
command's former position in the chain.

## Methodology

**Parity — the point of this whole document.**

- **Allocator:** the three Redis-family engines built with their bundled
  jemalloc 5.3.0 (`make MALLOC=jemalloc`, identical version), Goblin Core on its
  native glibc + `malloc_trim`, Dragonfly on its bundled mimalloc 2.2.4 — each on
  the allocator it ships with and is tuned for (see [Allocators](#allocators)).
  Both `used_memory` and RSS are reported, plus the fragmentation ratio.
- **Config:** the three Redis-family engines load the same
  [`benchmarks/redis-parity.conf`](redis-parity.conf) — persistence off,
  `maxmemory-policy noeviction`, `activedefrag no`, `io-threads 1` (one serving
  thread), and the listpack/intset thresholds pinned to shared defaults. Dragonfly
  is multi-threaded by design and is run with `--proactor_threads=1` — a single
  serving shard, the one-core comparison — snapshotting off, eviction disabled; it
  manages its own CPU affinity, so it is tested as shipped (no external pinning).
  Differing configs are the most common way a benchmark gets contested; the file
  is published here.
- **Host:** AMD Ryzen Threadripper PRO 5995WX (128 logical CPUs,
  GCC 16.1.0); every figure in this document — except the
  [native-idioms-vs-Lua](#scripting-native-idioms-vs-lua) table, whose absolute
  µs/op are from the local dev machine (macOS) and whose native/Lua **ratios** are
  the host-independent result. Server and client pinned to
  separate cores;
  throughput/latency are best-or-median of repeated runs on the idle machine.
- **RSS** via `ps -o rss`, measured after load and `GOBLIN.OPTIMIZE`;
  `used_memory` via `INFO memory` (Redis/Valkey) and `GOBLIN.MEMORY` (Goblin
  Core).
- **Cross-engine RSS parity.** The RSS above is the external `ps -o rss`
  (`VmRSS`) for every engine; the engines' *own* self-reporting is aligned to it
  too. Redis normally reads its own RSS from field 24 of `/proc/self/stat`, a
  value the Linux `proc(5)` documentation flags as inaccurate and redirects
  readers away from in favor of `/proc/self/statm`. For these benchmarks the
  Redis and Valkey builds were patched to read field 2 of `/proc/self/statm`
  instead: the same resident-memory counter that `ps` prints and that
  `/proc/<pid>/status` reports as `VmRSS`, matching Dragonfly and Goblin Core so
  every engine reports the same kernel figure.

**Reproduce** — every driver takes repeatable `--engine LABEL:KIND:PATH`, where
KIND is `goblin` | `redis` | `dragonfly` (`redis` covers both Redis and Valkey,
which share the server flags):

- Sorted-set / hash memory + value-size: `benchmarks/zset_memory.py` and
  `benchmarks/hash_benchmark.py --engine ... --sizes N,... [--value-bytes B]`.
- Sorted-set / hash throughput + depth-1 latency: `benchmarks/zset_speed.py` and
  `benchmarks/hash_speed.py --engine ... --redis-benchmark <path>`.
- PING latency, concurrency, and write-path tail: `benchmarks/lat_tail.py
  --engine ... --probe ./write_tail_latency --redis-benchmark <path>` (build the
  probe with `c++ -O2 -std=c++20 -o write_tail_latency
  benchmarks/write_tail_latency.cpp`).
- Persistence: `benchmarks/persistence.py --engine ... --members N`.
- Native `GOBLIN.*` idioms vs their Lua scripts (one Goblin server, over UDS):
  `benchmarks/idiom_native_vs_lua.py --goblin-bin build/goblin-core [--keys N
  --rounds R --pipeline D]`.

Example engine set: `--engine goblin:goblin:build-release/goblin-core --engine
redis724:redis:<redis-7.2.4>/src/redis-server --engine
redis88:redis:<redis-8.8>/src/redis-server --engine
valkey:redis:<valkey>/src/valkey-server --engine
dragonfly:dragonfly:$(command -v dragonfly)`.
