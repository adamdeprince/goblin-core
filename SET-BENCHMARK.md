# Goblin Core SET Benchmark

Generated on a quiet dedicated benchmark host at 2026-07-16 19:23:35 UTC.

## Summary

Goblin's RESP/TCP `SISMEMBER` reaches `2.44M` operations/s, `1.33x` the fastest incumbent; SBE over `2` MiB shared-memory rings per direction reaches `6.73M` operations/s. The populated `1,000,000`-member set costs `28.65` RSS-delta bytes/member over RESP versus `44.90` for the leanest incumbent.

## Method

- `1,000,000` distinct `16`-byte binary-safe members in one set.
- Population uses `SADD` batches of `128`; all timed rows use pipeline depth `256`.
- Goblin runs `GOBLIN.OPTIMIZE key 0.97` after population and before memory/operation measurements.
- Goblin Core is measured twice: RESP/TCP and typed SBE over `2` MiB request and reply rings. Every incumbent uses RESP/TCP.
- Fixed command rows use the median of `3` native C++ client rounds. Compound churn counts one `SREM` + `SADD` pair as one logical operation.
- The intersection row uses two equally sized sets with 50% overlap and returns only cardinality.
- Servers run one at a time on one serving core; Dragonfly uses one proactor and mini-redis-go uses `GOMAXPROCS=1`.
- Unsupported command cells are `n/a`; error replies are not timed.
- RSS is read from the launched PID. mini-redis-go uses `ps -o rss=`; other engines use `/proc/<pid>/status` as `VmRSS + HugetlbPages`. No server-reported RSS field is trusted.
- Redis-family servers are exercised as black boxes; their implementation source is not inspected.

## Population And Memory

| Engine | Transport | members/s | RSS MiB | RSS delta MiB | RSS delta B/member | key-reported B/member |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `goblin-resp-tcp` | `resp` | 2.25M | 33.46 | 27.32 | 28.65 | 28.16 |
| `goblin-sbe-ring` | `sbe` | 2.98M | 41.54 | 27.44 | 28.77 | 28.16 |
| `redis-7.2.4` | `resp` | 3.17M | 50.89 | 43.93 | 46.06 | 56.39 |
| `redis-8.8` | `resp` | 3.09M | 52.07 | 44.08 | 46.22 | 50.39 |
| `valkey-9.1` | `resp` | 2.96M | 53.68 | 45.99 | 48.23 | 50.89 |
| `dragonfly` | `resp` | 3.97M | 64.93 | 42.82 | 44.90 | 44.06 |
| `mini-redis-go-55178df` | `resp` | 1.38M | 136.02 | 124.77 | 130.83 | n/a |

## Operations

Logical operations per second.

| Operation | `goblin-resp-tcp` | `goblin-sbe-ring` | `redis-7.2.4` | `redis-8.8` | `valkey-9.1` | `dragonfly` | `mini-redis-go-55178df` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `SISMEMBER hit` | 2.44M | 6.73M | 1.83M | 1.56M | 1.49M | 1.19M | 152K |
| `SISMEMBER miss` | 1.60M | 2.74M | 1.80M | 1.57M | 1.57M | 1.18M | 152K |
| `SMISMEMBER four hits` | 1.22M | 3.66M | 1.04M | 909K | 895K | 734K | n/a |
| `SCARD` | 3.23M | 11.77M | 2.61M | 2.12M | 1.97M | 1.51M | 167K |
| `SREM + SADD` | 1.03M | 2.53M | 856K | 676K | 712K | 480K | 73K |
| `SINTERCARD, 50% overlap` | 13 | 13 | 7 | 6 | 18 | 3 | n/a |

## Tested Servers

- `goblin-resp-tcp`
- `goblin-sbe-ring`
- `redis-7.2.4`
- `redis-8.8`
- `valkey-9.1`
- `dragonfly`
- `mini-redis-go-55178df`
