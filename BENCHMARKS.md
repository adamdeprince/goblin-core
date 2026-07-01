# Goblin Core Benchmarks

Generated: 2026-07-01 22:19:07 UTC.

These results compare Goblin Core against Redis for the current sorted-set-focused implementation. Goblin Core's optional rank-location cache and score-string cache are off by default.

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

## Default Configuration

Goblin Core default run: rank cache mode `off`; score-string cache `False`.

Source data: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-off.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 980,358 | 624,890 | 1.57x |
| `ZSCORE ops` | 449,978 | 401,897 | 1.12x |
| `ZRANK ops` | 477,493 | 346,988 | 1.38x |
| `ZREVRANK ops` | 477,707 | 343,066 | 1.39x |
| `ZRANGE ops` | 80,239 | 69,811 | 1.15x |
| `ZRANGE WITHSCORES ops` | 42,291 | 40,544 | 1.04x |
| `ZREVRANGE ops` | 78,952 | 71,325 | 1.11x |
| `ZREVRANGE WITHSCORES ops` | 41,788 | 40,903 | 1.02x |
| `ZREM members` | 1,275,173 | 866,436 | 1.47x |

| Memory metric | Goblin Core | Redis |
| --- | ---: | ---: |
| RSS delta | 64.30 MiB | 88.31 MiB |
| RSS delta per loaded member | 67.42 B | 92.60 B |
| Redis `used_memory` | n/a | 80.46 MiB |
| Redis `used_memory` per member | n/a | 84.37 B |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.42 us | 28.58 us | 55.54 us | 124.42 us |
| `ZSCORE` | redis | 21.58 us | 28.62 us | 56.21 us | 138.00 us |
| `ZRANK` | goblin | 20.67 us | 25.87 us | 28.33 us | 92.83 us |
| `ZRANK` | redis | 21.92 us | 26.58 us | 29.83 us | 50.17 us |
| `ZREVRANK` | goblin | 20.38 us | 25.92 us | 28.25 us | 43.79 us |
| `ZREVRANK` | redis | 21.92 us | 26.50 us | 29.83 us | 117.08 us |
| `ZRANGE` | goblin | 36.83 us | 41.21 us | 49.04 us | 144.17 us |
| `ZRANGE` | redis | 37.29 us | 42.58 us | 46.83 us | 142.29 us |
| `ZREVRANGE` | goblin | 36.88 us | 40.58 us | 45.04 us | 87.88 us |
| `ZREVRANGE` | redis | 37.08 us | 42.00 us | 45.75 us | 66.96 us |

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

`--rank-cache-mode exact` enables the packed member-id-to-score-location cache. `--rank-cache-mode block-hint` stores only member-to-score-block hints, reducing write maintenance while retaining a smaller `ZRANK` read assist.

Source data:

- `off`: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-off.json`
- `exact`: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-exact.json`
- `block-hint`: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-block-hint.json`

| Metric | off ops/sec | exact ops/sec | block-hint ops/sec | exact vs off | block-hint vs off |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZADD members` | 980,358 | 817,082 | 909,724 | -16.7% | -7.2% |
| `ZSCORE ops` | 449,978 | 433,161 | 418,916 | -3.7% | -6.9% |
| `ZRANK ops` | 477,493 | 481,859 | 458,668 | 0.9% | -3.9% |
| `ZREVRANK ops` | 477,707 | 487,545 | 462,598 | 2.1% | -3.2% |
| `ZRANGE ops` | 80,239 | 77,985 | 78,310 | -2.8% | -2.4% |
| `ZRANGE WITHSCORES ops` | 42,291 | 41,710 | 41,642 | -1.4% | -1.5% |
| `ZREVRANGE ops` | 78,952 | 77,827 | 78,011 | -1.4% | -1.2% |
| `ZREVRANGE WITHSCORES ops` | 41,788 | 41,711 | 41,660 | -0.2% | -0.3% |
| `ZREM members` | 1,275,173 | 1,072,117 | 1,242,840 | -15.9% | -2.5% |

Rank-cache allocation by mode:

| Mode | Rank cache MiB | Rank cache B/member | RSS delta B/member |
| --- | ---: | ---: | ---: |
| `off` | 0.00 | 0.00 | 67.42 |
| `exact` | 4.02 | 4.21 | 71.32 |
| `block-hint` | 4.02 | 4.21 | 71.30 |

