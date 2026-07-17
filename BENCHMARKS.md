# Goblin Core ZSET Benchmark

Generated on a quiet dedicated benchmark host at 2026-07-16 19:23:35 UTC.

## Summary

After loading and packing `1,000,000` members, Goblin Core uses `52.32` RSS-delta bytes/member versus `55.26` for the leanest incumbent. RESP/TCP same-member `ZADD` reaches `1.06M` operations/s, `1.13x` the fastest incumbent; typed SBE over `2` MiB request and reply rings reaches `3.39M` operations/s.

## Method

- `1,000,000` distinct `16`-byte members in one sorted set, with deterministic scattered unsigned 32-bit integer scores.
- Population uses `ZADD` batches of `128`. Every timed operation uses pipeline depth `256`.
- Goblin runs `GOBLIN.OPTIMIZE key 0.97` after population and before memory/operation measurements.
- Goblin Core is measured over RESP/TCP and typed SBE over `2` MiB request and reply rings; every incumbent uses RESP/TCP.
- Fixed rows are medians of `3` native C++ client rounds. `ZREM + ZADD` counts as one logical churn operation.
- One serving core per engine. Dragonfly uses one proactor; mini-redis-go uses `GOMAXPROCS=1`.
- A server without sorted-set support remains in the matrix with `n/a` cells; error replies are not timed.
- RSS comes from the launched server PID (`ps` for mini-redis-go, `/proc` for the others). Server-reported RSS is not used.
- Incumbent implementations are black boxes; no incompatible source is inspected.

## Population And Memory

| Engine | Transport | members/s | RSS MiB | RSS delta MiB | RSS delta B/member | key-reported B/member |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | `resp` | 1.08M | 56.02 | 49.90 | 52.32 | 51.34 |
| `goblin-sbe-ring` | `sbe` | 1.42M | 64.22 | 50.07 | 52.51 | 51.34 |
| `redis-7.2.4` | `resp` | 925K | 111.64 | 104.68 | 109.76 | 99.59 |
| `redis-8.8` | `resp` | 905K | 86.84 | 78.85 | 82.68 | 95.73 |
| `valkey-9.1` | `resp` | 910K | 88.31 | 80.64 | 84.56 | 78.27 |
| `dragonfly` | `resp` | 1.24M | 74.77 | 52.70 | 55.26 | 54.30 |
| `mini-redis-go-55178df` | `resp` | n/a | n/a | n/a | n/a | n/a |

## Operations

Logical operations per second.

| Operation | `goblin-resp-tcp` | `goblin-sbe-ring` | `redis-7.2.4` | `redis-8.8` | `valkey-9.1` | `dragonfly` | `mini-redis-go-55178df` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `ZADD existing member` | 1.06M | 3.39M | 946K | 920K | 900K | 452K | n/a |
| `ZSCORE hit` | 1.70M | 3.77M | 1.64M | 1.34M | 1.45M | 1.07M | n/a |
| `ZRANK` | 1.08M | 1.61M | 809K | 765K | 797K | 663K | n/a |
| `ZRANGE first 16` | 740K | 2.80M | 575K | 560K | 504K | 380K | n/a |
| `ZCARD` | 2.96M | 9.01M | 2.40M | 1.99M | 1.87M | 1.46M | n/a |
| `ZREM + ZADD` | 495K | 760K | 424K | 372K | 386K | 323K | n/a |

## Interpretation

The RESP rows isolate the sorted-set implementation under the same wire protocol. The ring row answers a different deployment question: how much transport and parsing work remains when a native client submits the same pipelined operations as typed SBE messages without a syscall or ASCII conversion. It is reported alongside, never substituted for the protocol-parity result.

The scattered score generator crosses signed 32-bit range, so Goblin stores this population at `f64` score width. Workloads whose scores fit `i16` or signed `i32` can use less memory.

## Tested Servers

- `goblin-resp-tcp`
- `goblin-sbe-ring`
- `redis-7.2.4`
- `redis-8.8`
- `valkey-9.1`
- `dragonfly`
- `mini-redis-go-55178df`
