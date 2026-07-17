# Goblin Core 100k List Benchmark

Generated on a dedicated benchmark host at 2026-07-16 19:21:14 UTC.

## Summary

`goblin-resp-tcp` leads `8` of `8` fixed-command rows, reaches `1.99x` the fastest incumbent on middle-list `LINDEX`, population is `1.4%` behind the fastest incumbent, and uses `25.35` RSS-delta bytes/item. `goblin-sbe-ring` leads `8` of `8` fixed-command rows, reaches `3.29x` the fastest incumbent on middle-list `LINDEX`, population is `1475.1%` ahead of the fastest incumbent, and uses `26.05` RSS-delta bytes/item. `goblin-pma-resp-tcp` leads `8` of `8` fixed-command rows, reaches `2.52x` the fastest incumbent on middle-list `LINDEX`, population is `46.7%` behind the fastest incumbent, and uses `27.85` RSS-delta bytes/item. `goblin-resp-tcp` is the leanest key representation at `17.16` accounted bytes/item versus `18.14` from the leanest incumbent's key-level counter. The leanest incumbent uses `21.42` RSS-delta bytes/item. TCP compound rows use the Python RESP pipeline; ring rows use the native C++ SBE client at the same pipeline depth. Fixed TCP rows use `redis-benchmark`.

## Method

- `100,000` distinct `16`-byte values in one list.
- Population: multi-value `RPUSH` batches of `128`. RESP/TCP and SBE/ring both use pipeline depth `256`.
- Population measures command ingestion, not Goblin Core native snapshot restoration; native restore reconstructs each list in one bulk operation and uses the compatible raw-copy accelerator when present.
- Fixed-command rates: `200,000` requests and one client. RESP/TCP uses pipeline depth `256` and the median of `3` `redis-benchmark` runs. SBE/ring uses the median of `3` native C++ runs at the same pipeline depth.
- Compound rates keep the list length constant. TCP engines use the existing RESP pipeline client; the ring row uses the native typed SBE client.
- Transport matrix: Goblin Core is measured as `goblin-resp-tcp` and `goblin-sbe-ring`; every incumbent is measured over RESP/TCP. No UDS row is included. The ring row uses a `2mb` request and reply ring.
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
| `goblin-resp-tcp` | 1,472,900 | 0.0679 | 782 |
| `goblin-sbe-ring` | 23,519,832 | 0.0043 | 782 |
| `goblin-pma-resp-tcp` | 796,505 | 0.1255 | 782 |
| `redis-7.2.4` | 1,493,261 | 0.0670 | 782 |
| `redis-8.8` | 1,470,975 | 0.0680 | 782 |
| `valkey-9.1` | 1,444,738 | 0.0692 | 782 |
| `dragonfly` | 1,053,633 | 0.0949 | 782 |
| `mini-redis-go-55178df` | 941,496 | 0.1062 | 782 |

## Operations

Logical operations per second. Fixed TCP commands use `redis-benchmark`; ring commands use the native C++ SBE client. Compound rows count a two-command pair as one logical operation.

| Operation | goblin-resp-tcp | goblin-sbe-ring | goblin-pma-resp-tcp | redis-7.2.4 | redis-8.8 | valkey-9.1 | dragonfly | mini-redis-go-55178df |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `LLEN control` | 3,574,857 | 11,396,967 | 3,228,903 | 2,780,444 | 2,249,348 | 2,063,835 | 1,493,970 | 88,856 |
| `LINDEX 0` | 2,819,605 | 6,441,440 | 2,502,400 | 2,224,356 | 1,962,667 | 1,711,043 | 1,243,429 | n/a |
| `LINDEX 25000` | 2,274,909 | 4,256,173 | 2,383,238 | 1,177,600 | 1,118,391 | 1,131,028 | 1,037,264 | n/a |
| `LINDEX 50000` | 1,924,923 | 3,178,220 | 2,441,366 | 967,111 | 643,704 | 855,521 | 866,632 | n/a |
| `LINDEX 75000` | 1,668,267 | 2,533,871 | 2,327,814 | 1,493,970 | 1,228,172 | 1,220,683 | 848,271 | n/a |
| `LINDEX -1` | 2,441,366 | 4,742,012 | 2,411,952 | 2,249,348 | 1,943,612 | 1,711,043 | 1,228,172 | n/a |
| `LRANGE 49992 50007` | 775,938 | 1,303,508 | 741,452 | 464,483 | 397,996 | 441,925 | 303,782 | 70,218 |
| `LSET 50000` | 1,924,923 | 3,555,317 | 1,711,043 | 957,856 | 606,642 | 813,789 | 785,067 | n/a |
| `LINSERT before middle pivot + LREM inserted value` | 1,076 | 1,081 | 597 | 617 | 836 | 663 | 844 | n/a |
| `LPUSH + LPOP` | 97,398 | 168,053 | 215,068 | 211,504 | 199,897 | 199,906 | 183,160 | 543 |
| `RPUSH + RPOP` | 209,017 | 2,280,664 | 203,009 | 214,977 | 202,695 | 202,217 | 184,556 | 40,215 |
| `LPUSH 8 values + LPOP count=8` | 42,464 | 150,074 | 56,313 | 62,960 | 62,256 | 62,309 | 56,682 | n/a |
| `RPUSH + LPOP` | 183,026 | 866,641 | 178,127 | 212,553 | 200,662 | 200,597 | 174,115 | 40,843 |

## Memory After Population

| Engine | RSS MiB | RSS delta MiB | RSS delta B/item | INFO used MiB | INFO delta B/item | key-reported B/item |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | 8.55 | 2.42 | 25.35 | 1.70 | 17.87 | 17.16 |
| `goblin-sbe-ring` | 16.63 | 2.48 | 26.05 | 1.70 | 17.87 | 17.16 |
| `goblin-pma-resp-tcp` | 8.79 | 2.66 | 27.85 | 2.14 | 22.40 | 22.40 |
| `redis-7.2.4` | 8.95 | 2.04 | 21.42 | 2.93 | 21.15 | 20.33 |
| `redis-8.8` | 10.13 | 2.11 | 22.08 | 2.78 | 21.15 | 18.37 |
| `valkey-9.1` | 9.94 | 2.20 | 23.10 | 2.98 | 21.35 | 20.33 |
| `dragonfly` | 24.70 | 2.46 | 25.76 | 2.00 | 18.15 | 18.14 |
| `mini-redis-go-55178df` | 25.28 | 14.00 | 146.84 | n/a | n/a | n/a |

## Memory After Operations

| Engine | RSS MiB | INFO used MiB | key bytes/item | list length |
| --- | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | 8.69 | 1.77 | 17.22 | 100000 |
| `goblin-sbe-ring` | 16.68 | 1.77 | 17.22 | 100000 |
| `goblin-pma-resp-tcp` | 12.08 | 2.54 | 26.64 | 100000 |
| `redis-7.2.4` | 9.10 | 3.13 | 17.73 | 100000 |
| `redis-8.8` | 10.26 | 2.99 | 18.37 | 100000 |
| `valkey-9.1` | 10.19 | 3.16 | 17.73 | 100000 |
| `dragonfly` | 25.20 | 2.00 | 18.15 | 100000 |
| `mini-redis-go-55178df` | 23.20 | n/a | n/a | 100000 |

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
- `mini-redis-go-55178df`
