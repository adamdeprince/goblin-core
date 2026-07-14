# Goblin Core 100k List Benchmark

Generated on a dedicated benchmark host at 2026-07-14 04:21:51 UTC.

## Summary

`goblin-resp-tcp` leads `8` of `8` fixed-command rows, reaches `2.04x` the fastest incumbent on middle-list `LINDEX`, population is `0.2%` behind the fastest incumbent, and uses `26.87` RSS-delta bytes/item. `goblin-sbe-ring` leads `8` of `8` fixed-command rows, reaches `2.60x` the fastest incumbent on middle-list `LINDEX`, population is `792.1%` ahead of the fastest incumbent, and uses `27.03` RSS-delta bytes/item. `goblin-pma-resp-tcp` leads `8` of `8` fixed-command rows, reaches `2.78x` the fastest incumbent on middle-list `LINDEX`, population is `47.6%` behind the fastest incumbent, and uses `29.41` RSS-delta bytes/item. `goblin-resp-tcp` is the leanest key representation at `17.16` accounted bytes/item versus `18.14` from the leanest incumbent's key-level counter. The leanest incumbent uses `21.42` RSS-delta bytes/item. TCP compound rows use the Python RESP pipeline; ring rows use the native C++ SBE client with one request outstanding. Fixed TCP rows use `redis-benchmark`.

## Method

- `100,000` distinct `16`-byte values in one list.
- Population: multi-value `RPUSH` batches of `128`. RESP/TCP uses pipeline depth `256`; SBE/ring currently keeps one request outstanding.
- Population measures command ingestion, not Goblin Core native snapshot restoration; native restore reconstructs each list in one bulk operation and uses the compatible raw-copy accelerator when present.
- Fixed-command rates: `200,000` requests and one client. RESP/TCP uses pipeline depth `256` and the median of `3` `redis-benchmark` runs. SBE/ring uses the median of `3` native C++ runs with one request outstanding.
- Compound rates keep the list length constant. TCP engines use the existing RESP pipeline client; the ring row uses the native typed SBE client.
- Transport matrix: Goblin Core is measured as `goblin-resp-tcp` and `goblin-sbe-ring`; every incumbent is measured over RESP/TCP. No UDS row is included.
- Linux affinity: server core `2`, client/load-generator core `3` (`-1` means unpinned). Ring client and server must not share a core.
- Goblin implementation engines use their qualified command family (`goblin-pma` becomes `GOBLIN.PMA.*`); `goblin` exercises the selected standard aliases.
- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. The intended target is a modest single-core memory server; the quiet benchmark host is a 64-core machine. Engines run one at a time.
- mini-redis-go runs as an external TCP server with `GOMAXPROCS=1`, AOF disabled, and metrics disabled. Unsupported commands are shown as `n/a`, not timed error replies.
- RSS is read from the launched server PID: mini-redis-go uses `ps -o rss=`, while the other engines use Linux `/proc/<pid>/status` as `VmRSS + HugetlbPages`. No server-reported RSS field is used. `INFO used_memory` and `MEMORY USAGE` are independent corroborating counters; RSS/INFO deltas subtract the empty-server baseline.
- Per-key bytes are `MEMORY USAGE` where an incumbent exposes it and `GOBLIN.MEMORY total_allocated_bytes` on Goblin Core. mini-redis-go exposes neither `INFO memory` nor `MEMORY USAGE`, so its RSS is the cross-engine memory measurement.
- Incumbents are exercised strictly as black-box RESP servers; their source code is not inspected.

## Population

| Engine | items/s | seconds | RPUSH commands |
| --- | ---: | ---: | ---: |
| `goblin-resp-tcp` | 1,523,291 | 0.0656 | 782 |
| `goblin-sbe-ring` | 13,617,171 | 0.0073 | 782 |
| `goblin-pma-resp-tcp` | 799,451 | 0.1251 | 782 |
| `redis-7.2.4` | 1,526,398 | 0.0655 | 782 |
| `redis-8.8` | 1,515,778 | 0.0660 | 782 |
| `valkey-9.1` | 1,482,830 | 0.0674 | 782 |
| `dragonfly` | 1,057,086 | 0.0946 | 782 |
| `mini-redis-go-74d87c0` | 1,020,297 | 0.0980 | 782 |

## Operations

Logical operations per second. Fixed TCP commands use `redis-benchmark`; ring commands use the native C++ SBE client. Compound rows count a two-command pair as one logical operation.

