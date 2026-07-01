# Goblin Core Microbenchmarks

Generated: 2026-07-01 22:09:54 UTC.

These measurements isolate in-process read-path costs. They do not include sockets, polling, Redis comparison, or process RSS accounting.

Source data:

- `off`: `benchmark-results/microbench-1m-rank-cache-off-v2.json`
- `exact`: `benchmark-results/microbench-1m-rank-cache-exact-v2.json`
- `block-hint`: `benchmark-results/microbench-1m-rank-cache-block-hint-v2.json`

## Configuration

| Setting | `off` | `exact` | `block-hint` |
| --- | ---: | ---: | ---: |
| `members` | `1000000` | `1000000` | `1000000` |
| `ops` | `1000000` | `1000000` | `1000000` |
| `range_size` | `16` | `16` | `16` |
| `warmups` | `1` | `1` | `1` |
| `seed` | `12345` | `12345` | `12345` |
| `score_shape` | `integer` | `integer` | `integer` |
| `rank_cache_mode` | `off` | `exact` | `block-hint` |
| `score_string_cache` | `false` | `false` | `false` |

## Rank Cache `off`

| Metric | Raw ZSet ns/op | RESP ns/op | Command into ns/op | Command string ns/op | Parse into ns/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZSCORE` | 75.60 | 29.16 | 155.23 | 155.34 | 191.88 |
| `ZRANK` | 376.51 | 25.32 | 435.44 | 434.07 | 432.97 |
| `ZREVRANK` | 372.07 | 25.32 | 439.19 | 429.88 | 431.71 |
| `ZRANGE` | 242.44 | 275.77 | 468.14 | 500.83 | 482.04 |
| `ZRANGE WITHSCORES` | 242.44 | 546.62 | 918.88 | 941.54 | 920.92 |

## Rank Cache `exact`

| Metric | Raw ZSet ns/op | RESP ns/op | Command into ns/op | Command string ns/op | Parse into ns/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZSCORE` | 75.39 | 29.37 | 159.57 | 158.10 | 196.74 |
| `ZRANK` | 200.08 | 24.14 | 274.23 | 268.63 | 271.48 |
| `ZREVRANK` | 205.53 | 24.14 | 275.93 | 267.30 | 277.07 |
| `ZRANGE` | 244.69 | 273.06 | 486.58 | 495.86 | 500.84 |
| `ZRANGE WITHSCORES` | 244.69 | 572.88 | 935.96 | 953.69 | 939.20 |

## Rank Cache `block-hint`

| Metric | Raw ZSet ns/op | RESP ns/op | Command into ns/op | Command string ns/op | Parse into ns/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZSCORE` | 76.59 | 29.46 | 159.72 | 158.90 | 200.89 |
| `ZRANK` | 340.88 | 25.38 | 399.41 | 400.19 | 404.27 |
| `ZREVRANK` | 344.41 | 25.38 | 400.29 | 403.49 | 402.06 |
| `ZRANGE` | 238.07 | 279.51 | 480.84 | 507.60 | 485.55 |
| `ZRANGE WITHSCORES` | 238.07 | 574.95 | 937.35 | 947.99 | 931.02 |

## Rank Cache Effect

| Metric | off ns/op | exact ns/op | block-hint ns/op | exact vs off | block-hint vs off |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Raw ZSCORE` | 75.60 | 75.39 | 76.59 | -0.3% | 1.3% |
| `Raw ZRANK` | 376.51 | 200.08 | 340.88 | -46.9% | -9.5% |
| `Raw ZREVRANK` | 372.07 | 205.53 | 344.41 | -44.8% | -7.4% |
| `Raw ZRANGE iter` | 242.44 | 244.69 | 238.07 | 0.9% | -1.8% |
| `Command-into ZSCORE` | 155.23 | 159.57 | 159.72 | 2.8% | 2.9% |
| `Command-into ZRANK` | 435.44 | 274.23 | 399.41 | -37.0% | -8.3% |
| `Command-into ZREVRANK` | 439.19 | 275.93 | 400.29 | -37.2% | -8.9% |
| `Command-into ZRANGE` | 468.14 | 486.58 | 480.84 | 3.9% | 2.7% |

## ZRANGE Serialization Breakdown

| Component | off ns/op | exact ns/op | block-hint ns/op |
| --- | ---: | ---: | ---: |
| `Score-index traversal` | 49.48 | 49.07 | 51.31 |
| `Member lookup` | 134.46 | 137.62 | 138.56 |
| `Score formatting only` | 71.24 | 68.84 | 65.67 |
| `Member RESP append only` | 429.65 | 436.63 | 434.49 |
| `Score RESP append preformatted` | 297.64 | 298.65 | 300.83 |
| `Score RESP append formatting` | 181.14 | 181.76 | 177.96 |
| `Direct RESP append WITHSCORES` | 737.93 | 767.69 | 755.57 |
| `RESP append` | 247.54 | 245.71 | 249.33 |
| `RESP append WITHSCORES` | 501.92 | 532.76 | 517.82 |
| `Full command into` | 466.92 | 478.95 | 472.19 |
| `Full command into WITHSCORES` | 917.07 | 930.34 | 915.52 |

## Read-Path Notes

- `ZRANGE` response construction is the visible in-process cost: RESP-only serialization is 275.77 ns/op versus 242.44 ns/op for raw range iteration.
- `ZRANGE` breakdown separates score-index traversal, member lookup, and RESP append costs: 49.48 ns/op traversal, 134.46 ns/op member lookup, and 247.54 ns/op RESP append.
- `WITHSCORES` adds double formatting work in the RESP append path: 501.92 ns/op versus 247.54 ns/op without scores.
- Score formatting and score bulk append are now separated: 71.24 ns/op for formatting only and 181.14 ns/op for score bulk append with formatting.
- `ZRANGE` command-into versus string-wrapper cost should be read as a server-path check, not a guaranteed microbench win: command-into is 468.14 ns/op versus 500.83 ns/op for the string wrapper.
- `ZSCORE` command overhead over raw lookup is about 79.63 ns/op.
- `ZRANK` remains mostly data-structure work in-process: raw rank is 376.51 ns/op versus 435.44 ns/op for command execution.
