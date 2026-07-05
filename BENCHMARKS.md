# Goblin Core Benchmarks

Generated: 2026-07-02 15:42:47 UTC.

These results compare Goblin Core against Redis for the current sorted-set-focused implementation. Goblin Core's optional rank-location cache and score-string cache are off by default.

## Headline: Memory Footprint

Goblin Core's reason to exist is memory. After a load-then-`GOBLIN.OPTIMIZE` sequence (the deployment path), it stores a sorted set in about `49` RSS bytes per member versus Redis at about `130` — roughly `37%` of Redis's resident memory — and the ratio holds flat as the set grows, and even at member counts just past a power of two (Intel Xeon 6975P-C, Ubuntu 26.04, Redis `8.0.5`):

| Members | Goblin Core RSS B/member | Redis RSS B/member | Goblin Core / Redis | Goblin Core RSS saved |
| ---: | ---: | ---: | ---: | ---: |
| 250K | `49.5` | `131.0` | `37.8%` | `19` MiB |
| 1M | `49.1` | `133.2` | `36.9%` | `80` MiB |
| 1.08M (just past 2^20) | `49.1` | `147.8` | `33.2%` | `102` MiB |
| 4M | `48.8` | `128.8` | `37.9%` | `305` MiB |

Goblin Core's tracked zset allocation (`~49` B/member via `GOBLIN.MEMORY`) is within ~2% of its RSS delta, so almost none of the footprint is allocator slack. The member index holds `5.2` B/member at every size, including the 1.08M count just past `2^20` — a power-of-two-sized table would have been ~9.7 there. The benchmark runs `GOBLIN.OPTIMIZE` at density `0.97` after loading; Redis has no equivalent compaction step.

## Headline: Throughput (secondary)

Throughput is secondary to the memory story. Single-member read throughput must be measured with a C load generator: one Python pipelined connection is client-bound near `~350K` ops/sec, so the Python-driven tables further down understate both servers and previously reported `ZSCORE` at a misleading `0.95x` (the client, not the server). Measured with `redis-benchmark` against a loaded 1M-member set after `GOBLIN.OPTIMIZE` (the deployment path), pinned server/client, `-c 1 -P 256`, median of 3 rounds (absolute throughput is host-dependent; the ratio is the stable takeaway):