| Operation | goblin-resp-tcp | goblin-sbe-ring | goblin-pma-resp-tcp | redis-7.2.4 | redis-8.8 | valkey-9.1 | dragonfly | mini-redis-go-74d87c0 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `LLEN control` | 3,707,259 | 5,848,532 | 3,574,857 | 2,819,605 | 2,249,348 | 2,107,284 | 1,505,203 | 87,765 |
| `LINDEX 0` | 2,901,334 | 4,208,051 | 2,780,444 | 2,224,356 | 2,001,920 | 1,771,611 | 1,251,200 | n/a |
| `LINDEX 25000` | 2,355,200 | 3,149,492 | 2,634,105 | 1,213,285 | 1,118,391 | 1,143,954 | 1,048,126 | n/a |
| `LINDEX 50000` | 1,982,099 | 2,525,625 | 2,705,297 | 971,806 | 649,974 | 862,897 | 878,035 | n/a |
| `LINDEX 75000` | 1,711,043 | 2,016,582 | 2,566,564 | 1,505,203 | 1,251,200 | 1,243,429 | 851,881 | n/a |
| `LINDEX -1` | 2,502,400 | 3,430,708 | 2,669,226 | 2,274,909 | 2,001,920 | 1,740,800 | 1,243,429 | n/a |
| `LRANGE 49992 50007` | 752,602 | 869,951 | 736,000 | 472,151 | 401,186 | 439,982 | 307,043 | 69,175 |
| `LSET 50000` | 1,982,099 | 2,510,397 | 1,787,428 | 957,856 | 608,486 | 820,459 | 791,273 | n/a |
| `LINSERT before middle pivot + LREM inserted value` | 1,077 | 1,080 | 601 | 616 | 837 | 662 | 847 | n/a |
| `LPUSH + LPOP` | 99,286 | 159,977 | 216,911 | 211,963 | 201,033 | 198,499 | 181,859 | 930 |
| `RPUSH + RPOP` | 213,895 | 1,449,800 | 206,369 | 212,476 | 203,495 | 200,139 | 183,180 | 40,374 |
| `LPUSH 8 values + LPOP count=8` | 42,428 | 136,823 | 55,295 | 61,812 | 61,857 | 61,104 | 56,698 | n/a |
| `RPUSH + LPOP` | 183,123 | 707,149 | 179,007 | 212,587 | 198,631 | 198,107 | 173,747 | 39,928 |

## Memory After Population

| Engine | RSS MiB | RSS delta MiB | RSS delta B/item | INFO used MiB | INFO delta B/item | key-reported B/item |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | 7.79 | 2.56 | 26.87 | 1.70 | 17.87 | 17.16 |
| `goblin-sbe-ring` | 11.88 | 2.58 | 27.03 | 1.70 | 17.87 | 17.16 |
| `goblin-pma-resp-tcp` | 8.08 | 2.80 | 29.41 | 2.14 | 22.40 | 22.40 |
| `redis-7.2.4` | 8.88 | 2.04 | 21.42 | 2.93 | 21.15 | 20.33 |
| `redis-8.8` | 10.17 | 2.11 | 22.12 | 2.78 | 21.15 | 18.37 |
| `valkey-9.1` | 9.96 | 2.20 | 23.10 | 2.99 | 21.35 | 20.33 |
| `dragonfly` | 24.68 | 2.48 | 26.01 | 2.00 | 18.15 | 18.14 |
| `mini-redis-go-74d87c0` | 23.45 | 12.46 | 130.70 | n/a | n/a | n/a |

## Memory After Operations

| Engine | RSS MiB | INFO used MiB | key bytes/item | list length |
| --- | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | 7.93 | 1.77 | 17.22 | 100000 |
| `goblin-sbe-ring` | 11.91 | 1.77 | 17.22 | 100000 |
| `goblin-pma-resp-tcp` | 10.91 | 2.54 | 26.64 | 100000 |
| `redis-7.2.4` | 9.04 | 3.13 | 17.73 | 100000 |
| `redis-8.8` | 10.32 | 2.99 | 18.37 | 100000 |
| `valkey-9.1` | 10.21 | 3.16 | 17.73 | 100000 |
| `dragonfly` | 25.15 | 2.00 | 18.15 | 100000 |
| `mini-redis-go-74d87c0` | 23.32 | n/a | n/a | 100000 |

## Goblin List Internals

| Engine | representation | phase | elements | live value MiB | dead value MiB | value alloc MiB | slots/leaves | front slack | back slack | order alloc MiB | total alloc MiB |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | `segmented` | `after population` | 100,000 | 1.63 | 0.00 | 1.63 | 782 | 0 | 0 | 0.01 | 1.64 |
| `goblin-resp-tcp` | `segmented` | `after operations` | 100,000 | 1.63 | 0.00 | 1.63 | 782 | 0 | 0 | 0.01 | 1.64 |
| `goblin-sbe-ring` | `segmented` | `after population` | 100,000 | 1.63 | 0.00 | 1.63 | 782 | 0 | 0 | 0.01 | 1.64 |
| `goblin-sbe-ring` | `segmented` | `after operations` | 100,000 | 1.63 | 0.00 | 1.63 | 782 | 0 | 0 | 0.01 | 1.64 |
| `goblin-pma-resp-tcp` | `pma` | `after population` | 100,000 | 1.53 | 0.00 | 1.53 | 103,093 | 0 | 1,548 | 0.61 | 2.14 |
| `goblin-pma-resp-tcp` | `pma` | `after operations` | 100,000 | 1.53 | 0.17 | 1.82 | 122,599 | 8,671 | 2,981 | 0.72 | 2.54 |

## Tested Servers

- `goblin-resp-tcp`
- `goblin-sbe-ring`
- `goblin-pma-resp-tcp`
- `redis-7.2.4`
- `redis-8.8`
- `valkey-9.1`
- `dragonfly`
- `mini-redis-go-74d87c0`
