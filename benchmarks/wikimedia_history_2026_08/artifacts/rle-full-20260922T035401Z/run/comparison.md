# Wikimedia typed-layout and RLE comparison

Generated 2026-09-22T14:38:21.196479+00:00. Run: `wikimedia-rle-matrix-20260922T035401Z`.

Each variant receives 1,483,700,913 increments in source order. All variants run concurrently from empty sets using the same frozen Release executable, merge exponent 0.5, Unix sockets, and ordinary reply-per-command redis-cli.

UUID members are zero-extended numeric page IDs. Their longer command file is generated before timing. Digests normalize UUIDs back to decimal page IDs and compare against an independent reference, including numeric tie order. Compare RLE on/off within each layout; UUID and integer wire sizes differ.

| Layout | RLE | Feed status | Seconds | Increments/s | Final RSS GiB | Object GiB | Compressed leaves | Used base score MiB |
|---|---|---|---:|---:|---:|---:|---:|---:|
| int32-float32 | off | PASS | 34,211.488 | 43,368 | 2.089 | 2.034 | 0 | 312.069 |
| int32-float32 | on | PASS | 34,322.285 | 43,229 | 1.626 | 1.573 | 288444 | 3.393 |
| int32-float64 | off | PASS | 34,667.177 | 42,798 | 4.022 | 4.001 | 0 | 626.996 |
| int32-float64 | on | PASS | 34,679.061 | 42,784 | 2.553 | 2.519 | 577769 | 13.406 |
| int64-float32 | off | PASS | 34,669.548 | 42,796 | 4.022 | 4.001 | 0 | 313.498 |
| int64-float32 | on | PASS | 34,796.175 | 42,640 | 3.067 | 3.028 | 577769 | 6.703 |
| int64-float64 | off | PASS | 34,555.511 | 42,937 | 4.022 | 4.001 | 0 | 626.996 |
| int64-float64 | on | PASS | 34,845.986 | 42,579 | 3.104 | 3.071 | 577769 | 13.406 |
| uuid-float32 | off | PASS | 36,559.638 | 40,583 | 5.043 | 5.017 | 0 | 314.373 |
| uuid-float32 | on | PASS | 36,897.240 | 40,212 | 4.636 | 4.594 | 726602 | 8.406 |
| uuid-float64 | off | PASS | 37,071.213 | 40,023 | 5.922 | 5.897 | 0 | 630.251 |
| uuid-float64 | on | PASS | 37,317.990 | 39,758 | 5.024 | 4.979 | 876790 | 20.248 |

Positive savings below mean RLE used less time or memory.

| Layout | Feed time saved | RSS saved | Object allocation saved |
|---|---:|---:|---:|
| int32-float32 | -0.32% | +22.19% | +22.68% |
| int32-float64 | -0.03% | +36.52% | +37.03% |
| int64-float32 | -0.37% | +23.74% | +24.33% |
| int64-float64 | -0.84% | +22.81% | +23.25% |
| uuid-float32 | -0.92% | +8.08% | +8.44% |
| uuid-float64 | -0.67% | +15.15% | +15.56% |

## Full-state verification

```text
goblin-packed-int32-float32-rle-off: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int32-float32-rle-on: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int32-float64-rle-off: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int32-float64-rle-on: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int64-float32-rle-off: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int64-float32-rle-on: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int64-float64-rle-off: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-int64-float64-rle-on: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-uuid-float32-rle-off: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-uuid-float32-rle-on: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-uuid-float64-rle-off: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
goblin-packed-uuid-float64-rle-on: PASS: 80798328 members, score_sum=1483700913, max_score=2162914.0
all engines contain the same page_id -> edit-count mapping
```

RSS is sampled process residency immediately after feed drain, before verification; it is not peak RSS. Object allocation is the separate GOBLIN.MEMORY counter. No final forced compaction is performed. This is one concurrent trial per variant, including client and protocol costs; it does not establish maximum throughput or statistical significance.