| Operation | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZSCORE` | `2,083,467` | `1,602,667` | `1.30x` |
| `ZRANK` | `1,362,485` | `671,635` | `2.03x` |
| `ZREVRANK` | `1,354,183` | `675,035` | `2.01x` |
| `ZADD` (score update) | `840,743` | `376,388` | `2.23x` |
| `ZRANGE` (16) | `1,004,080` | `686,621` | `1.46x` |
| `ZRANGE WITHSCORES` (16) | `607,757` | `455,506` | `1.33x` |

The RESP throughput tables in the sections below come from the Python harness. `benchmarks/zset_benchmark.py` now drives `ZSCORE`/`ZRANK`/`ZREVRANK` through `redis-benchmark`, but the batched/range throughput rows remain client-bound; treat the sections below primarily as memory/RSS sources and cross-check throughput against the table above.

## Persistence

Snapshot save and load are several times faster than Redis, and `GOBLIN.SAVE ... NOACCEL` trades file size for load speed. Identical data loaded into Redis 7.2.4, `SAVE`d, then imported into Goblin Core so the datasets match exactly; avx10, pinned server, min of 3 runs. "load" is startup-to-ready minus the empty-start baseline. The "save" figure is the serialization work: in production Goblin Core runs it as a background copy-on-write `fork()` (`GOBLIN.SAVE` returns immediately and the server keeps serving), so it never pauses the server — this table times that work synchronously to compare like-for-like with Redis `SAVE`.

9.5M members:

| | save | load | file | bytes/member |
| --- | ---: | ---: | ---: | ---: |
| Redis 7.2.4 (RDB) | `3.89s` | `3.98s` | `266` MB | `28` |
| Goblin Core, full (default) | `0.78s` | `0.69s` | `388` MB | `41` |
| Goblin Core, `NOACCEL` | `0.55s` | `3.01s` | `294` MB | `31` |

At 9.5M, Goblin Core's default snapshot saves `5.0x` faster and loads `5.7x` faster than Redis; `NOACCEL` saves `7.1x` faster and loads `1.3x` faster. In steady-state throughput, Redis moves about `67` MB/s on both save and load (CPU-bound on RDB encode/decode plus skiplist/dict construction), while Goblin Core's default runs at roughly `500` MB/s save and `560` MB/s load — it dumps and `memcpy`s the packed structures with a hardware CRC32C rather than re-encoding and rebuilding. `NOACCEL` saves at about `530` MB/s but its load (`98` MB/s) rebuilds every index.

The default snapshot dumps the packed indexes (the "accelerator") for a larger file that loads fastest, and it is the everyday restart path. `GOBLIN.SAVE <path> NOACCEL` is the upgrade/migration path, not a routine-restart option: it drops the accelerator for a smaller, portable, canonical-only file whose load rebuilds every index (slower, though still faster than Redis). Reach for it when moving snapshots across Goblin versions, architectures, or C++ standard libraries — cases where a loader would discard the accelerator and rebuild the indexes anyway, so the smaller file costs nothing extra.

Scaling note: a smaller ~950K-member run measured much faster per item (default save `0.06s`, load `0.05s`) only because its ~30 MB files were cache-resident. At ~300 MB the 9.5M numbers above reflect real disk and cache behavior, so they are the representative rates. Extrapolating linearly from 9.5M (both already past cache), 100M members would be roughly:

| | save | load | file |
| --- | ---: | ---: | ---: |
| Redis 7.2.4 (RDB) | `~41s` | `~42s` | `~2.8` GB |
| Goblin Core, full | `~8s` | `~7s` | `~4.1` GB |
| Goblin Core, `NOACCEL` | `~6s` | `~32s` | `~3.1` GB |

## Methodology

The benchmark starts each server on a temporary localhost port, drives both over RESP, and records throughput plus process RSS. Redis also reports `INFO memory used_memory`; Goblin Core reports internal zset allocation and the active `rank_cache_mode` through `GOBLIN.MEMORY`.

Host:

- macOS-26.5.1-arm64-arm-64bit-Mach-O arm64
- Python 3.14.5
- Release build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`

Workload:

- loaded members: `1,000,000`
- read/range ops: `1,000,000`
- removed members: `500,000`
- `ZADD` batch size: `128`
- `ZREM` batch size: `128`
- pipeline depth: `256`
- latency samples: `10,000`
- latency warmup per metric: `100`
- latency pipeline depth: `1`
- `ZRANGE` size: `16`
- score shape: `integer`
- Goblin score-string cache: `False`
- seed: `12345`

## Linux Results

Deployment-oriented run: `Ubuntu 26.04, Intel Xeon 6975P-C, Redis 8.0.5, GCC 16.1.0` — the primary benchmark context for Linux users. The current-host section below is a local development baseline.

Headline: Goblin Core is `1.28x` geomean throughput vs Redis and `41.5%` of Redis process RSS.

Source data:

- `off`: `benchmark-results/linux-1m-rank-cache-off.json`
- `exact`: `benchmark-results/linux-1m-rank-cache-exact.json`
- `block-hint`: `benchmark-results/linux-1m-rank-cache-block-hint.json`

Linux default configuration: rank cache mode `off`; score-string cache `False`.

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 634,830 | 400,361 | 1.59x |
| `ZSCORE ops` | 321,705 | 339,410 | 0.95x |
| `ZRANK ops` | 366,421 | 238,608 | 1.54x |
| `ZREVRANK ops` | 344,737 | 238,413 | 1.45x |
| `ZRANGE ops` | 64,477 | 52,426 | 1.23x |
| `ZRANGE WITHSCORES ops` | 34,836 | 29,738 | 1.17x |
| `ZREVRANGE ops` | 63,992 | 52,981 | 1.21x |
| `ZREVRANGE WITHSCORES ops` | 34,369 | 30,230 | 1.14x |
| `ZREM members` | 717,455 | 499,278 | 1.44x |

