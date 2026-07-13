# Goblin Core 100k List Benchmark

Incumbent results were generated on a dedicated benchmark host at 2026-07-13
21:37:04 UTC. Goblin Core columns were refreshed on the same host at 2026-07-13
22:29:27 UTC from commit `26aa20f`; incumbents were not rerun.

## Summary

`goblin-pma` leads `8` of `8` fixed-command rows, reaches `2.63x` the fastest incumbent on middle-list `LINDEX`, RESP population is `47.8%` behind the fastest incumbent, and uses `29.37` RSS-delta bytes/item. `goblin-segmented` leads `8` of `8` fixed-command rows, reaches `1.96x` the fastest incumbent on middle-list `LINDEX`, RESP population is `1.5%` behind the fastest incumbent, and uses `26.91` RSS-delta bytes/item. `goblin-segmented` is the leanest key representation at `17.16` accounted bytes/item versus `18.14` from the leanest incumbent's key-level counter. The leanest incumbent uses `21.38` RSS-delta bytes/item. RESP population and compound-operation rows use the Python RESP pipeline and are client-influenced; fixed-command rows use the C benchmark client.

## Method

- `100,000` distinct `16`-byte values in one list.
- RESP population: multi-value `RPUSH` batches of `128`, pipeline depth `256`.
- RESP population measures command ingestion, not Goblin Core native snapshot restoration; native restore reconstructs each list in one bulk operation and uses the compatible raw-copy accelerator when present.
- Fixed-command rates: `200,000` requests, one client, pipeline depth `256`, median of `3` `redis-benchmark` runs.
- Compound rates use the repository's existing RESP pipeline client and keep the list length constant.
- Goblin implementation engines use their qualified command family (`goblin-pma` becomes `GOBLIN.PMA.*`); `goblin` exercises the selected standard aliases.
- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. The intended target is a modest single-core memory server; the quiet benchmark host is a 64-core machine. Engines run one at a time.
- RSS is read directly from the launched server PID's Linux `/proc/<pid>/status` as `VmRSS + HugetlbPages`; no server-reported RSS field is used. `INFO used_memory` and `MEMORY USAGE` are independent corroborating counters; RSS/INFO deltas subtract the empty-server baseline.
- Per-key bytes are `MEMORY USAGE` on incumbents and `GOBLIN.MEMORY total_allocated_bytes` on Goblin Core.
- Incumbents are exercised strictly as black-box RESP servers; their source code is not inspected.

## RESP Population

| Engine | items/s | seconds | RPUSH commands |
| --- | ---: | ---: | ---: |
| `goblin-pma` | 792,638 | 0.1262 | 782 |
| `goblin-segmented` | 1,496,056 | 0.0668 | 782 |
| `redis-7.2.4` | 1,518,455 | 0.0659 | 782 |
| `redis-8.8` | 1,484,023 | 0.0674 | 782 |
| `valkey-9.1` | 1,471,200 | 0.0680 | 782 |
| `dragonfly` | 1,043,582 | 0.0958 | 782 |

## Operations

Logical operations per second. Fixed commands use the C benchmark client; compound rows count a two-command pair as one logical operation.

| Operation | goblin-pma | goblin-segmented | redis-7.2.4 | redis-8.8 | valkey-9.1 | dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `LLEN control` | 3,393,085 | 3,451,586 | 2,742,356 | 2,199,912 | 2,001,920 | 1,539,938 |
| `LINDEX 0` | 2,599,896 | 2,599,896 | 2,152,602 | 1,924,923 | 1,725,793 | 1,275,108 |
| `LINDEX 25000` | 2,441,366 | 2,199,912 | 1,157,179 | 1,082,119 | 1,112,178 | 1,048,126 |
| `LINDEX 50000` | 2,502,400 | 1,870,953 | 953,295 | 633,519 | 851,881 | 870,400 |
| `LINDEX 75000` | 2,327,814 | 1,654,479 | 1,429,943 | 1,163,907 | 1,213,285 | 817,110 |
| `LINDEX -1` | 2,566,564 | 2,383,238 | 2,199,912 | 1,906,590 | 1,614,452 | 1,259,069 |
| `LRANGE 49992 50007` | 722,715 | 736,000 | 467,738 | 394,079 | 432,380 | 380,593 |
| `LSET 50000` | 1,725,793 | 1,888,604 | 944,302 | 597,588 | 803,984 | 788,157 |
| `LINSERT before middle pivot + LREM inserted value` | 597 | 1,067 | 614 | 836 | 661 | 825 |
| `LPUSH + LPOP` | 213,631 | 99,890 | 211,222 | 195,770 | 198,126 | 181,130 |
| `RPUSH + RPOP` | 198,893 | 214,788 | 214,921 | 200,750 | 198,097 | 183,167 |
| `LPUSH 8 values + LPOP count=8` | 54,559 | 42,566 | 61,809 | 61,484 | 61,013 | 55,900 |
| `RPUSH + LPOP` | 177,961 | 186,698 | 212,496 | 200,565 | 199,914 | 173,611 |

## Memory After Population

| Engine | RSS MiB | RSS delta MiB | RSS delta B/item | INFO used MiB | INFO delta B/item | key-reported B/item |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin-pma` | 8.08 | 2.80 | 29.37 | 2.14 | 22.40 | 22.40 |
| `goblin-segmented` | 7.79 | 2.57 | 26.91 | 1.70 | 17.87 | 17.16 |
| `redis-7.2.4` | 10.97 | 2.04 | 21.38 | 2.93 | 21.15 | 20.33 |
| `redis-8.8` | 12.15 | 2.11 | 22.12 | 2.78 | 21.15 | 18.37 |
| `valkey-9.1` | 11.98 | 2.21 | 23.14 | 2.98 | 21.35 | 20.33 |
| `dragonfly` | 24.58 | 2.50 | 26.21 | 2.00 | 18.15 | 18.14 |

## Memory After Operations

| Engine | RSS MiB | INFO used MiB | key bytes/item | list length |
| --- | ---: | ---: | ---: | ---: |
| `goblin-pma` | 11.45 | 2.54 | 26.64 | 100000 |
| `goblin-segmented` | 7.93 | 1.77 | 17.22 | 100000 |
| `redis-7.2.4` | 11.16 | 3.13 | 17.73 | 100000 |
| `redis-8.8` | 12.28 | 2.99 | 18.37 | 100000 |
| `valkey-9.1` | 12.23 | 3.16 | 17.73 | 100000 |
| `dragonfly` | 25.04 | 2.00 | 18.15 | 100000 |

## Goblin List Internals

| Engine | representation | phase | elements | live value MiB | dead value MiB | value alloc MiB | slots/leaves | front slack | back slack | order alloc MiB | total alloc MiB |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin-pma` | `pma` | `after population` | 100,000 | 1.53 | 0.00 | 1.53 | 103,093 | 0 | 1,548 | 0.61 | 2.14 |
| `goblin-pma` | `pma` | `after operations` | 100,000 | 1.53 | 0.17 | 1.82 | 122,599 | 8,671 | 2,981 | 0.72 | 2.54 |
| `goblin-segmented` | `segmented` | `after population` | 100,000 | 1.63 | 0.00 | 1.63 | 782 | 0 | 0 | 0.01 | 1.64 |
| `goblin-segmented` | `segmented` | `after operations` | 100,000 | 1.63 | 0.00 | 1.63 | 782 | 0 | 0 | 0.01 | 1.64 |

## Tested Servers

- `goblin-pma`
- `goblin-segmented`
- `redis-7.2.4`
- `redis-8.8`
- `valkey-9.1`
- `dragonfly`