Exact mode is the read-heavy `ZRANK` option; block-hint mode is the churn-heavy leaderboard option.

### Rank Cache Mode: `exact`

Source data: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-exact.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 817,082 | 623,041 | 1.31x |
| `ZSCORE ops` | 433,161 | 403,386 | 1.07x |
| `ZRANK ops` | 481,859 | 347,213 | 1.39x |
| `ZREVRANK ops` | 487,545 | 345,659 | 1.41x |
| `ZRANGE ops` | 77,985 | 69,179 | 1.13x |
| `ZRANGE WITHSCORES ops` | 41,710 | 40,116 | 1.04x |
| `ZREVRANGE ops` | 77,827 | 70,745 | 1.10x |
| `ZREVRANGE WITHSCORES ops` | 41,711 | 40,610 | 1.03x |
| `ZREM members` | 1,072,117 | 871,626 | 1.23x |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.21 us | 28.42 us | 59.00 us | 78.79 us |
| `ZSCORE` | redis | 21.21 us | 28.50 us | 56.46 us | 136.58 us |
| `ZRANK` | goblin | 19.42 us | 24.75 us | 27.37 us | 72.79 us |
| `ZRANK` | redis | 21.83 us | 26.33 us | 29.50 us | 81.92 us |
| `ZREVRANK` | goblin | 19.29 us | 24.67 us | 27.08 us | 89.33 us |
| `ZREVRANK` | redis | 21.92 us | 26.46 us | 30.13 us | 110.08 us |
| `ZRANGE` | goblin | 36.54 us | 40.00 us | 43.87 us | 99.83 us |
| `ZRANGE` | redis | 37.79 us | 42.83 us | 47.37 us | 145.17 us |
| `ZREVRANGE` | goblin | 36.58 us | 40.42 us | 44.79 us | 128.17 us |
| `ZREVRANGE` | redis | 37.21 us | 42.08 us | 46.42 us | 97.33 us |

### Rank Cache Mode: `block-hint`

Source data: `benchmark-results/redis-goblin-core-1m-modes-rank-cache-block-hint.json`

| Metric | Goblin Core ops/sec | Redis ops/sec | Goblin Core / Redis |
| --- | ---: | ---: | ---: |
| `ZADD members` | 909,724 | 622,179 | 1.46x |
| `ZSCORE ops` | 418,916 | 402,942 | 1.04x |
| `ZRANK ops` | 458,668 | 347,759 | 1.32x |
| `ZREVRANK ops` | 462,598 | 344,315 | 1.34x |
| `ZRANGE ops` | 78,310 | 69,622 | 1.12x |
| `ZRANGE WITHSCORES ops` | 41,642 | 40,260 | 1.03x |
| `ZREVRANGE ops` | 78,011 | 71,066 | 1.10x |
| `ZREVRANGE WITHSCORES ops` | 41,660 | 40,653 | 1.02x |
| `ZREM members` | 1,242,840 | 862,317 | 1.44x |

Latency percentiles:

| Metric | Target | p50 | p95 | p99 | max |
| --- | --- | ---: | ---: | ---: | ---: |
| `ZSCORE` | goblin | 21.54 us | 29.21 us | 53.75 us | 85.83 us |
| `ZSCORE` | redis | 21.25 us | 28.67 us | 55.92 us | 120.46 us |
| `ZRANK` | goblin | 20.42 us | 25.92 us | 28.54 us | 88.21 us |
| `ZRANK` | redis | 21.92 us | 26.54 us | 30.17 us | 88.17 us |
| `ZREVRANK` | goblin | 20.00 us | 25.79 us | 28.21 us | 92.00 us |
| `ZREVRANK` | redis | 22.00 us | 26.67 us | 30.54 us | 83.62 us |
| `ZRANGE` | goblin | 36.79 us | 40.25 us | 44.46 us | 91.04 us |
| `ZRANGE` | redis | 37.62 us | 42.46 us | 45.96 us | 129.58 us |
| `ZREVRANGE` | goblin | 37.04 us | 41.08 us | 44.79 us | 113.42 us |
| `ZREVRANGE` | redis | 36.96 us | 41.67 us | 44.96 us | 166.67 us |

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