| Memory metric | Goblin Core | Redis |
| --- | ---: | ---: |
| RSS delta | 52.70 MiB | 126.97 MiB |
| RSS delta per loaded member | 55.26 B | 133.14 B |
| Redis `used_memory` | n/a | 120.59 MiB |
| Redis `used_memory` per member | n/a | 126.45 B |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 36.14 us | 42.87 us | 48.94 us | 94.25 us |
| `ZSCORE` | redis | 36.44 us | 43.47 us | 49.69 us | 173.85 us |
| `ZRANK` | goblin | 35.80 us | 43.03 us | 48.57 us | 59.25 us |
| `ZRANK` | redis | 37.21 us | 44.04 us | 50.82 us | 197.18 us |
| `ZREVRANK` | goblin | 35.79 us | 43.27 us | 48.56 us | 116.85 us |
| `ZREVRANK` | redis | 13.41 us | 38.27 us | 45.59 us | 163.10 us |
| `ZRANGE` | goblin | 48.17 us | 57.67 us | 61.25 us | 217.56 us |
| `ZRANGE` | redis | 50.90 us | 58.68 us | 64.52 us | 196.57 us |
| `ZREVRANGE` | goblin | 48.52 us | 57.73 us | 61.37 us | 136.46 us |
| `ZREVRANGE` | redis | 51.35 us | 60.09 us | 65.20 us | 205.52 us |

Linux rank-cache mode comparison:

