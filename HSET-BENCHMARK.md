# Goblin Core HSET Speed and Memory Benchmark

Generated on `naamah` at 2026-07-12 21:27:16 UTC.

## Method

- Every scenario starts a fresh server; engines run one at a time.
- RSS is `INFO memory`'s `used_memory_rss` before and after the workload. The naamah builds read the same live `/proc` resident-set field and do not use a cached value.
- Application memory is the independent `INFO used_memory` delta. Per-key memory is `MEMORY USAGE` on incumbents and `GOBLIN.MEMORY total_allocated_bytes` on Goblin Core. These are engine-specific corroborating counters; RSS is the cross-engine comparison.
- Bulk loads use multi-field `HSET` batches of `128` and pipeline depth `256` through the shared Python RESP harness; load timing includes client encoding and is reported as observed end-to-end throughput. Point-update rates use one `redis-benchmark` client at pipeline depth `256`, `500,000` requests, median of `3` runs.
- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. The target is a modest single-core memory server; naamah is a quiet 64-core test machine.
- This dated run used Goblin's former 32-field compact-hash cutoff. The current default promotes at the 64 KiB compact-blob boundary instead. The parity config keeps Redis and Valkey listpacked through 128 fields, so the many-hash sweep measures the engines' configured representation choices, not matched internal encodings.
- Incumbents are exercised strictly as black-box RESP servers; their source code is not inspected.

## One Large Hash

`1,000,000` distinct `18`-byte fields start with `16`-byte values. Existing-field HSET uses the load generator's `12`-digit random token. A deterministic pass then grows every value to `64` bytes.

### Speed

