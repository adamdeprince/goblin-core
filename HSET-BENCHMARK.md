# Goblin Core HSET Speed and Memory Benchmark

Generated on `naamah` at 2026-07-13 01:42:36 UTC.

## Method

- Every scenario starts a fresh server; engines run one at a time.
- Before the empty-server baseline, every engine constructs and deletes a `2,048`-field fixed-width hash, then settles. This warms the hash arena, index, and allocator.
- RSS is `INFO memory`'s `used_memory_rss` before and after the workload. The naamah builds read the same live `/proc` resident-set field and do not use a cached value.
- Application memory is the independent `INFO used_memory` delta. Per-key memory is `MEMORY USAGE` on incumbents and `GOBLIN.MEMORY total_allocated_bytes` on Goblin Core. These are engine-specific corroborating counters; RSS is the cross-engine comparison.
- Bulk loads use multi-field `HSET` batches of `128` and pipeline depth `256` through the shared Python RESP harness; load timing includes client encoding and is reported as observed end-to-end throughput. Point-update rates use one `redis-benchmark` client at pipeline depth `256` and `500,000` requests. Each fresh-server round takes a median of `3` point runs; the large/many tables then median `3` fresh rounds (`9` point samples), while relocation HGET medians `5` fresh rounds (`15` point samples).
- Baselines are component-wise medians of each scenario's fresh-server rounds (`3` for construction/mixed and `5` for relocation). RSS and `used_memory` deltas are paired within each round and then medianed; absolute checkpoint values are the median baseline plus that paired median delta.
- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. The target is a modest single-core memory server; naamah is a quiet 64-core test machine.
- Goblin uses the tested binary's configured compact-hash policy. The parity config keeps Redis and Valkey listpacked through 128 fields. The many-hash sweep therefore measures the engines' configured representation choices, not matched internal encodings.
- Incumbents are exercised strictly as black-box RESP servers; their source code is not inspected.

## One Large Hash

`1,000,000` distinct `18`-byte fields start with `16`-byte values. Existing-field HSET uses the load generator's `12`-digit random token. A deterministic pass then grows every value to `64` bytes.

### Speed