| Metric | off ops/sec | exact ops/sec | block-hint ops/sec | exact vs off | block-hint vs off |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZADD members` | 634,830 | 476,097 | 606,674 | -25.0% | -4.4% |
| `ZSCORE ops` | 321,705 | 349,050 | 310,886 | 8.5% | -3.4% |
| `ZRANK ops` | 366,421 | 374,024 | 378,015 | 2.1% | 3.2% |
| `ZREVRANK ops` | 344,737 | 341,469 | 377,237 | -0.9% | 9.4% |
| `ZRANGE ops` | 64,477 | 64,177 | 66,919 | -0.5% | 3.8% |
| `ZRANGE WITHSCORES ops` | 34,836 | 34,347 | 35,280 | -1.4% | 1.3% |
| `ZREVRANGE ops` | 63,992 | 63,767 | 65,207 | -0.4% | 1.9% |
| `ZREVRANGE WITHSCORES ops` | 34,369 | 34,432 | 35,052 | 0.2% | 2.0% |
| `ZREM members` | 717,455 | 513,964 | 699,522 | -28.4% | -2.5% |

Linux rank-cache allocation by mode:

| Mode | Rank cache MiB | Rank cache B/member | RSS delta B/member |
| --- | ---: | ---: | ---: |
| `off` | 0.00 | 0.00 | 55.26 |
| `exact` | 4.02 | 4.21 | 59.28 |
| `block-hint` | 2.02 | 2.11 | 57.27 |

## Current Host Default Configuration

Goblin Core default run: rank cache mode `off`; score-string cache `False`.

Source data: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-off.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 945,957 | 547,592 | 1.73x |
| `ZSCORE ops` | 434,185 | 393,774 | 1.10x |
| `ZRANK ops` | 461,684 | 319,609 | 1.44x |
| `ZREVRANK ops` | 450,633 | 319,731 | 1.41x |
| `ZRANGE ops` | 77,416 | 66,855 | 1.16x |
| `ZRANGE WITHSCORES ops` | 41,310 | 39,089 | 1.06x |
| `ZREVRANGE ops` | 77,599 | 69,103 | 1.12x |
| `ZREVRANGE WITHSCORES ops` | 40,826 | 39,474 | 1.03x |
| `ZREM members` | 1,068,245 | 775,862 | 1.38x |

| Memory metric | Goblin Core | Redis |
| --- | ---: | ---: |
| RSS delta | 64.25 MiB | 88.36 MiB |
| RSS delta per loaded member | 67.37 B | 92.65 B |
| Redis `used_memory` | n/a | 80.46 MiB |
| Redis `used_memory` per member | n/a | 84.37 B |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.96 us | 41.96 us | 53.83 us | 308.75 us |
| `ZSCORE` | redis | 21.75 us | 45.37 us | 52.54 us | 128.62 us |
| `ZRANK` | goblin | 20.63 us | 26.12 us | 30.00 us | 99.92 us |
| `ZRANK` | redis | 22.17 us | 28.71 us | 38.29 us | 151.83 us |
| `ZREVRANK` | goblin | 20.83 us | 26.25 us | 30.71 us | 114.42 us |
| `ZREVRANK` | redis | 22.13 us | 27.75 us | 33.08 us | 146.33 us |
| `ZRANGE` | goblin | 36.50 us | 40.04 us | 44.12 us | 68.17 us |
| `ZRANGE` | redis | 38.50 us | 45.00 us | 52.71 us | 133.96 us |
| `ZREVRANGE` | goblin | 36.71 us | 40.83 us | 46.29 us | 98.96 us |
| `ZREVRANGE` | redis | 37.54 us | 43.79 us | 51.75 us | 170.58 us |

Goblin Core tracked zset allocation:

| Component | Allocated |
| --- | ---: |
| member storage | 33.00 MiB |
| member index | 5.00 MiB |
| score index | 16.00 MiB |
| score-string cache | 0.00 MiB |
| rank-location cache | 0.00 MiB |
| total tracked zset allocation | 54.00 MiB |

RSS includes allocator retained pages, executable/runtime mappings, and other process overhead, so it is expected to exceed tracked zset allocation.

## Rank Cache Modes

`--rank-cache-mode exact` enables the packed member-id-to-score-location cache. `--rank-cache-mode block-hint` stores only member-to-score-block hints, reducing write maintenance while retaining a smaller `ZRANK` read assist. Block hints start as 16-bit ids and promote to 32-bit ids automatically if the block-id space grows beyond the narrow encoding.

Source data:

- `off`: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-off.json`
- `exact`: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-exact.json`
- `block-hint`: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-block-hint.json`

