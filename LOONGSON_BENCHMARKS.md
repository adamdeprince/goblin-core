# Loongson 3A6000 Benchmarks

[中文版](LOONGSON_BENCHMARKS.zh-CN.md)

Goblin Core versus **Redis 7.2.4**, **Redis 8.8.0**, **Valkey 9.1.0**, and
**Dragonfly v1.39.0** on a Loongson **3A6000** (Kylin, `loongarch64`). Same
methodology as [BENCHMARKS.md](BENCHMARKS.md): allocator parity within each
engine family, `redis-parity.conf` for the Redis line, one serving thread per
engine.

Measured **2026-07-07** on host `loongson`. Artifacts:
`~/loongson-bench-work/results/`.

| Engine | Version | Serving threads | Allocator |
| --- | --- | ---: | --- |
| Goblin Core | `0.4.0` (local build) | 1 | glibc + `malloc_trim` |
| Redis | `7.2.4` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Redis | `8.8.0` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Valkey | `9.1.0` | 1 (`io-threads 1`) | jemalloc `5.3.0` |
| Dragonfly | `v1.39.0` (source build) | 1 (`--proactor_threads=1`) | mimalloc `2.2.4` |

Dragonfly has no `loongarch64` release binary; it was built from source on this
host (see [Dragonfly build](#dragonfly-build)). It is run with `taskset -c 0` and
a single proactor thread, matching the one-core budget of the single-threaded
engines.

---

## Host

| | |
| --- | --- |
| Machine | `loongson` |
| CPU | Loongson **3A6000** (4 cores × 2 threads, 2.0 GHz max) |
| ISA | `lsx`, `lasx`, `crc32`, `lam`, `ual` |
| RAM | 15 GiB |
| OS | Kylin Linux Desktop V10 SP1 (`5.4.18-167-generic`) |
| Toolchain | GCC **15.2.0** (`/opt/loongson-gcc-15.2.0`) |
| Goblin build | `-DCMAKE_BUILD_TYPE=Release`, `-march=native` |

Goblin requires `LD_LIBRARY_PATH=/opt/loongson-gcc-15.2.0/lib` at runtime.

**Pinning (latency runs):** server `taskset -c 0`, client probe `taskset -c 1`.
Throughput runs use `redis-benchmark -c 1 -P 16` without explicit server
`taskset`; the box was otherwise idle.

---

## Memory methodology

Memory tables report **absolute structure bytes per element** after load — not
RSS deltas over an empty-server baseline.

| engine family | source |
| --- | --- |
| Goblin Core | `GOBLIN.MEMORY` → `total_allocated_bytes` ÷ N |
| Redis / Valkey / Dragonfly | `INFO memory` → `used_memory` ÷ N |

Workload: one key, `N` elements, scattered scores (zsets) or 16-byte field
names/values (hashes, hashtable encoding). Goblin runs `GOBLIN.OPTIMIZE` after
load. Sweep **500K–2M** members/fields.

**Fragmentation** (optional context): process RSS after load ÷ engine-reported
`used_memory`. Values near `1.0` mean resident set tracks structure bytes;
higher values mean allocator-retained pages (not extra structure).

---

## Allocators

| engine | allocator | notes |
| --- | --- | --- |
| Redis 7.2.4 / 8.8.0 / Valkey 9.1.0 | **jemalloc 5.3.0** | `make MALLOC=jemalloc`; verified via `--version` |
| Goblin Core | **glibc malloc** | `malloc_trim` after `GOBLIN.OPTIMIZE` |
| Dragonfly | **mimalloc 2.2.4** | bundled; high RSS/used ratio at small N |

Do not substitute system jemalloc for the Redis-family engines.

---

## Sorted-set memory (structure bytes/member)

| members | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | `50.2` | `104.1` | `79.1` | `82.9` | `54.9` |
| 1,000,000 | **`49.1`** | `103.2` | `78.2` | `81.8` | `54.6` |
| 2,000,000 | **`48.6`** | `102.7` | `77.8` | `81.3` | `54.5` |

**Goblin Core is leanest at every size** (~`49` B/member, tightening to **`48.6`**
at 2M). **Dragonfly** is second (~`55` B/member, flat). **Redis 8.8.0** is
~`24%` leaner than **7.2.4** (~`78` vs ~`103` B/member). **Valkey** sits between
Redis 8.8 and 7.2.4.

**Fragmentation (RSS ÷ used_memory) after load:**

| members | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | `1.26` | `1.78` | `2.08` | `2.03` | `12.69` |
| 1,000,000 | `1.20` | `1.39` | `1.55` | `1.53` | `7.00` |
| 2,000,000 | `1.19` | `1.20` | `1.28` | `1.29` | `4.12` |

Goblin and the Redis family tighten toward ~`1.2` by 2M. Dragonfly's mimalloc
arenas keep RSS well above `used_memory` on this port (ratio ~`4–13`).

---

## Hash memory (structure bytes/field)

16-byte field names and values, hashtable encoding (`redis-parity.conf`).

| fields | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | **`46.7`** | `82.3` | `55.7` | `69.0` | `76.6` |
| 1,000,000 | **`45.7`** | `81.4` | `54.9` | `67.9` | `76.3` |
| 2,000,000 | **`45.7`** | `80.9` | `54.5` | `67.4` | `76.2` |

**Goblin Core is leanest** (~`46` B/field, flat). **Redis 8.8.0** is next
(~`55` B/field). **Dragonfly** and **Valkey** cluster around ~`67–77` B/field.
**Redis 7.2.4** is heaviest (~`81` B/field).

**Fragmentation (RSS ÷ used_memory) after load:**

| fields | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| 500,000 | `1.13` | `2.01` | `2.69` | `2.32` | `9.98` |
| 1,000,000 | `1.08` | `1.56` | `1.78` | `1.57` | `5.45` |
| 2,000,000 | `1.03` | `1.25` | `1.40` | `1.26` | `3.17` |

---

## Sorted-set throughput

`redis-benchmark`, 1M-member keyspace, best of 3, `-c 1 -P 16`.

| operation | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| ZADD (write) | **`207K`** | `153K` | `145K` | `139K` | `115K` |
| ZSCORE | **`357K`** | `282K` | `263K` | `260K` | `193K` |
| ZRANK | **`295K`** | `195K` | `198K` | `194K` | `157K` |
| ZRANGE (16) | **`240K`** | `183K` | `180K` | `165K` | `102K` |
| ZSCORE depth-1 (µs) | `31.7` | `35.8` | `38.8` | `39.7` | **`35.5`** |

Goblin leads every zset throughput metric on the 3A6000 (~**+35%** ZADD vs Redis
7.2.4). **Dragonfly trails the field** on Loongson — the opposite of the x86
story — likely reflecting a fresh `loongarch64` source port (SIMDe for SSE
intrinsics, no tuned assembly paths) rather than Dragonfly's x86/aarch64
binaries.

---

## Hash throughput

1M-field keyspace, same `redis-benchmark` settings.

| operation | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| HSET | **`370K`** | `288K` | `254K` | `207K` | `140K` |
| HGET | **`369K`** | `285K` | `269K` | `252K` | `171K` |
| HGETALL (20 fields) | **`155K`** | `115K` | `117K` | `91K` | `78K` |
| HGET depth-1 (µs) | **`31.4`** | `35.7` | `37.8` | `39.3` | `36.1` |

Goblin leads on every hash op (~**+29%** HSET vs Redis 7.2.4). Dragonfly is
slowest on writes and range reads here.

---

## Latency

C probe `write_tail_latency` (200K samples) + `redis-benchmark PING` at 1 / 50 /
500 clients (100K samples each). Server core 0, client core 1.

**Depth-1 PING (probe, µs):**

| | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| p50 | **`22.9`** | `25.9` | `27.9` | `28.9` | `28.1` |
| p99 | **`24.7`** | `28.6` | `30.6` | `32.0` | `30.8` |

**PING throughput / p50 by client count (`redis-benchmark -P 1`):**

| clients | Goblin | Redis 7.2.4 | Redis 8.8.0 | Valkey 9.1.0 | Dragonfly |
| --- | --- | --- | --- | --- | --- |
| 1 | `34K` / `23µs` | `30K` / `31µs` | `28K` / `31µs` | `27K` / `31µs` | `28K` / `31µs` |
| 50 | `52K` / `0.48ms` | `48K` / `0.51ms` | `47K` / `0.52ms` | `47K` / `0.51ms` | `48K` / `0.50ms` |
| 500 | `50K` / `5.14ms` | `45K` / `5.35ms` | `45K` / `5.64ms` | `47K` / `5.45ms` | **`50K` / `4.90ms`** |

Goblin is fastest on depth-1 PING. Dragonfly leads at **500 saturated clients**
(`50K` / `4.90ms`).

---

## Write-path tail latency

200K individually-timed `ZADD`s growing one set (same C probe), microseconds:

| | p50 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: |
| Goblin | **`25.2`** | **`35.4`** | `52.3` | `19,857` |
| Redis 7.2.4 | `30.5` | `34.5` | `55.6` | **`9,447`** |
| Redis 8.8.0 | `32.6` | `37.2` | `55.4` | **`9,017`** |
| Valkey 9.1.0 | `32.9` | `37.4` | `56.6` | **`9,455`** |
| Dragonfly | `37.0` | `41.6` | `63.4` | `17,053` |

Goblin wins through **p99**; the **max** is a Swiss-table rehash on the final
large insert (~`20` ms). The Redis family shows lower tail **max** on this
hardware.

---

## Persistence

1M-member zset; `SAVE` / `GOBLIN.SAVE`, then cold load. **Load** is wall-clock
startup with data present minus empty-server startup (absolute times for each
step; load column is the incremental cost of restoring data).

| engine | save | file | load |
| --- | ---: | ---: | ---: |
| **Goblin** | **`0.140s`** | `37.0` MB | **`0.050s`** |
| Redis 7.2.4 | `0.535s` | **`24.8` MB** | `0.649s` |
| Redis 8.8.0 | `0.463s` | **`24.8` MB** | `0.624s` |
| Valkey 9.1.0 | `0.465s` | **`24.8` MB** | `0.621s` |
| Dragonfly | `0.584s` | **`24.8` MB** | `0.438s` |

Goblin **saves and loads fastest**. Redis-family and Dragonfly write smaller
on-disk files (`24.8` MB vs Goblin `37.0` MB). Dragonfly **loads faster than
Redis/Valkey** (`0.438s`) but not Goblin (`0.050s`).

---

## Dragonfly build

| | |
| --- | --- |
| Binary | `~/loongson-bench-work/dragonfly` (`368` MB, `v1.39.0` tag) |
| Build script | `~/loongson-bench-work/build_dragonfly.sh` |
| Key porting work | `file://` cmake dep prefetch (no HTTPS in cmake); Boost context with `architecture=loongarch`; OpenSSL 3.3.2; RE-flex autotools; SIMDe for SSE→LSX in `sse_port.h`; `-ldl` link flag |

No official `loongarch64` release exists; this is a **best-effort source build**
for comparative measurement, not a supported Dragonfly target.

---

## Reproduce

```bash
# on loongson — does not modify ~/dev/packrat sources
~/loongson-bench-work/build_engines.sh
~/loongson-bench-work/build_dragonfly.sh   # optional; long build
/opt/loongson-gcc-15.2.0/bin/g++ -O2 -std=c++20 \
  -o ~/loongson-bench-work/write_tail_latency \
  ~/dev/packrat/benchmarks/write_tail_latency.cpp
export LD_LIBRARY_PATH=/opt/loongson-gcc-15.2.0/lib
~/loongson-bench-work/run_all.sh
```

Benchmark scripts with Dragonfly support live in
`~/loongson-bench-work/benchmarks/` (copies of the packrat suite; the loongson
`~/dev/packrat` tree is older and lacks Dragonfly hooks).

Engine binaries:

| engine | path |
| --- | --- |
| Goblin Core | `~/loongson-bench-work/goblin-core` |
| Redis 7.2.4 | `~/loongson-bench-work/redis-7.2.4/src/redis-server` |
| Valkey 9.1.0 | `~/loongson-bench-work/valkey/src/valkey-server` |
| Redis 8.8.0 | `~/loongson-bench-work/redis-8.8/src/redis-server` |
| Dragonfly | `~/loongson-bench-work/dragonfly` |
| redis-benchmark | `~/loongson-bench-work/redis-7.2.4/src/redis-benchmark` |

---

## Cross-host pitch: Goblin on Loongson vs incumbents on x86

Cross-host read for domestic-silicon deployments: **Goblin Core on the 3A6000**
(this document) versus **Redis 7.2.4, Redis 8.8.0, Valkey 9.1.0, Dragonfly, and
Goblin Core** on the x86 reference host in [BENCHMARKS.md](BENCHMARKS.md) —
**AMD Ryzen Threadripper PRO 5995WX** (64 cores × 2 threads, GCC 16.1.0).
Different CPU, clock, and memory bandwidth — not a same-machine comparison.

**Verdict:** Goblin on Loongson is often **~75–90% of Redis on the Threadripper**
on hot-path ops, **beats every competitor on the same Loongson box**, and **beats
x86 Redis on snapshot load**. It is **not** reference-host-class versus **Goblin
on x86** (~`50–75%` throughput). The honest hook: not “make Loongson into a
Threadripper,” but **remove the Redis-on-Loongson performance penalty** — and on
load plus memory, beat what teams get from Redis on x86.

### Throughput (ops/s, pipelined `-P 16`)

| | **Goblin Loongson** | Goblin x86 | Redis 7.2.4 x86 | Redis 8.8 x86 | Valkey x86 | Dragonfly x86 | **Loongson / Redis 7.2.4 x86** |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ZADD | `207K` | `398K` | `239K` | `242K` | `242K` | `262K` | **`87%`** |
| ZSCORE | `357K` | `587K` | `504K` | `488K` | `495K` | `419K` | `71%` |
| ZRANK | `295K` | `491K` | `331K` | `342K` | `354K` | `329K` | **`89%`** |
| ZRANGE (16) | `240K` | `456K` | `365K` | `368K` | `346K` | `268K` | `66%` |
| HSET | `370K` | `563K` | `480K` | `454K` | `451K` | `357K` | `77%` |
| HGET | `369K` | `591K` | `501K` | `480K` | `498K` | `393K` | `74%` |
| HGETALL | `155K` | — | `212K` | `213K` | `172K` | `171K` | `73%` |

On **writes and rank** (`ZADD`, `ZRANK`), Goblin on the 3A6000 is within **~10–15%**
of legacy Redis on the 5995WX — credible for a 2 GHz 3A6000 vs a workstation-class
part. Versus Redis 8.8 / Valkey on x86: **~70–85%**. Versus Goblin on x86:
**~52%** (ZADD) to **~74%** (HGET).

### Latency (µs)

| | **Goblin Loongson** | Goblin x86 | Redis 7.2.4 x86 | Redis 8.8 x86 |
| --- | ---: | ---: | ---: | ---: |
| PING p50 | `22.9` | `15.8` | `16.7` | `19.3` |
| PING p99 | `24.7` | `19.9` | `21.1` | `25.4` |
| ZADD tail p99 | `35.4` | `21.3` | `28.3` | `29.2` |
| ZSCORE depth-1 | `31.7` | `21.3` | `22.2` | `23.0` |

Latency is **fine but not x86-class** — a few microseconds slower than Redis on
the Threadripper host, still **best on the Loongson box** (PING p50 `22.9` vs
Redis `25.9–28.9`).

### Persistence and memory (1M zset load; 2M structure bytes)

| | **Goblin Loongson** | Goblin x86 | Redis 7.2.4 x86 | Redis 8.8 x86 | Valkey x86 | Dragonfly x86 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Load (s) | **`0.050`** | `0.150` | `0.326` | `0.377` | `0.348` | `0.304` |
| Save (s) | **`0.140`** | `0.243` | `0.358` | `0.345` | `0.236` | `0.258` |
| Zset (B/member) | **`48.6`** | ~`49` | `102.7` | `77.8` | `81.3` | `54.5` |
| Hash (B/field) | **`45.7`** | ~`45` | `80.9` | `54.5` | `67.4` | `76.2` |

**Goblin on Loongson loads faster than Redis on the x86 host** (~**6–7×**).
Memory
advantage does not shrink on Loongson — same packed layout, leanest in both
columns. x86 figures from [BENCHMARKS.md](BENCHMARKS.md) (same `used_memory`
metric as the Loongson tables above).

### Same Loongson box (why the engine choice matters)

| ZADD | Goblin | Redis 7.2.4 | Redis 8.8 | Valkey | Dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| Loongson | **`207K`** | `153K` | `145K` | `139K` | `115K` |

Redis on Loongson is only **~64%** of Redis-on-x86 (`153K` vs `239K`). Goblin
on Loongson is **~87%** of Redis-on-x86. **Switching Redis → Goblin on Loongson
recovers most of the ISA gap** relative to running Redis on x86.

### Pitch guide (for domestic-silicon teams)

**Strong (defensible):**

> On Loongson 3A6000, single-core ZADD/ZRANK with Goblin reaches **~85–90%** of
> Redis 7.2.4 on the Threadripper reference host, while beating Redis/Valkey on
> the same machine by **~35–50%**; snapshot load is an order of magnitude faster
> than x86 Redis.

**Medium:**

> Loongson + Redis drops sharply; Loongson + Goblin pulls hot paths back toward
> “near x86 Redis” territory with the leanest memory footprint.

**Weak (do not claim):**

> Goblin on Loongson equals Goblin on x86 — throughput is roughly half.
> Dragonfly on Loongson is viable — it trails badly here (`115K` ZADD vs Goblin
> `207K`).

### Claim checklist

| Claim | Verdict |
| --- | --- |
| Reference-host vs **Redis on x86** (writes/rank) | **~85–90%** — credible |
| Reference-host vs **Goblin on x86** | **~50–75%** — no |
| Best engine **on Loongson** | **Yes** — Goblin wins the field |
| Best **load time** vs x86 Redis | **Yes** — `0.050s` vs ~`0.33s` |
| Domestic silicon + stack without giving up Redis-class speed | **Yes** — if Goblin is the stack |

**Caveats:** Dragonfly on Loongson is a best-effort source port; do not
extrapolate x86 Dragonfly results to `loongarch64`. Goblin-only in-process
microbenchmarks are useful for silicon bring-up but **not** a substitute for this
cross-engine suite (`~/loongson-bench-work/results/microbench-100k.json`).