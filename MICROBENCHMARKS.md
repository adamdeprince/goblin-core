# Goblin Core Microbenchmarks

Generated: 2026-07-02 01:05:30 UTC.

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
| `ZSCORE` | 103.44 | 34.95 | 177.94 | 184.77 | 238.92 |
| `ZRANK` | 338.34 | 24.67 | 385.36 | 470.87 | 451.27 |
| `ZREVRANK` | 465.05 | 24.67 | 404.34 | 455.06 | 490.36 |
| `ZRANGE` | 287.70 | 410.07 | 541.31 | 714.15 | 624.93 |
| `ZRANGE WITHSCORES` | 287.70 | 684.42 | 1,189.78 | 1,462.15 | 1,402.94 |

## Rank Cache Effect

| Metric | off ns/op | exact ns/op | block-hint ns/op | exact vs off | block-hint vs off |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Raw ZSCORE` | 75.60 | 75.39 | 103.44 | -0.3% | 36.8% |
| `Raw ZRANK` | 376.51 | 200.08 | 338.34 | -46.9% | -10.1% |
| `Raw ZREVRANK` | 372.07 | 205.53 | 465.05 | -44.8% | 25.0% |
| `Raw ZRANGE iter` | 242.44 | 244.69 | 287.70 | 0.9% | 18.7% |
| `Command-into ZSCORE` | 155.23 | 159.57 | 177.94 | 2.8% | 14.6% |
| `Command-into ZRANK` | 435.44 | 274.23 | 385.36 | -37.0% | -11.5% |
| `Command-into ZREVRANK` | 439.19 | 275.93 | 404.34 | -37.2% | -7.9% |
| `Command-into ZRANGE` | 468.14 | 486.58 | 541.31 | 3.9% | 15.6% |

## ZRANGE Serialization Breakdown

| Component | off ns/op | exact ns/op | block-hint ns/op |
| --- | ---: | ---: | ---: |
| `Score-index traversal` | 49.48 | 49.07 | 48.57 |
| `Member lookup` | 134.46 | 137.62 | 145.54 |
| `Score formatting only` | 71.24 | 68.84 | 67.01 |
| `Member RESP append only` | 429.65 | 436.63 | 520.21 |
| `Score RESP append preformatted` | 297.64 | 298.65 | 341.42 |
| `Score RESP append formatting` | 181.14 | 181.76 | 177.52 |
| `Direct RESP append WITHSCORES` | 737.93 | 767.69 | 878.89 |
| `RESP append` | 247.54 | 245.71 | 252.58 |
| `RESP append WITHSCORES` | 501.92 | 532.76 | 546.46 |
| `Full command into` | 466.92 | 478.95 | 546.05 |
| `Full command into WITHSCORES` | 917.07 | 930.34 | 1,351.33 |

## Read-Path Notes

- `ZRANGE` response construction is the visible in-process cost: RESP-only serialization is 275.77 ns/op versus 242.44 ns/op for raw range iteration.
- `ZRANGE` breakdown separates score-index traversal, member lookup, and RESP append costs: 49.48 ns/op traversal, 134.46 ns/op member lookup, and 247.54 ns/op RESP append.
- `WITHSCORES` adds double formatting work in the RESP append path: 501.92 ns/op versus 247.54 ns/op without scores.
- Score formatting and score bulk append are now separated: 71.24 ns/op for formatting only and 181.14 ns/op for score bulk append with formatting.
- `ZRANGE` command-into versus string-wrapper cost should be read as a server-path check, not a guaranteed microbench win: command-into is 468.14 ns/op versus 500.83 ns/op for the string wrapper.
- `ZSCORE` command overhead over raw lookup is about 79.63 ns/op.
- `ZRANK` remains mostly data-structure work in-process: raw rank is 376.51 ns/op versus 435.44 ns/op for command execution.
