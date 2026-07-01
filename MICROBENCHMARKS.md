# Goblin Core Microbenchmarks

Generated: 2026-07-01 21:17:03 UTC.

These measurements isolate in-process read-path costs. They do not include sockets, polling, Redis comparison, or process RSS accounting.

Source data:

- `benchmark-results/microbench-1m-rank-cache-off.json`
- `benchmark-results/microbench-1m-rank-cache-on.json`

## Configuration

| Setting | Rank cache off | Rank cache on |
| --- | ---: | ---: |
| `members` | `1000000` | `1000000` |
| `ops` | `1000000` | `1000000` |
| `range_size` | `16` | `16` |
| `warmups` | `1` | `1` |
| `seed` | `12345` | `12345` |
| `score_shape` | `integer` | `integer` |
| `rank_cache` | `false` | `true` |
| `score_string_cache` | `false` | `false` |

## Rank Cache Off

| Metric | Raw ZSet ns/op | RESP ns/op | Command into ns/op | Command string ns/op | Parse into ns/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZSCORE` | 78.11 | 29.92 | 159.26 | 159.93 | 203.57 |
| `ZRANK` | 344.90 | 24.69 | 432.31 | 438.13 | 431.31 |
| `ZREVRANK` | 328.21 | 24.69 | 438.35 | 431.14 | 433.89 |
| `ZRANGE` | 241.56 | 279.96 | 477.48 | 506.10 | 491.95 |
| `ZRANGE WITHSCORES` | 241.56 | 576.14 | 952.52 | 967.47 | 955.68 |

## Rank Cache On

| Metric | Raw ZSet ns/op | RESP ns/op | Command into ns/op | Command string ns/op | Parse into ns/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ZSCORE` | 74.28 | 30.59 | 160.53 | 160.73 | 203.90 |
| `ZRANK` | 196.61 | 25.14 | 256.38 | 257.37 | 315.13 |
| `ZREVRANK` | 205.07 | 25.14 | 256.09 | 261.44 | 273.06 |
| `ZRANGE` | 244.88 | 283.62 | 482.14 | 511.43 | 493.68 |
| `ZRANGE WITHSCORES` | 244.88 | 560.81 | 936.31 | 967.37 | 961.33 |

## Rank Cache Effect

| Metric | Off ns/op | On ns/op | Change | Off ops/sec | On ops/sec |
| --- | ---: | ---: | ---: | ---: | ---: |
| `Raw ZSCORE` | 78.11 | 74.28 | -4.9% | 12,802,376 | 13,462,627 |
| `Raw ZRANK` | 344.90 | 196.61 | -43.0% | 2,899,363 | 5,086,136 |
| `Raw ZREVRANK` | 328.21 | 205.07 | -37.5% | 3,046,790 | 4,876,368 |
| `Raw ZRANGE iter` | 241.56 | 244.88 | 1.4% | 4,139,758 | 4,083,596 |
| `Command-into ZSCORE` | 159.26 | 160.53 | 0.8% | 6,278,945 | 6,229,199 |
| `Command-into ZRANK` | 432.31 | 256.38 | -40.7% | 2,313,170 | 3,900,494 |
| `Command-into ZREVRANK` | 438.35 | 256.09 | -41.6% | 2,281,262 | 3,904,941 |
| `Command-into ZRANGE` | 477.48 | 482.14 | 1.0% | 2,094,319 | 2,074,100 |

## ZRANGE Serialization Breakdown

| Component | Off ns/op | On ns/op | Off ops/sec | On ops/sec |
| --- | ---: | ---: | ---: | ---: |
| `Score-index traversal` | 50.69 | 50.70 | 19,729,297 | 19,722,926 |
| `Member lookup` | 140.81 | 137.13 | 7,101,981 | 7,292,437 |
| `Score formatting only` | 68.55 | 66.98 | 14,587,697 | 14,930,396 |
| `Member RESP append only` | 438.73 | 426.64 | 2,279,322 | 2,343,908 |
| `Score RESP append preformatted` | 315.50 | 301.18 | 3,169,551 | 3,320,247 |
| `Score RESP append formatting` | 181.94 | 180.72 | 5,496,324 | 5,533,538 |
| `Direct RESP append WITHSCORES` | 739.93 | 750.92 | 1,351,484 | 1,331,702 |
| `RESP append` | 251.51 | 248.76 | 3,975,984 | 4,019,874 |
| `RESP append WITHSCORES` | 532.59 | 533.01 | 1,877,622 | 1,876,146 |
| `Full command into` | 471.10 | 469.36 | 2,122,674 | 2,130,551 |
| `Full command into WITHSCORES` | 988.38 | 944.64 | 1,011,761 | 1,058,607 |

## Read-Path Notes

- `ZRANGE` response construction is the visible in-process cost: RESP-only serialization is 279.96 ns/op versus 241.56 ns/op for raw range iteration.
- `ZRANGE` breakdown separates score-index traversal, member lookup, and RESP append costs: 50.69 ns/op traversal, 140.81 ns/op member lookup, and 251.51 ns/op RESP append.
- `WITHSCORES` adds double formatting work in the RESP append path: 532.59 ns/op versus 251.51 ns/op without scores.
- Score formatting and score bulk append are now separated: 68.55 ns/op for formatting only and 181.94 ns/op for score bulk append with formatting.
- `ZRANGE` command-into versus string-wrapper cost should be read as a server-path check, not a guaranteed microbench win: command-into is 477.48 ns/op versus 506.10 ns/op for the string wrapper.
- `ZSCORE` command overhead over raw lookup is about 81.15 ns/op.
- `ZRANK` remains mostly data-structure work in-process: raw rank is 344.90 ns/op versus 432.31 ns/op for command execution.
