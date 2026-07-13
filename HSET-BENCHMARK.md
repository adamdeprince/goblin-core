# Goblin Core HSET Speed and Memory Benchmark

Generated from one complete arena-enabled run of the native C++ harness on a
dedicated benchmark host at 2026-07-13 18:55:52 UTC.

## Summary

Goblin wins on memory, compact-hash construction, and existing-field updates:
it has the lowest workload RSS at 8, 64, and 128 fields per compact hash, comes
within `0.7%` of the lowest result at 32, leads construction at 8 and 32 fields,
and leads every middle-field HSET test by `28.2-120.9%` over the fastest
incumbent. At one million fields it uses `20.1%` less RSS than Redis 8.8 with
16-byte values and `18.0%` less after values grow to 64 bytes; same-width HSET
is `28.5%` faster than Redis 7.2.4. It also leads mixed-workload throughput and
memory. Goblin loses on million-field construction, where it is `22-36%`
behind the incumbents, and Dragonfly leads compact-hash construction by `3.6%`
at 64 fields and `10.1%` at 128. Dragonfly has the best mixed p99 and p99.9;
all four incumbents beat Goblin at p99.9, and Goblin records the largest
mixed-workload maximum latency. Randomly relocating half of a full hash is
another weak path, requiring `393.62 ms` to compact.

## Method

- Every scenario starts a fresh server; engines run one at a time.
- Before the empty-server baseline, every engine constructs and deletes a `2,048`-field fixed-width hash, then settles. This warms the hash arena, index, and allocator.
- RSS is read by the harness directly from the launched server PID's Linux `/proc/<pid>/status`: `VmRSS + HugetlbPages`. The latter is added because explicit HugeTLB mappings are resident but excluded from `VmRSS`. No server-reported RSS field is used.
- Application memory is the independent `INFO used_memory` delta. Per-key memory is `MEMORY USAGE` on incumbents and `GOBLIN.MEMORY total_allocated_bytes` on Goblin Core. These are engine-specific corroborating counters; RSS is the cross-engine comparison.
- Bulk generation, response validation, mixed latency, process control, aggregation, and report generation run in the native C++ harness. Point probes use the C `redis-benchmark` client.
- Bulk loads use multi-field `HSET` batches of `128` and pipeline depth `256`; load timing includes native client encoding and is reported as observed end-to-end throughput. Point-update rates use pipeline depth `256` and `500,000` requests. Each fresh-server round takes a median of `3` point runs; the large/many tables then median `3` fresh rounds (`9` point samples), while relocation HGET medians `5` fresh rounds (`15` point samples).
- Baselines are component-wise medians of each scenario's fresh-server rounds (`3` for construction, `3` for mixed, and `5` for relocation). RSS and `used_memory` deltas are paired within each round and then medianed; absolute checkpoint values are the median baseline plus that paired median delta.
- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. The target is a modest single-core memory server; the benchmark host is a quiet 64-core test machine.
- Goblin uses its default shared string encoding and configured compact-hash policy; no optional Goblin arguments are set. The parity config keeps Redis and Valkey listpacked through 128 fields. The many-hash sweep therefore measures the engines' configured representation choices, not matched internal encodings.
- Incumbents are exercised strictly as black-box RESP servers; their source code is not inspected.

## One Large Hash

`1,000,000` distinct `18`-byte fields start with `16`-byte values. Existing-field HSET uses the load generator's `12`-digit random token. A deterministic pass then grows every value to `64` bytes.

### Speed