| Engine | bulk new fields/s | same-width HSET ops/s | bulk grow fields/s | initial optimize (s) | grown optimize (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 619,095 | 1,618,848 | 646,475 | 0.1549 | 0.3089 |
| `redis-7.2.4` | 679,639 | 1,279,345 | 641,462 | n/a | n/a |
| `redis-8.8` | 693,374 | 1,147,303 | 651,913 | n/a | n/a |
| `valkey-9.1` | 665,900 | 1,131,728 | 666,049 | n/a | n/a |
| `dragonfly` | 711,312 | 743,275 | 581,654 | n/a | n/a |

### Memory With 16-Byte Values

Goblin's row is sampled after `GOBLIN.OPTIMIZE`; the pre-optimize RSS is retained in the raw JSON.

| Engine | RSS MiB | RSS delta MiB | RSS B/field | used B/field | key B/field |
| --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 50.82 | 45.00 | 47.19 | 39.15 | 47.32 |
| `redis-7.2.4` | 93.65 | 84.46 | 88.57 | 80.44 | 80.39 |
| `redis-8.8` | 66.53 | 56.07 | 58.80 | 54.13 | 72.39 |
| `valkey-9.1` | 71.18 | 60.96 | 63.92 | 66.93 | 66.88 |
| `dragonfly` | 97.32 | 74.54 | 78.16 | 76.07 | 76.07 |

### Memory After Growing Every Value to 64 Bytes

Pre-optimize captures update fragmentation. Post-optimize is the equal-data steady state; non-Goblin engines perform no intervening maintenance operation.

| Engine | pre-opt RSS B/field | post-opt RSS B/field | post-opt used B/field | post-opt key B/field |
| --- | ---: | ---: | ---: | ---: |
| `goblin` | 118.29 | 95.47 | 87.15 | 95.31 |
| `redis-7.2.4` | 166.17 | 166.18 | 136.46 | 136.39 |
| `redis-8.8` | 121.23 | 121.25 | 110.15 | 120.39 |
| `valkey-9.1` | 122.49 | 122.51 | 113.51 | 113.46 |
| `dragonfly` | 158.70 | 158.70 | 124.07 | 124.07 |

### Goblin Large-Hash Internals

| Phase | fields | live MiB | dead MiB | arena MiB | index MiB | total MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `loaded before optimize` | 1,000,000 | 32.42 | 0.00 | 41.06 | 5.57 | 46.64 |
| `loaded after optimize` | 1,000,000 | 32.42 | 0.00 | 40.21 | 4.92 | 45.13 |
| `grown before optimize` | 1,000,000 | 78.20 | 15.26 | 106.49 | 5.85 | 112.34 |
| `grown after optimize` | 1,000,000 | 78.20 | 0.00 | 85.98 | 4.92 | 90.89 |

## Many Hashes

Each row holds approximately `500,000` total fields while varying fields per hash. Keys are `14` bytes, fields are `5` bytes, and values are `16` bytes. The update targets the middle field of random existing hashes.

### Speed

| fields/hash | hashes | Engine | load fields/s | load hashes/s | middle-field HSET ops/s |
| ---: | ---: | --- | ---: | ---: | ---: |
| 8 | 62,500 | `goblin` | 704,132 | 88,017 | 2,033,431 |
| 8 | 62,500 | `redis-7.2.4` | 659,997 | 82,500 | 1,558,330 |
| 8 | 62,500 | `redis-8.8` | 654,931 | 81,866 | 1,312,924 |
| 8 | 62,500 | `valkey-9.1` | 632,783 | 79,098 | 1,337,497 |
| 8 | 62,500 | `dragonfly` | 667,758 | 83,470 | 1,020,865 |
| 32 | 15,625 | `goblin` | 843,055 | 26,345 | 2,165,472 |
| 32 | 15,625 | `redis-7.2.4` | 737,091 | 23,034 | 1,437,425 |
| 32 | 15,625 | `redis-8.8` | 736,282 | 23,009 | 1,244,338 |
| 32 | 15,625 | `valkey-9.1` | 720,768 | 22,524 | 1,247,441 |
| 32 | 15,625 | `dragonfly` | 748,902 | 23,403 | 956,451 |
| 64 | 7,812 | `goblin` | 865,447 | 13,523 | 2,203,630 |
| 64 | 7,812 | `redis-7.2.4` | 709,239 | 11,082 | 1,253,694 |
| 64 | 7,812 | `redis-8.8` | 723,546 | 11,305 | 1,121,578 |
| 64 | 7,812 | `valkey-9.1` | 710,575 | 11,103 | 1,116,571 |
| 64 | 7,812 | `dragonfly` | 818,246 | 12,785 | 934,998 |
| 128 | 3,906 | `goblin` | 880,751 | 6,881 | 2,223,218 |
| 128 | 3,906 | `redis-7.2.4` | 625,002 | 4,883 | 984,693 |
| 128 | 3,906 | `redis-8.8` | 658,244 | 5,143 | 922,922 |
| 128 | 3,906 | `valkey-9.1` | 652,116 | 5,095 | 931,516 |
| 128 | 3,906 | `dragonfly` | 844,109 | 6,595 | 934,998 |

### Memory

Sampled key bytes are the mean of the first, middle, and last hash.

| fields/hash | Engine | RSS MiB | RSS B/hash | RSS B/field | used B/hash | sampled key B/hash |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 8 | `goblin` | 26.80 | 352.12 | 44.02 | 271.20 | 232.00 |
| 8 | `redis-7.2.4` | 26.36 | 287.83 | 35.98 | 288.80 | 280.00 |
| 8 | `redis-8.8` | 27.03 | 278.40 | 34.80 | 278.57 | 247.00 |
| 8 | `valkey-9.1` | 27.09 | 284.88 | 35.61 | 281.87 | 264.00 |
| 8 | `dragonfly` | 40.12 | 290.26 | 36.28 | 286.93 | 224.00 |
| 32 | `goblin` | 22.38 | 1112.01 | 34.75 | 943.18 | 904.00 |
| 32 | `redis-7.2.4` | 23.42 | 954.73 | 29.84 | 962.03 | 952.00 |
| 32 | `redis-8.8` | 24.64 | 951.84 | 29.75 | 951.69 | 847.00 |
| 32 | `valkey-9.1` | 24.57 | 962.33 | 30.07 | 955.10 | 936.00 |
| 32 | `dragonfly` | 36.92 | 946.60 | 29.58 | 946.34 | 896.00 |
| 64 | `goblin` | 22.14 | 2190.62 | 34.23 | 1,839.16 | 1,800.00 |
| 64 | `redis-7.2.4` | 22.89 | 1846.66 | 28.85 | 1,859.68 | 1,848.00 |
| 64 | `redis-8.8` | 24.22 | 1850.33 | 28.91 | 1,849.41 | 1,647.00 |
| 64 | `valkey-9.1` | 24.13 | 1866.58 | 29.17 | 1,852.72 | 1,832.00 |
| 64 | `dragonfly` | 51.51 | 3846.95 | 60.11 | 3,857.56 | 3,824.00 |
| 128 | `goblin` | 21.76 | 4279.51 | 33.43 | 3,631.11 | 3,592.00 |
| 128 | `redis-7.2.4` | 22.80 | 3654.52 | 28.55 | 3,654.96 | 3,640.00 |
| 128 | `redis-8.8` | 24.04 | 3650.33 | 28.52 | 3,644.59 | 3,247.00 |
| 128 | `valkey-9.1` | 23.96 | 3687.03 | 28.80 | 3,648.02 | 3,624.00 |
| 128 | `dragonfly` | 50.66 | 7490.46 | 58.52 | 7,520.00 | 7,520.00 |

## Goblin Full-Hash Relocation Density

Goblin is started with `--hash-listpack-max-entries 0` so every case uses the full representation. A deterministic subset grows from `16` to `64` bytes. The first growth is timed separately because it lazily allocates one 64-field relocation block; HGET is measured before compaction. Each row is the median of `5` fresh-server rounds over `1,000,000` fields.

| pattern | density | grown fields | first grow us | HGET ops/s | pre-opt RSS B/field | post-opt RSS B/field | dead MiB | compact ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `random` | 0% | 0 | n/a | 1,736,889 | 51.79 | 47.34 | 0.00 | 171.02 |
| `random` | 0.1% | 1,000 | 76.42 | 1,761,352 | 51.86 | 47.46 | 0.02 | 172.44 |
| `random` | 1% | 10,000 | 83.64 | 1,707,249 | 52.44 | 47.90 | 0.15 | 177.87 |
| `random` | 10% | 100,000 | 272.77 | 1,684,256 | 59.03 | 52.25 | 1.53 | 202.19 |
| `random` | 50% | 500,000 | 306.98 | 1,493,206 | 84.65 | 71.46 | 7.63 | 293.60 |
| `random` | 100% | 1,000,000 | 253.04 | 1,359,304 | 116.65 | 95.47 | 15.26 | 407.15 |
| `clustered` | 0% | 0 | n/a | 1,707,249 | 51.79 | 47.34 | 0.00 | 170.41 |
| `clustered` | 0.1% | 1,000 | 110.47 | 1,773,844 | 51.86 | 47.39 | 0.02 | 172.33 |
| `clustered` | 1% | 10,000 | 83.50 | 1,701,442 | 52.44 | 47.84 | 0.15 | 177.10 |
| `clustered` | 10% | 100,000 | 81.15 | 1,684,256 | 58.20 | 52.18 | 1.53 | 192.06 |
| `clustered` | 50% | 500,000 | 87.99 | 1,515,830 | 83.80 | 71.47 | 7.63 | 258.04 |
| `clustered` | 100% | 1,000,000 | 119.94 | 1,344,688 | 116.65 | 95.55 | 15.26 | 334.83 |

## Mixed Hash Write Latency

A seeded depth-one workload starts with `100,000` fields and mixes inserts, same-width updates, deletes, and value growth. `10,000` operations warm the exact workload before `200,000` measured samples. Percentiles are end-to-end RESP round-trip latency; p99.9 is not inferred from throughput batches. Each row is the median of `3` fresh-server rounds.

| Engine | ops/s | insert/update/delete/grow | p50 us | p95 us | p99 us | p99.9 us | max us | RSS B/field | dead MiB | compacting | final fields |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 42,326 | 20,153/24,902/80,091/74,854 | 22.45 | 26.61 | 33.41 | 46.99 | 744.64 | 208.49 | 2.81 | 0 | 37,053 |
| `redis-7.2.4` | 40,324 | 20,153/24,902/80,091/74,854 | 23.37 | 29.98 | 34.89 | 45.01 | 381.34 | 313.72 | n/a | n/a | 37,053 |
| `redis-8.8` | 38,000 | 20,153/24,902/80,091/74,854 | 24.45 | 33.07 | 37.27 | 48.91 | 1485.65 | 258.78 | n/a | n/a | 37,053 |
| `valkey-9.1` | 39,256 | 20,153/24,902/80,091/74,854 | 24.04 | 31.46 | 35.92 | 46.80 | 386.14 | 253.37 | n/a | n/a | 37,053 |
| `dragonfly` | 39,562 | 20,153/24,902/80,091/74,854 | 24.48 | 27.12 | 30.71 | 36.35 | 478.25 | 349.21 | n/a | n/a | 37,053 |

| Engine | operation | p50 us | p95 us | p99 us | p99.9 us | max us |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | insert | 22.56 | 26.69 | 33.51 | 50.23 | 611.78 |
| `goblin` | update | 22.55 | 26.56 | 33.31 | 40.45 | 390.59 |
| `goblin` | delete | 22.22 | 26.32 | 33.08 | 39.60 | 393.08 |
| `goblin` | grow | 22.56 | 26.90 | 33.79 | 68.28 | 744.64 |
| `redis-7.2.4` | insert | 23.42 | 30.11 | 34.97 | 44.64 | 282.17 |
| `redis-7.2.4` | update | 23.50 | 30.37 | 35.11 | 45.94 | 154.32 |
| `redis-7.2.4` | delete | 22.98 | 29.52 | 34.44 | 44.59 | 273.64 |
| `redis-7.2.4` | grow | 23.54 | 29.68 | 34.93 | 44.41 | 276.11 |
| `redis-8.8` | insert | 24.47 | 32.91 | 37.28 | 48.47 | 79.58 |
| `redis-8.8` | update | 24.49 | 33.04 | 37.07 | 49.07 | 1485.65 |
| `redis-8.8` | delete | 24.11 | 32.65 | 36.76 | 47.94 | 114.28 |
| `redis-8.8` | grow | 24.65 | 33.40 | 37.82 | 49.51 | 108.07 |
| `valkey-9.1` | insert | 24.08 | 31.64 | 36.12 | 47.64 | 134.17 |
| `valkey-9.1` | update | 24.12 | 31.84 | 36.45 | 46.82 | 98.99 |
| `valkey-9.1` | delete | 23.61 | 30.82 | 35.35 | 46.40 | 133.95 |
| `valkey-9.1` | grow | 24.23 | 31.77 | 36.25 | 47.04 | 372.44 |
| `dragonfly` | insert | 24.59 | 26.86 | 30.47 | 35.95 | 333.93 |
| `dragonfly` | update | 24.63 | 27.32 | 30.92 | 35.48 | 109.45 |
| `dragonfly` | delete | 24.19 | 26.31 | 30.11 | 35.00 | 446.19 |
| `dragonfly` | grow | 24.63 | 27.73 | 31.12 | 37.52 | 365.80 |

The medium-hash representation crossover is measured separately in [HASH-THRESHOLD-SWEEP.md](HASH-THRESHOLD-SWEEP.md).

## Conclusions and Recommendations

Goblin's large-hash steady state is the strongest memory result. With 16-byte
values it uses `47.19` RSS bytes per field, `19.7%` less than Redis 8.8 and
`46.7%` less than Redis 7.2.4. After every value grows to 64 bytes and Goblin is
optimized, it remains `21.3%` below Redis 8.8. Same-width HSET reaches `1.62M`
operations/s, ahead of every incumbent in this test.

The mixed result is competitive on both throughput and latency. Goblin is
fastest at `42,326` operations/s with a `33.41 us` p99. Dragonfly has the best
tail at `30.71 us` p99 and `36.35 us` p99.9. Goblin's growth path is the main
remaining tail: grow p99.9 is `68.28 us`, versus `44-50 us` for the Redis-family
servers.

The small-hash RSS result needs interpretation. Goblin is `17-26%` heavier than
Redis/Valkey in resident bytes from 8 through 128 short fields. Its per-key
counter beats Redis 8.8 at 8 fields (`232` versus `247` bytes), then runs `7-11%`
heavier from 32 through 128 fields. Process `used_memory` is slightly lower at
every shape. Both the compact layout and allocator/blob-pool residency therefore
need measurement; the RSS gap is not explained by either counter alone.

1. Close the remaining construction gap. Large-hash loading is now only `7-13%`
   behind the incumbents. Profile compact-to-full promotion and command-sized
   HSET batches, then add a known-unique bulk build path after resolving duplicate
   fields once.
2. Reduce the remaining tail events. Build an actually fitting demotion blob in
   one known-unique pass, and make Swiss-index tombstone cleanup incremental.
   Do not put the existing synchronous full-table cleanup on the command path.
3. Keep the lazy 64-field relocation blocks. They avoid the old O(N) first-grow
   initialization and cost almost nothing at 0.1% density. HGET falls about 22%
   at 100% relocation, so workloads with pervasive growth should compact during
   a maintenance window or allow bounded automatic evacuation to catch up.
4. Keep full `GOBLIN.OPTIMIZE` deliberate. Rebuilding one million fields takes
   `170-407 ms` here. Continue validating the eight-field/16 KiB automatic arena
   budget under longer sustained churn, and add cumulative reclaimed-chunk
   counters so production progress remains visible after a victim completes.

## Binaries

- `goblin`: `build-hset/goblin-core`
- `redis-7.2.4`: `/home/adam/bench/redis-7.2.4/src/redis-server`
- `redis-8.8`: `/home/adam/bench/redis-8.8.0/src/redis-server`
- `valkey-9.1`: `/home/adam/bench/valkey-9.1.0/src/valkey-server`
- `dragonfly`: `/home/adam/dragonfly/build-opt/dragonfly`