| Engine | bulk new fields/s | same-width HSET ops/s | bulk grow fields/s | initial optimize (s) | grown optimize (s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 579,772 | 1,593,070 | 621,521 | 0.1780 | 0.3571 |
| `redis-7.2.4` | 654,932 | 1,202,462 | 625,960 | n/a | n/a |
| `redis-8.8` | 683,149 | 1,080,397 | 641,357 | n/a | n/a |
| `valkey-9.1` | 659,714 | 1,066,576 | 642,586 | n/a | n/a |
| `dragonfly` | 692,707 | 735,624 | 571,394 | n/a | n/a |

### Memory With 16-Byte Values

Goblin's row is sampled after `GOBLIN.OPTIMIZE`; the pre-optimize RSS is retained in the raw JSON.

| Engine | RSS MiB | RSS delta MiB | RSS B/field | used B/field | key B/field |
| --- | ---: | ---: | ---: | ---: | ---: |
| `goblin` | 54.46 | 49.44 | 51.84 | 39.15 | 51.19 |
| `redis-7.2.4` | 93.57 | 84.69 | 88.81 | 80.51 | 80.39 |
| `redis-8.8` | 66.50 | 56.46 | 59.21 | 54.20 | 72.39 |
| `valkey-9.1` | 70.91 | 61.20 | 64.17 | 67.03 | 66.88 |
| `dragonfly` | 95.41 | 73.17 | 76.72 | 76.07 | 76.07 |

### Memory After Growing Every Value to 64 Bytes

Pre-optimize captures update fragmentation. Post-optimize is the equal-data steady state; non-Goblin engines perform no intervening maintenance operation.

| Engine | pre-opt RSS B/field | post-opt RSS B/field | post-opt used B/field | post-opt key B/field |
| --- | ---: | ---: | ---: | ---: |
| `goblin` | 120.46 | 99.87 | 87.15 | 99.18 |
| `redis-7.2.4` | 166.44 | 166.44 | 136.54 | 136.39 |
| `redis-8.8` | 121.33 | 121.34 | 110.23 | 120.39 |
| `valkey-9.1` | 124.13 | 124.14 | 113.62 | 113.47 |
| `dragonfly` | 157.25 | 157.25 | 124.07 | 124.07 |

### Goblin Large-Hash Internals

| Phase | fields | live MiB | dead MiB | arena MiB | index MiB | total MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `loaded before optimize` | 1,000,000 | 32.42 | 0.00 | 45.16 | 5.56 | 50.71 |
| `loaded after optimize` | 1,000,000 | 32.42 | 0.00 | 43.91 | 4.92 | 48.82 |
| `grown before optimize` | 1,000,000 | 78.20 | 15.26 | 107.07 | 5.85 | 112.92 |
| `grown after optimize` | 1,000,000 | 78.20 | 0.00 | 89.67 | 4.92 | 94.59 |

## Many Hashes

Each row holds approximately `500,000` total fields while varying fields per hash. Keys are `14` bytes, fields are `5` bytes, and values are `16` bytes. The update targets the middle field of random existing hashes.

### Speed

| fields/hash | hashes | Engine | load fields/s | load hashes/s | middle-field HSET ops/s |
| ---: | ---: | --- | ---: | ---: | ---: |
| 8 | 62,500 | `goblin` | 666,526 | 83,316 | 1,780,157 |
| 8 | 62,500 | `redis-7.2.4` | 653,255 | 81,657 | 1,573,032 |
| 8 | 62,500 | `redis-8.8` | 648,695 | 81,087 | 1,269,604 |
| 8 | 62,500 | `valkey-9.1` | 623,768 | 77,971 | 1,279,345 |
| 8 | 62,500 | `dragonfly` | 657,484 | 82,186 | 1,000,448 |
| 32 | 15,625 | `goblin` | 752,180 | 23,506 | 1,718,983 |
| 32 | 15,625 | `redis-7.2.4` | 731,972 | 22,874 | 1,425,140 |
| 32 | 15,625 | `redis-8.8` | 739,124 | 23,098 | 1,157,926 |
| 32 | 15,625 | `valkey-9.1` | 718,710 | 22,460 | 1,217,090 |
| 32 | 15,625 | `dragonfly` | 741,809 | 23,182 | 936,749 |
| 64 | 7,812 | `goblin` | 785,668 | 12,276 | 1,887,638 |
| 64 | 7,812 | `redis-7.2.4` | 705,745 | 11,027 | 1,196,708 |
| 64 | 7,812 | `redis-8.8` | 710,061 | 11,095 | 1,078,069 |
| 64 | 7,812 | `valkey-9.1` | 705,452 | 11,023 | 1,085,085 |
| 64 | 7,812 | `dragonfly` | 772,479 | 12,070 | 907,848 |
| 128 | 3,906 | `goblin` | 828,965 | 6,476 | 1,931,367 |
| 128 | 3,906 | `redis-7.2.4` | 615,185 | 4,806 | 942,041 |
| 128 | 3,906 | `redis-8.8` | 656,041 | 5,125 | 912,818 |
| 128 | 3,906 | `valkey-9.1` | 647,447 | 5,058 | 882,229 |
| 128 | 3,906 | `dragonfly` | 813,159 | 6,353 | 912,818 |

### Memory

Sampled key bytes are the mean of the first, middle, and last hash.

| fields/hash | Engine | RSS MiB | RSS B/hash | RSS B/field | used B/hash | sampled key B/hash |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 8 | `goblin` | 26.69 | 363.79 | 45.47 | 247.20 | 208.00 |
| 8 | `redis-7.2.4` | 26.33 | 291.83 | 36.48 | 289.99 | 280.00 |
| 8 | `redis-8.8` | 26.95 | 284.69 | 35.59 | 279.72 | 247.00 |
| 8 | `valkey-9.1` | 26.98 | 288.95 | 36.12 | 283.39 | 264.00 |
| 8 | `dragonfly` | 39.89 | 297.27 | 37.16 | 286.93 | 224.00 |
| 32 | `goblin` | 22.33 | 1161.56 | 36.30 | 847.19 | 808.00 |
| 32 | `redis-7.2.4` | 23.40 | 970.72 | 30.33 | 966.77 | 952.00 |
| 32 | `redis-8.8` | 24.57 | 976.22 | 30.51 | 956.47 | 847.00 |
| 32 | `valkey-9.1` | 24.36 | 979.37 | 30.61 | 961.17 | 936.00 |
| 32 | `dragonfly` | 36.80 | 978.06 | 30.56 | 946.34 | 896.00 |
| 64 | `goblin` | 29.42 | 3278.06 | 51.22 | 1,799.18 | 2,600.00 |
| 64 | `redis-7.2.4` | 22.92 | 1877.07 | 29.33 | 1,869.16 | 1,848.00 |
| 64 | `redis-8.8` | 24.19 | 1900.67 | 29.70 | 1,858.86 | 1,647.00 |
| 64 | `valkey-9.1` | 23.99 | 1900.14 | 29.69 | 1,864.97 | 1,832.00 |
| 64 | `dragonfly` | 51.33 | 3911.44 | 61.12 | 3,857.56 | 3,824.00 |
| 128 | `goblin` | 27.15 | 5941.61 | 46.42 | 3,543.14 | 5,165.00 |
| 128 | `redis-7.2.4` | 22.71 | 3698.56 | 28.90 | 3,673.93 | 3,640.00 |
| 128 | `redis-8.8` | 23.98 | 3745.75 | 29.26 | 3,663.54 | 3,247.00 |
| 128 | `valkey-9.1` | 23.73 | 3748.90 | 29.29 | 3,672.31 | 3,624.00 |
| 128 | `dragonfly` | 50.60 | 7616.29 | 59.50 | 7,520.00 | 7,520.00 |

## Binaries

- `goblin`: `build-release/goblin-core`
- `redis-7.2.4`: `/home/adam/bench/redis-7.2.4/src/redis-server`
- `redis-8.8`: `/home/adam/bench/redis-8.8.0/src/redis-server`
- `valkey-9.1`: `/home/adam/bench/valkey-9.1.0/src/valkey-server`
- `dragonfly`: `/home/adam/dragonfly/build-opt/dragonfly`

## Indexed Compact Follow-up

The compact hash now carries a SIMD-scanned fingerprint and 16-bit entry offset
per field, and the full arena stores an 8-byte base reference with value
relocations in an optional sidecar. The 20-point
[indexed compact threshold sweep](HASH-THRESHOLD-SWEEP.md) measures the revised
implementation against forced-full Goblin and every incumbent from 32 through
the natural 64 KiB promotion boundary and 2048 fields per hash.
