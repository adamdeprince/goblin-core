# Goblin Core Benchmarks

Generated: 2026-07-01 21:18:27 UTC.

These results compare Goblin Core against Redis for the current sorted-set-focused implementation. Goblin Core's optional rank-location cache and score-string cache are off by default.

## Methodology

The benchmark starts each server on a temporary localhost port, drives both over RESP, and records throughput plus process RSS. Redis also reports `INFO memory used_memory`; Goblin Core reports internal zset allocation through `GOBLIN.MEMORY`.

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

## Default Configuration

Goblin Core default run: rank-location cache off; score-string cache `False`.

Source data: `benchmark-results/redis-goblin-core-1m-default-rank-cache-off.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 931,637 | 622,898 | 1.50x |
| `ZSCORE ops` | 425,956 | 401,743 | 1.06x |
| `ZRANK ops` | 456,317 | 303,458 | 1.50x |
| `ZREVRANK ops` | 454,973 | 327,235 | 1.39x |
| `ZRANGE ops` | 78,669 | 68,852 | 1.14x |
| `ZRANGE WITHSCORES ops` | 41,746 | 40,448 | 1.03x |
| `ZREVRANGE ops` | 78,248 | 71,664 | 1.09x |
| `ZREVRANGE WITHSCORES ops` | 41,749 | 40,902 | 1.02x |
| `ZREM members` | 1,267,257 | 876,487 | 1.45x |

| Memory metric | Goblin Core | Redis |
| --- | ---: | ---: |
| RSS delta | 64.30 MiB | 88.30 MiB |
| RSS delta per loaded member | 67.42 B | 92.59 B |
| Redis `used_memory` | n/a | 80.45 MiB |
| Redis `used_memory` per member | n/a | 84.36 B |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.17 us | 29.96 us | 52.33 us | 71.00 us |
| `ZSCORE` | redis | 21.38 us | 27.92 us | 60.17 us | 112.96 us |
| `ZRANK` | goblin | 20.92 us | 26.38 us | 28.96 us | 67.33 us |
| `ZRANK` | redis | 21.79 us | 26.29 us | 29.50 us | 117.54 us |
| `ZREVRANK` | goblin | 20.71 us | 26.21 us | 28.33 us | 118.54 us |
| `ZREVRANK` | redis | 21.87 us | 26.33 us | 29.17 us | 43.88 us |
| `ZRANGE` | goblin | 37.17 us | 40.58 us | 45.04 us | 64.08 us |
| `ZRANGE` | redis | 37.58 us | 42.37 us | 45.67 us | 71.29 us |
| `ZREVRANGE` | goblin | 37.08 us | 40.75 us | 45.50 us | 84.83 us |
| `ZREVRANGE` | redis | 37.13 us | 41.67 us | 44.42 us | 65.42 us |

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

## Optional Rank Cache

`--rank-cache` enables a packed member-id-to-score-location cache. It is off by default because it adds maintenance work to inserts, deletes, score updates, and score-block rebalancing.

The cache uses one packed `uint32_t` location per member plus a small block-id map.

Source data: `benchmark-results/redis-goblin-core-1m-rank-cache-on.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 712,221 | 626,886 | 1.14x |
| `ZSCORE ops` | 419,506 | 407,979 | 1.03x |
| `ZRANK ops` | 479,481 | 344,215 | 1.39x |
| `ZREVRANK ops` | 480,973 | 346,932 | 1.39x |
| `ZRANGE ops` | 78,987 | 69,591 | 1.14x |
| `ZRANGE WITHSCORES ops` | 41,954 | 40,056 | 1.05x |
| `ZREVRANGE ops` | 77,903 | 71,316 | 1.09x |
| `ZREVRANGE WITHSCORES ops` | 41,616 | 40,478 | 1.03x |
| `ZREM members` | 892,416 | 867,639 | 1.03x |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.54 us | 29.17 us | 51.12 us | 130.00 us |
| `ZSCORE` | redis | 21.63 us | 28.33 us | 52.42 us | 80.25 us |
| `ZRANK` | goblin | 19.79 us | 24.96 us | 27.46 us | 53.54 us |
| `ZRANK` | redis | 21.88 us | 26.37 us | 29.96 us | 61.29 us |
| `ZREVRANK` | goblin | 20.46 us | 25.38 us | 29.04 us | 81.83 us |
| `ZREVRANK` | redis | 21.79 us | 26.46 us | 30.25 us | 50.54 us |
| `ZRANGE` | goblin | 37.25 us | 40.75 us | 44.00 us | 90.54 us |
| `ZRANGE` | redis | 37.79 us | 42.79 us | 46.96 us | 79.33 us |
| `ZREVRANGE` | goblin | 37.29 us | 40.75 us | 44.50 us | 100.08 us |
| `ZREVRANGE` | redis | 37.33 us | 42.17 us | 46.71 us | 118.17 us |

| Metric | Default ops/sec | Rank cache ops/sec | Change |
| --- | ---: | ---: | ---: |
| `ZADD members` | 931,637 | 712,221 | -23.6% |
| `ZSCORE ops` | 425,956 | 419,506 | -1.5% |
| `ZRANK ops` | 456,317 | 479,481 | 5.1% |
| `ZREVRANK ops` | 454,973 | 480,973 | 5.7% |
| `ZRANGE ops` | 78,669 | 78,987 | 0.4% |
| `ZRANGE WITHSCORES ops` | 41,746 | 41,954 | 0.5% |
| `ZREVRANGE ops` | 78,248 | 77,903 | -0.4% |
| `ZREVRANGE WITHSCORES ops` | 41,749 | 41,616 | -0.3% |
| `ZREM members` | 1,267,257 | 892,416 | -29.6% |

Measured rank-cache allocation: 4.02 MiB (4.21 B/member).

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

For the default configuration, Goblin Core uses about 72.8% of Redis process RSS on this workload.

The rank cache is intentionally a command-line choice, not the default policy. Enable it only after measuring a workload where `ZRANK` latency dominates write throughput.