| Engine | bulk new fields/s | same-width HSET ops/s | bulk grow fields/s | initial optimize (s) | grown optimize (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 1,522,601 | 1,640,079 | 1,881,187 | 0.0215 | 0.1617 |
| `redis-7.2.4` | 1,997,344 | 1,276,082 | 1,843,332 | n/a | n/a |
| `redis-8.8` | 2,187,644 | 1,149,940 | 2,045,146 | n/a | n/a |
| `valkey-9.1` | 1,956,254 | 1,116,571 | 2,109,156 | n/a | n/a |
| `dragonfly` | 2,381,894 | 774,341 | 1,573,442 | n/a | n/a |

### Memory With 16-Byte Values

Goblin's row is sampled after `GOBLIN.OPTIMIZE`; the pre-optimize RSS is retained in the raw JSON.

| Engine | RSS MiB | RSS delta MiB | RSS B/field | used B/field | key B/field |
| --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 52.60 | 46.41 | 48.67 | 40.16 | 49.38 |
| `redis-7.2.4` | 93.73 | 84.47 | 88.57 | 80.44 | 80.39 |
| `redis-8.8` | 68.51 | 58.06 | 60.88 | 54.13 | 72.39 |
| `valkey-9.1` | 71.19 | 60.94 | 63.90 | 66.93 | 66.88 |
| `dragonfly` | 95.48 | 72.70 | 76.23 | 76.07 | 76.07 |

### Memory After Growing Every Value to 64 Bytes

Pre-optimize captures update fragmentation. Post-optimize is the equal-data steady state; non-Goblin engines perform no intervening maintenance operation.

| Engine | pre-opt RSS B/field | post-opt RSS B/field | post-opt used B/field | post-opt key B/field |
| --- | ---: | ---: | ---: | ---: |
| `goblin` | 118.90 | 100.99 | 88.16 | 101.18 |
| `redis-7.2.4` | 168.30 | 168.30 | 136.46 | 136.39 |
| `redis-8.8` | 123.13 | 123.13 | 110.15 | 120.39 |
| `valkey-9.1` | 122.14 | 122.14 | 113.51 | 113.46 |
| `dragonfly` | 156.72 | 156.72 | 124.07 | 124.07 |

### Goblin Large-Hash Internals

| Phase | fields | live MiB | dead MiB | arena MiB | index MiB | total MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `loaded before optimize` | 1,000,000 | 33.38 | 0.00 | 42.17 | 5.57 | 47.75 |
| `loaded after optimize` | 1,000,000 | 33.38 | 0.00 | 42.17 | 4.92 | 47.09 |
| `grown before optimize` | 1,000,000 | 79.15 | 16.21 | 107.99 | 5.85 | 113.84 |
| `grown after optimize` | 1,000,000 | 79.15 | 0.00 | 91.58 | 4.92 | 96.50 |

## Many Hashes

Each row holds approximately `500,000` total fields while varying fields per hash. Keys are `14` bytes, fields are `5` bytes, and values are `16` bytes. The update targets the middle field of random existing hashes.

### Speed

| fields/hash | hashes | Engine | load fields/s | load hashes/s | middle-field HSET ops/s |
| ---: | ---: | --- | ---: | ---: | ---: |
| 8 | 62,500 | `goblin` | 3,197,066 | 399,633 | 2,137,710 |
| 8 | 62,500 | `redis-7.2.4` | 2,964,990 | 370,624 | 1,667,413 |
| 8 | 62,500 | `redis-8.8` | 2,810,137 | 351,267 | 1,351,957 |
| 8 | 62,500 | `valkey-9.1` | 2,495,291 | 311,911 | 1,363,008 |
| 8 | 62,500 | `dragonfly` | 3,036,296 | 379,537 | 1,035,660 |
| 32 | 15,625 | `goblin` | 3,697,304 | 115,541 | 2,184,384 |
| 32 | 15,625 | `redis-7.2.4` | 2,699,128 | 84,348 | 1,488,762 |
| 32 | 15,625 | `redis-8.8` | 2,715,778 | 84,868 | 1,272,835 |
| 32 | 15,625 | `valkey-9.1` | 2,511,279 | 78,477 | 1,256,844 |
| 32 | 15,625 | `dragonfly` | 3,047,503 | 95,234 | 973,198 |
| 64 | 7,812 | `goblin` | 3,466,189 | 54,159 | 2,233,143 |
| 64 | 7,812 | `redis-7.2.4` | 2,114,432 | 33,038 | 1,279,345 |
| 64 | 7,812 | `redis-8.8` | 2,221,816 | 34,716 | 1,134,295 |
| 64 | 7,812 | `valkey-9.1` | 2,118,800 | 33,106 | 1,129,174 |
| 64 | 7,812 | `dragonfly` | 3,594,342 | 56,162 | 943,819 |
| 128 | 3,906 | `goblin` | 3,300,346 | 25,784 | 2,174,887 |
| 128 | 3,906 | `redis-7.2.4` | 1,433,608 | 11,200 | 984,693 |
| 128 | 3,906 | `redis-8.8` | 1,615,113 | 12,618 | 916,161 |
| 128 | 3,906 | `valkey-9.1` | 1,577,533 | 12,324 | 922,923 |
| 128 | 3,906 | `dragonfly` | 3,670,699 | 28,677 | 938,507 |

### Memory

Sampled key bytes are the mean of the first, middle, and last hash.

| fields/hash | Engine | RSS MiB | RSS B/hash | RSS B/field | used B/hash | sampled key B/hash |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 8 | `goblin` | 22.64 | 276.10 | 34.51 | 267.20 | 228.00 |
| 8 | `redis-7.2.4` | 26.36 | 287.77 | 35.97 | 288.80 | 280.00 |
| 8 | `redis-8.8` | 27.02 | 278.13 | 34.77 | 278.55 | 247.00 |
| 8 | `valkey-9.1` | 27.21 | 284.75 | 35.59 | 281.87 | 264.00 |
| 8 | `dragonfly` | 40.08 | 290.72 | 36.34 | 286.93 | 224.00 |
| 32 | `goblin` | 20.39 | 953.16 | 29.79 | 939.18 | 900.00 |
| 32 | `redis-7.2.4` | 23.42 | 954.47 | 29.83 | 962.03 | 952.00 |
| 32 | `redis-8.8` | 24.62 | 951.32 | 29.73 | 951.69 | 847.00 |
| 32 | `valkey-9.1` | 24.58 | 962.07 | 30.06 | 955.07 | 936.00 |
| 32 | `dragonfly` | 36.87 | 946.86 | 29.59 | 946.34 | 896.00 |
| 64 | `goblin` | 19.85 | 1834.08 | 28.66 | 1,835.16 | 1,796.00 |
| 64 | `redis-7.2.4` | 22.95 | 1846.14 | 28.85 | 1,859.68 | 1,848.00 |
| 64 | `redis-8.8` | 24.23 | 1848.76 | 28.89 | 1,849.35 | 1,647.00 |
| 64 | `valkey-9.1` | 24.08 | 1865.01 | 29.14 | 1,852.69 | 1,832.00 |
| 64 | `dragonfly` | 51.45 | 3848.52 | 60.13 | 3,857.56 | 3,824.00 |
| 128 | `goblin` | 19.61 | 3605.24 | 28.17 | 3,627.11 | 3,588.00 |
| 128 | `redis-7.2.4` | 22.80 | 3650.33 | 28.52 | 3,654.96 | 3,640.00 |
| 128 | `redis-8.8` | 24.11 | 3650.33 | 28.52 | 3,644.66 | 3,247.00 |
| 128 | `valkey-9.1` | 23.97 | 3682.83 | 28.77 | 3,648.08 | 3,624.00 |
| 128 | `dragonfly` | 50.65 | 7490.46 | 58.52 | 7,520.00 | 7,520.00 |

## Goblin Full-Hash Relocation Density

Goblin is started with `--hash-listpack-max-entries 0` so every case uses the full representation. A deterministic subset grows from `16` to `64` bytes. The first growth is timed separately because it lazily allocates one 64-field relocation block; HGET is measured before compaction. Each row is the median of `5` fresh-server rounds over `1,000,000` fields.

| pattern | density | grown fields | first grow us | HGET ops/s | pre-opt RSS B/field | post-opt RSS B/field | dead MiB | compact ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `random` | 0% | 0 | n/a | 1,718,983 | 52.26 | 48.58 | 0.00 | 24.81 |
| `random` | 0.1% | 1,000 | 61.58 | 1,724,910 | 52.33 | 48.95 | 0.02 | 149.03 |
| `random` | 1% | 10,000 | 61.53 | 1,672,990 | 52.91 | 51.14 | 0.16 | 174.44 |
| `random` | 10% | 100,000 | 63.73 | 1,645,474 | 59.90 | 57.68 | 1.62 | 220.97 |
| `random` | 50% | 500,000 | 65.51 | 1,534,429 | 85.93 | 76.91 | 8.11 | 393.62 |
| `random` | 100% | 1,000,000 | 64.72 | 1,433,307 | 118.44 | 100.91 | 16.21 | 228.78 |
| `clustered` | 0% | 0 | n/a | 1,724,910 | 52.26 | 48.58 | 0.00 | 26.76 |
| `clustered` | 0.1% | 1,000 | 50.30 | 1,656,371 | 52.33 | 48.69 | 0.02 | 35.21 |
| `clustered` | 1% | 10,000 | 67.91 | 1,749,035 | 52.91 | 49.16 | 0.16 | 35.59 |
| `clustered` | 10% | 100,000 | 69.42 | 1,650,904 | 58.76 | 53.87 | 1.62 | 52.51 |
| `clustered` | 50% | 500,000 | 61.36 | 1,563,200 | 84.76 | 74.78 | 8.11 | 109.92 |
| `clustered` | 100% | 1,000,000 | 63.88 | 1,421,091 | 118.43 | 100.91 | 16.21 | 190.64 |

## Mixed Hash Write Latency

A seeded depth-one workload starts with `100,000` fields and mixes inserts, same-width updates, deletes, and value growth. `10,000` operations warm the exact workload before `200,000` measured samples. The native C++ client measures each end-to-end RESP round trip; p99.9 is not inferred from throughput batches. Each row is the median of `3` fresh-server rounds.

| Engine | ops/s | insert/update/delete/grow | p50 us | p95 us | p99 us | p99.9 us | max us | RSS B/field | dead MiB | compacting | final fields |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 55,455 | 20,079/25,292/79,923/74,706 | 17.34 | 20.56 | 27.19 | 36.07 | 2131.13 | 236.51 | 2.72 | 1 | 37,165 |
| `redis-7.2.4` | 53,985 | 20,079/25,292/79,923/74,706 | 18.09 | 21.42 | 27.79 | 33.10 | 352.45 | 312.56 | n/a | n/a | 37,165 |
| `redis-8.8` | 51,616 | 20,079/25,292/79,923/74,706 | 18.91 | 21.55 | 29.07 | 34.59 | 349.61 | 257.67 | n/a | n/a | 37,165 |
| `valkey-9.1` | 52,778 | 20,079/25,292/79,923/74,706 | 18.55 | 21.03 | 28.36 | 32.81 | 398.21 | 252.38 | n/a | n/a | 37,165 |
| `dragonfly` | 51,244 | 20,079/25,292/79,923/74,706 | 19.26 | 20.48 | 24.61 | 27.85 | 341.47 | 296.91 | n/a | n/a | 37,165 |

| Engine | operation | p50 us | p95 us | p99 us | p99.9 us | max us |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | insert | 17.37 | 20.58 | 27.20 | 35.42 | 1535.34 |
| `goblin` | update | 17.45 | 20.52 | 26.53 | 31.90 | 78.05 |
| `goblin` | delete | 17.26 | 20.42 | 26.46 | 32.67 | 1125.83 |
| `goblin` | grow | 17.36 | 20.63 | 27.67 | 46.50 | 1394.05 |
| `redis-7.2.4` | insert | 18.06 | 21.36 | 27.55 | 31.96 | 73.29 |
| `redis-7.2.4` | update | 18.18 | 21.41 | 27.82 | 33.78 | 251.09 |
| `redis-7.2.4` | delete | 17.89 | 21.19 | 27.51 | 33.22 | 352.45 |
| `redis-7.2.4` | grow | 18.18 | 21.63 | 27.90 | 33.04 | 342.59 |
| `redis-8.8` | insert | 18.87 | 21.34 | 29.30 | 33.58 | 93.86 |
| `redis-8.8` | update | 18.97 | 21.53 | 29.28 | 34.98 | 71.28 |
| `redis-8.8` | delete | 18.79 | 21.19 | 28.93 | 34.31 | 311.96 |
| `redis-8.8` | grow | 19.03 | 22.10 | 29.25 | 35.41 | 102.59 |
| `valkey-9.1` | insert | 18.54 | 20.96 | 28.59 | 32.13 | 74.74 |
| `valkey-9.1` | update | 18.59 | 20.98 | 28.36 | 33.20 | 180.08 |
| `valkey-9.1` | delete | 18.36 | 20.65 | 27.97 | 32.07 | 336.22 |
| `valkey-9.1` | grow | 18.69 | 21.62 | 28.71 | 33.57 | 158.37 |
| `dragonfly` | insert | 19.27 | 20.32 | 24.17 | 27.05 | 121.04 |
| `dragonfly` | update | 19.35 | 20.45 | 24.42 | 27.46 | 71.28 |
| `dragonfly` | delete | 19.12 | 20.15 | 24.02 | 26.65 | 101.48 |
| `dragonfly` | grow | 19.36 | 20.99 | 25.03 | 29.32 | 183.56 |

The medium-hash representation crossover is measured separately in [HASH-THRESHOLD-SWEEP.md](HASH-THRESHOLD-SWEEP.md).

## Conclusions and Recommendations

Goblin's large-hash steady state remains the strongest memory result. With
16-byte values it uses `48.67` RSS bytes per field, `20.1%` less than Redis 8.8
and `45.0%` less than Redis 7.2.4. After every value grows to 64 bytes and
Goblin is optimized, it uses `100.99` RSS bytes per field, `18.0%` less than
Redis 8.8. Same-width HSET reaches `1.64M` operations/s, `28.5%` ahead of Redis
7.2.4 and `42.6%` ahead of Redis 8.8. Bulk construction is the large-hash speed
gap: Goblin is `22-36%` behind the incumbents.

Compact hashes use a 16-byte handle in the keyspace object slot and store their
exact-size blob in the movable keyspace arena. With default encoding enabled,
Goblin has the lowest RSS at 8 fields (`276.10` B/hash), is within `0.7%` of the
lowest result at 32, and leads the Redis family at 64 and 128 fields. Its
middle-field HSET rate leads every shape, ranging from `2.12M` to `2.24M`
operations/s. Construction leads every incumbent at 8 and 32 fields. Dragonfly
is `3.6%` faster at 64 fields and `10.1%` faster at 128, while Goblin remains
well ahead of the Redis-family engines at both shapes.

Goblin leads the mixed workload at `55,455` operations/s and has the lowest
mixed RSS at `236.51` bytes per final field, `6.3%` below the next-lowest
incumbent. Its `27.19 us` p99 beats the Redis-family servers, but Dragonfly has
the best tail at `24.61 us` p99 and `27.85 us` p99.9. All four incumbents beat
Goblin's `36.07 us` p99.9, and Goblin's `2131.13 us` maximum is the largest
outlier in the table. The sample ends with `2.72 MiB` dead and
bounded automatic compaction still active.

- Profile the random 50% relocation case. It requires `393.62 ms` to compact,
  compared with `109.92 ms` when the same number of grown fields are clustered.
- Close the `22-36%` large-hash construction gap with a known-unique bulk build
  path after resolving duplicate fields once per command-sized HSET batch.
- Investigate the mixed-workload maximum latency, especially insert, delete,
  and grow operations while automatic compaction is active.
- Repeat the memory sweep with binary field/value shapes and a larger total
  footprint to verify the result under allocator and TLB pressure.

## Tested Servers

- Goblin Core development build containing the benchmarked HSET changes
- Redis 7.2.4
- Redis 8.8
- Valkey 9.1
- Dragonfly, configured with one proactor thread