| Metric | off ops/sec | exact ops/sec | block-hint ops/sec | exact vs off | block-hint vs off |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZADD members` | 945,957 | 773,415 | 899,025 | -18.2% | -5.0% |
| `ZSCORE ops` | 434,185 | 407,329 | 410,526 | -6.2% | -5.4% |
| `ZRANK ops` | 461,684 | 452,984 | 441,761 | -1.9% | -4.3% |
| `ZREVRANK ops` | 450,633 | 451,006 | 445,880 | 0.1% | -1.1% |
| `ZRANGE ops` | 77,416 | 74,476 | 76,213 | -3.8% | -1.6% |
| `ZRANGE WITHSCORES ops` | 41,310 | 40,078 | 40,769 | -3.0% | -1.3% |
| `ZREVRANGE ops` | 77,599 | 76,017 | 76,422 | -2.0% | -1.5% |
| `ZREVRANGE WITHSCORES ops` | 40,826 | 40,086 | 40,665 | -1.8% | -0.4% |
| `ZREM members` | 1,068,245 | 916,526 | 1,121,514 | -14.2% | 5.0% |

Rank-cache allocation by mode:

| Mode | Rank cache MiB | Rank cache B/member | RSS delta B/member |
| --- | ---: | ---: | ---: |
| `off` | 0.00 | 0.00 | 67.37 |
| `exact` | 4.02 | 4.21 | 71.27 |
| `block-hint` | 2.02 | 2.11 | 69.75 |

Exact mode is the read-heavy `ZRANK` option; block-hint mode is the churn-heavy leaderboard option.

### Rank Cache Mode: `exact`

Source data: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-exact.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 773,415 | 554,779 | 1.39x |
| `ZSCORE ops` | 407,329 | 381,214 | 1.07x |
| `ZRANK ops` | 452,984 | 308,260 | 1.47x |
| `ZREVRANK ops` | 451,006 | 306,567 | 1.47x |
| `ZRANGE ops` | 74,476 | 65,954 | 1.13x |
| `ZRANGE WITHSCORES ops` | 40,078 | 38,307 | 1.05x |
| `ZREVRANGE ops` | 76,017 | 67,762 | 1.12x |
| `ZREVRANGE WITHSCORES ops` | 40,086 | 39,324 | 1.02x |
| `ZREM members` | 916,526 | 722,142 | 1.27x |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.96 us | 33.08 us | 51.92 us | 135.29 us |
| `ZSCORE` | redis | 22.38 us | 35.54 us | 51.75 us | 139.21 us |
| `ZRANK` | goblin | 19.67 us | 25.12 us | 28.33 us | 152.96 us |
| `ZRANK` | redis | 22.13 us | 26.63 us | 30.08 us | 47.83 us |
| `ZREVRANK` | goblin | 19.63 us | 25.08 us | 29.04 us | 1,631.46 us |
| `ZREVRANK` | redis | 22.63 us | 28.50 us | 34.92 us | 122.92 us |
| `ZRANGE` | goblin | 36.75 us | 42.04 us | 50.46 us | 1,209.63 us |
| `ZRANGE` | redis | 38.37 us | 44.29 us | 49.63 us | 83.13 us |
| `ZREVRANGE` | goblin | 37.00 us | 41.58 us | 46.63 us | 99.92 us |
| `ZREVRANGE` | redis | 37.75 us | 42.83 us | 47.96 us | 183.75 us |

### Rank Cache Mode: `block-hint`

Source data: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-block-hint.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 899,025 | 555,691 | 1.62x |
| `ZSCORE ops` | 410,526 | 388,168 | 1.06x |
| `ZRANK ops` | 441,761 | 316,754 | 1.39x |
| `ZREVRANK ops` | 445,880 | 321,261 | 1.39x |
| `ZRANGE ops` | 76,213 | 66,101 | 1.15x |
| `ZRANGE WITHSCORES ops` | 40,769 | 38,289 | 1.06x |
| `ZREVRANGE ops` | 76,422 | 67,376 | 1.13x |
| `ZREVRANGE WITHSCORES ops` | 40,665 | 39,251 | 1.04x |
| `ZREM members` | 1,121,514 | 791,968 | 1.42x |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.79 us | 32.33 us | 52.92 us | 97.67 us |
| `ZSCORE` | redis | 21.71 us | 30.54 us | 54.00 us | 165.88 us |
| `ZRANK` | goblin | 19.92 us | 26.04 us | 29.33 us | 74.58 us |
| `ZRANK` | redis | 22.50 us | 27.92 us | 32.79 us | 125.25 us |
| `ZREVRANK` | goblin | 20.63 us | 26.33 us | 30.33 us | 95.58 us |
| `ZREVRANK` | redis | 22.58 us | 28.25 us | 34.33 us | 136.46 us |
| `ZRANGE` | goblin | 36.83 us | 41.62 us | 47.58 us | 248.00 us |
| `ZRANGE` | redis | 38.25 us | 43.83 us | 49.58 us | 106.67 us |
| `ZREVRANGE` | goblin | 36.63 us | 41.62 us | 47.71 us | 146.50 us |
| `ZREVRANGE` | redis | 37.75 us | 43.29 us | 48.83 us | 169.96 us |

## Post-Delete Reads

This run loads members, removes a configured fraction, then measures reads against the remaining members.

Source data: `benchmark-results/post-delete-read-1m.json`

Detailed report: `benchmark-results/post-delete-read-1m.md`

- loaded members: `1,000,000`
- removed members: `500,000`
- remove order: `load-prefix`
- range size: `16`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZREM members` | 1,308,228 | 863,019 | 1.52x |
| `ZSCORE ops` | 453,566 | 409,226 | 1.11x |
| `ZRANK ops` | 487,218 | 376,037 | 1.30x |
| `ZREVRANK ops` | 484,630 | 373,533 | 1.30x |
| `ZRANGE ops` | 81,040 | 71,432 | 1.13x |
| `ZRANGE WITHSCORES ops` | 42,706 | 40,824 | 1.05x |
| `ZREVRANGE ops` | 79,624 | 72,846 | 1.09x |
| `ZREVRANGE WITHSCORES ops` | 42,233 | 41,242 | 1.02x |

