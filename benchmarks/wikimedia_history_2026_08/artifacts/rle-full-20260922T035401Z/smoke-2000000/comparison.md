# Wikimedia typed-layout and RLE comparison

Generated 2026-09-22T04:10:50.385147+00:00. Run: `wikimedia-rle-matrix-20260922T035401Z`.

Each variant receives 2,000,000 increments in source order. All variants run concurrently from empty sets using the same frozen Release executable, merge exponent 0.5, Unix sockets, and ordinary reply-per-command redis-cli.

UUID members are zero-extended numeric page IDs. Their longer command file is generated before timing. Digests normalize UUIDs back to decimal page IDs and compare against an independent reference, including numeric tie order. Compare RLE on/off within each layout; UUID and integer wire sizes differ.

| Layout | RLE | Feed status | Seconds | Increments/s | Final RSS GiB | Object GiB | Compressed leaves | Used base score MiB |
|---|---|---|---:|---:|---:|---:|---:|---:|
| int32-float32 | off | PASS | 40.232 | 49,712 | 0.019 | 0.007 | 0 | 1.086 |
| int32-float32 | on | PASS | 40.045 | 49,944 | 0.017 | 0.006 | 1095 | 0.016 |
| int32-float64 | off | PASS | 39.836 | 50,206 | 0.026 | 0.014 | 0 | 2.191 |
| int32-float64 | on | PASS | 40.061 | 49,924 | 0.021 | 0.009 | 2216 | 0.057 |
| int64-float32 | off | PASS | 39.742 | 50,325 | 0.026 | 0.014 | 0 | 1.095 |
| int64-float32 | on | PASS | 40.421 | 49,479 | 0.023 | 0.011 | 2216 | 0.028 |
| int64-float64 | off | PASS | 39.834 | 50,208 | 0.026 | 0.014 | 0 | 2.191 |
| int64-float64 | on | PASS | 40.524 | 49,353 | 0.023 | 0.011 | 2216 | 0.057 |
| uuid-float32 | off | PASS | 42.496 | 47,063 | 0.030 | 0.018 | 0 | 1.100 |
| uuid-float32 | on | PASS | 42.668 | 46,874 | 0.028 | 0.016 | 2773 | 0.035 |
| uuid-float64 | off | PASS | 42.314 | 47,266 | 0.035 | 0.023 | 0 | 2.208 |
| uuid-float64 | on | PASS | 42.574 | 46,977 | 0.032 | 0.020 | 3348 | 0.083 |

Positive savings below mean RLE used less time or memory.

| Layout | Feed time saved | RSS saved | Object allocation saved |
|---|---:|---:|---:|
| int32-float32 | +0.46% | +8.78% | +23.94% |
| int32-float64 | -0.56% | +20.61% | +39.25% |
| int64-float32 | -1.71% | +13.33% | +25.78% |
| int64-float64 | -1.73% | +12.82% | +24.61% |
| uuid-float32 | -0.40% | +5.55% | +8.89% |
| uuid-float64 | -0.61% | +10.15% | +15.14% |

## Full-state verification

```text
goblin-packed-int32-float32-rle-off: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int32-float32-rle-on: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int32-float64-rle-off: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int32-float64-rle-on: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int64-float32-rle-off: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int64-float32-rle-on: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int64-float64-rle-off: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-int64-float64-rle-on: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-uuid-float32-rle-off: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-uuid-float32-rle-on: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-uuid-float64-rle-off: PASS: 279458 members, score_sum=2000000, max_score=11792.0
goblin-packed-uuid-float64-rle-on: PASS: 279458 members, score_sum=2000000, max_score=11792.0
all engines contain the same page_id -> edit-count mapping
```

RSS is sampled process residency immediately after feed drain, before verification; it is not peak RSS. Object allocation is the separate GOBLIN.MEMORY counter. No final forced compaction is performed. This is one concurrent trial per variant, including client and protocol costs; it does not establish maximum throughput or statistical significance.
