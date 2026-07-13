# Goblin Core 100k List Benchmark

Generated on a dedicated benchmark host at 2026-07-12 20:22:22 UTC.

## Method

- `100,000` distinct `16`-byte values in one list.
- Load: multi-value `RPUSH` batches of `128`, pipeline depth `256`.
- Fixed-command rates: `200,000` requests, one client, pipeline depth `256`, median of `3` `redis-benchmark` runs.
- Compound rates use the repository's existing RESP pipeline client and keep the list length constant.
- Goblin implementation engines use their qualified command family (`goblin-pma` becomes `GOBLIN.PMA.*`); `goblin` exercises the selected standard aliases.
- Redis and Valkey use `benchmarks/redis-parity.conf`; Dragonfly uses one proactor thread for single-core parity. The intended target is a modest single-core memory server; the quiet benchmark host is a 64-core machine. Engines run one at a time.
- RSS is external `ps` RSS. `INFO used_memory` and `MEMORY USAGE` are reported independently; RSS/INFO deltas subtract the empty-server baseline.
- Per-key bytes are `MEMORY USAGE` on incumbents and `GOBLIN.MEMORY total_allocated_bytes` on Goblin Core.
- Incumbent implementations were treated strictly as black-box RESP servers;
  no incumbent source code was inspected.

## PMA tuning result

Before the final comparison, the same PMA binary was swept at three maximum
densities with 100,000 fixed-command requests and two timing rounds. Explicit
endpoint reservation made `0.97` workable: after compaction it retained 1,548
contiguous tail slots. Lower density spent more memory for little steady-state
queue improvement, so the default remains `0.97`.

| maximum density | load items/s | queue pairs/s | key bytes/item |
| ---: | ---: | ---: | ---: |
| `0.97` | 965,569 | 190,569 | 26.76 |
| `0.95` | 1,010,208 | 197,876 | 26.99 |
| `0.90` | 1,127,615 | 198,669 | 27.58 |

## Load

| Engine | items/s | seconds | RPUSH commands |
| --- | ---: | ---: | ---: |
| `goblin-pma` | 1,008,768 | 0.0991 | 782 |
| `redis-7.2.4` | 1,466,991 | 0.0682 | 782 |
| `redis-8.8` | 1,432,058 | 0.0698 | 782 |
| `valkey-9.1` | 1,422,831 | 0.0703 | 782 |
| `dragonfly` | 1,005,229 | 0.0995 | 782 |

## Operations

Logical operations per second. Fixed commands use the C load generator; compound rows count a two-command pair as one logical operation.

| Operation | goblin-pma | redis-7.2.4 | redis-8.8 | valkey-9.1 | dragonfly |
| --- | ---: | ---: | ---: | ---: | ---: |
| `LLEN control` | 3,033,212 | 2,502,400 | 2,085,333 | 1,819,927 | 1,472,000 |
| `LINDEX 0` | 2,669,226 | 1,962,667 | 1,771,611 | 1,450,667 | 1,220,683 |
| `LINDEX 25000` | 2,441,366 | 1,082,119 | 1,070,545 | 1,082,119 | 1,016,203 |
| `LINDEX 50000` | 2,599,896 | 901,766 | 639,591 | 820,459 | 844,692 |
| `LINDEX 75000` | 2,274,909 | 1,440,230 | 1,124,674 | 1,205,976 | 823,835 |
| `LINDEX -1` | 2,566,564 | 2,042,776 | 1,819,927 | 1,482,904 | 1,228,172 |
| `LRANGE 49992 50007` | 878,035 | 447,857 | 368,678 | 417,937 | 370,726 |
| `LSET 50000` | 1,627,577 | 905,846 | 594,042 | 772,942 | 761,186 |
| `LINSERT before middle pivot + LREM inserted value` | 2,259 | 613 | 812 | 653 | 822 |
| `LPUSH + LPOP` | 214,254 | 202,715 | 195,990 | 194,961 | 177,179 |
| `RPUSH + RPOP` | 206,107 | 211,736 | 198,869 | 198,861 | 178,823 |
| `LPUSH 8 values + LPOP count=8` | 56,101 | 61,914 | 61,597 | 59,913 | 53,703 |
| `RPUSH + LPOP` | 194,751 | 210,393 | 200,022 | 199,588 | 169,785 |

## Memory After Load

| Engine | RSS MiB | RSS delta MiB | RSS delta B/item | INFO used MiB | INFO delta B/item | key-reported B/item |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `goblin-pma` | 7.95 | 3.02 | 31.66 | 2.53 | 26.50 | 26.76 |
| `redis-7.2.4` | 10.97 | 2.04 | 21.34 | 2.93 | 21.15 | 20.33 |
| `redis-8.8` | 12.17 | 2.11 | 22.08 | 2.78 | 21.15 | 18.37 |
| `valkey-9.1` | 11.98 | 2.20 | 23.10 | 2.98 | 21.35 | 20.33 |
| `dragonfly` | 24.52 | 2.49 | 26.13 | 2.00 | 18.15 | 18.14 |

## Memory After Operations

| Engine | RSS MiB | INFO used MiB | key bytes/item | list length |
| --- | ---: | ---: | ---: | ---: |
| `goblin-pma` | 11.02 | 2.88 | 31.17 | 100000 |
| `redis-7.2.4` | 11.14 | 3.13 | 17.73 | 100000 |
| `redis-8.8` | 12.31 | 2.99 | 18.37 | 100000 |
| `valkey-9.1` | 12.25 | 3.16 | 17.73 | 100000 |
| `dragonfly` | 24.96 | 2.00 | 18.15 | 100000 |

## Goblin List Internals

| Phase | elements | live value MiB | dead value MiB | value alloc MiB | order capacity | front slack | back slack | order alloc MiB | total alloc MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `after load` | 100,000 | 1.53 | 0.00 | 1.55 | 103,093 | 0 | 1,548 | 1.00 | 2.55 |
| `after operations` | 100,000 | 1.53 | 0.17 | 1.78 | 122,599 | 8,671 | 2,981 | 1.19 | 2.97 |

## Binaries

- `goblin-pma`: `/home/adam/goblin-list-bench-20260712/build-release/goblin-core`
- `redis-7.2.4`: `/home/adam/bench/redis-7.2.4/src/redis-server`
- `redis-8.8`: `/home/adam/bench/redis-8.8.0/src/redis-server`
- `valkey-9.1`: `/home/adam/bench/valkey-9.1.0/src/valkey-server`
- `dragonfly`: `/home/adam/dragonfly/build-opt/dragonfly`