Goblin Core member-index tombstones after removal: `0`.

## Mixed Leaderboard Workload

This run preloads one zset and runs an interleaved leaderboard workload with reads, top ranges, score updates, and remove/add churn.

Source data: `benchmark-results/mixed-leaderboard-1m-r100.json`

Detailed report: `benchmark-results/mixed-leaderboard-1m-r100.md`

- loaded members: `1,000,000`
- logical mixed operations: `1,000,000`
- range size: `100`

| Metric | Goblin Core | Redis | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `Load members/sec` | 955,003 | 612,824 | 1.56x |
| `Mixed logical ops/sec` | 43,749 | 42,338 | 1.03x |
| `Mixed RESP commands/sec` | 45,928 | 44,447 | 1.03x |

Mixed workload latency:

| Target | p50 | p95 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| `goblin` | 24.58 us | 153.29 us | 167.92 us | 246.46 us |
| `redis` | 24.67 us | 152.71 us | 173.29 us | 1,664.50 us |

Goblin Core member-index tombstones after the mixed run: `9,890`.

## Cold Range Output

This no-warmup range-output sweep isolates first-burst response-buffer growth after geometric range-output reserve. Goblin Core uses the default `0` KiB initial output buffer.

Source data: `benchmark-results/range-output-geometric-reserve-r4-r16.json`

Detailed report: `benchmark-results/range-output-geometric-reserve-r4-r16.md`

- range commands per metric: `10,000`
- warmup commands per metric: `0`
- pipeline depth: `2048`
- read delay per burst: `2.0` ms
- Goblin initial output buffer: `0` KiB

| Range size | Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| ---: | --- | ---: | ---: | ---: |
| 4 | `ZRANGE` | 184,355 | 174,814 | 1.05x |
| 4 | `ZREVRANGE` | 208,709 | 200,989 | 1.04x |
| 4 | `ZRANGE WITHSCORES` | 141,410 | 143,062 | 0.99x |
| 4 | `ZREVRANGE WITHSCORES` | 141,951 | 140,625 | 1.01x |
| 16 | `ZRANGE` | 87,421 | 86,825 | 1.01x |
| 16 | `ZREVRANGE` | 93,628 | 94,054 | 1.00x |
| 16 | `ZRANGE WITHSCORES` | 52,699 | 53,172 | 0.99x |
| 16 | `ZREVRANGE WITHSCORES` | 53,322 | 52,269 | 1.02x |

## Interpretation

For the default configuration, Goblin Core uses about 72.7% of Redis process RSS on this workload.

The rank cache is intentionally a command-line choice, not the default policy. Enable it only after measuring a workload where `ZRANK` latency dominates write throughput.


## Running These Benchmarks

Redis is required for comparison runs. The top-level benchmark workflow configures a release build, runs the test suite, starts Goblin Core and Redis on temporary localhost ports, drives both over RESP, and writes JSON artifacts plus this Markdown report.

Full benchmark report:

```sh
python3 benchmarks/run_benchmarks.py \
  --redis-server /path/to/redis-server \
  --report BENCHMARKS.md \
  --name redis-goblin-core-1m-modes \
  --latency-samples 10000
```

`--latency-samples` enables per-command latency rows for `ZSCORE`, `ZRANK`, `ZREVRANK`, `ZRANGE`, and `ZREVRANGE`. Latency sampling defaults to single-command round trips with `--latency-pipeline-depth 1` so percentiles are not hidden by the throughput pipeline.

CI-style smoke benchmark:

```sh
scripts/benchmark_smoke.sh
```

The smoke script writes `benchmark-results/ci-smoke.md`, regenerates the static HTML docs, and accepts environment overrides such as `BENCHMARK_MEMBERS`, `BENCHMARK_OPS`, `BENCHMARK_LATENCY_SAMPLES`, `REDIS_SERVER`, and `SKIP_BUILD=1`.

Quick Goblin-only validation:

```sh
python3 benchmarks/run_benchmarks.py \
  --target goblin \
  --members 10000 \
  --ops 10000 \
  --remove-members 5000 \
  --latency-samples 1000 \
  --rank-cache-modes off
```

Targeted server harness:

```sh
python3 benchmarks/zset_benchmark.py \
  --target goblin \
  --goblin-rank-cache-mode off
```

Use `--goblin-rank-cache-mode off|exact|block-hint` to benchmark a specific rank-cache mode. Add `--goblin-score-string-cache` to measure the optional cached score-output path. Add `--score-shape integer|short-decimal|long-decimal|random-double` to vary score serialization behavior.

In-process microbenchmark:

```sh
cmake --build build-release --target goblin_core_microbench
./build-release/goblin_core_microbench \
  --members 1000000 \
  --ops 1000000 \
  --score-shape integer \
  --format json \
  --output benchmark-results/microbench.json
```

Regenerate the microbenchmark summary after running the rank-cache modes:

```sh
python3 benchmarks/report_microbench.py --output MICROBENCHMARKS.md
```

Supplemental benchmark scripts:

```sh
python3 benchmarks/range_output_benchmark.py \
  --output-json benchmark-results/range-output.json \
  --report benchmark-results/range-output.md

python3 benchmarks/range_size_sweep.py \
  --score-shape integer \
  --range-sizes 1 4 16 64 256 1024 \
  --output-json benchmark-results/range-size-sweep.json \
  --report benchmark-results/range-size-sweep.md

python3 benchmarks/range_output_sweep.py \
  --score-shape integer \
  --range-sizes 16 64 256 1024 \
  --warmup-ops 4096 \
  --output-json benchmark-results/range-output-sweep.json \
  --report benchmark-results/range-output-sweep.md

python3 benchmarks/update_scaling_sweep.py \
  --member-counts 10000 100000 1000000 \
  --remove-fraction 0.5 \
  --output-json benchmark-results/update-scaling-sweep.json \
  --report benchmark-results/update-scaling-sweep.md

python3 benchmarks/zrem_shape_sweep.py \
  --member-counts 50000 100000 200000 \
  --remove-fractions 0.01 0.1 0.5 0.9 \
  --remove-orders load-prefix load-suffix reshuffled \
  --output-json benchmark-results/zrem-shape-sweep.json \
  --report benchmark-results/zrem-shape-sweep.md

python3 benchmarks/post_delete_read_benchmark.py \
  --members 1000000 \
  --ops 1000000 \
  --remove-fraction 0.5 \
  --latency-samples 10000 \
  --output-json benchmark-results/post-delete-read.json \
  --report benchmark-results/post-delete-read.md

python3 benchmarks/mixed_leaderboard_benchmark.py \
  --members 1000000 \
  --ops 1000000 \
  --range-size 100 \
  --latency-samples 10000 \
  --output-json benchmark-results/mixed-leaderboard.json \
  --report benchmark-results/mixed-leaderboard.md
```