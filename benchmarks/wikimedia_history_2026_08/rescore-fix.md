# Standard zset rescore fix — September 7, 2026

The full run under
`/home/adam/wiki2/wikimedia-zset-bplus-20260905T023755Z/runs/full-1483700913-20260905T024352Z`
on naamah was stopped at the user's request. Its input, source, binary, samples,
and incumbent results are preserved; the run is marked `STOPPED-BY-USER`.

Standard zset positioning used a score-only binary search followed by a linear
walk through equal-score blocks. The allocation-safe rescore path repeated
that walk for the old and new positions. The fix binary-searches block maxima
by `(score, lexicographic member)`, reserves before transferring an entry, and
tracks positions through splits/removal without repeated global searches.
Member-score preparation now precedes the transfer, so failure restores the
member snapshot without a reverse rescore. Optional source-block merging may
be deferred on allocation failure. Redis member ordering is unchanged.

The fixed build and fresh diagnostic results are under
`/home/adam/wiki2/wikimedia-zset-rescore-fix-20260907T182911Z` on naamah.

## Focused score-index measurement

[`../zset_tie_rescore.cpp`](../zset_tie_rescore.cpp) was compiled with
`g++ -std=c++23 -O3 -DNDEBUG` against the preserved original header and the
fixed header. Each run performs 50,000 random member lookups followed by
50,000 score increments. Setup is excluded. This measures the index directly,
not end-to-end Redis command or full Wikimedia replay throughput.

All members initially have score 2 in the tied case:

| Members | Before rescore (µs/op) | Fixed rescore (µs/op) | Speedup |
| ---: | ---: | ---: | ---: |
| 10,000 | 0.503864 | 0.234286 | 2.15× |
| 100,000 | 3.093790 | 0.411698 | 7.51× |
| 1,000,000 | 38.865765 | 0.553253 | 70.25× |

At one million distinct initial scores, rescore time was 0.395309 µs before
and 0.391482 µs after. The raw files also include 2,048-score-bucket cases.
Each variant/shape/size was measured once, so small timing differences should
not be treated as statistically significant.

Every completed measurement independently verifies the final member/score
mapping, cardinality, uniqueness, and lexicographic score order. All passed.
The original header's layout validator failed on the 100k and 1M tied cases:
64-slot capacity rounding could exceed the default 1,125-slot block limit.
That pre-existing issue is recorded as `layout_valid=0` in those raw results;
the fixed version caps the rounding and passes every layout check.

Raw files: `results/verified-{before,after}-{10000,100000,1000000}-{tied,bucketed,unique}.tsv`.

## Regression coverage

- Equal-score churn, decimal lexicographic ordering, shared prefixes, embedded
  NULs, negative/fractional/infinite scores, and absent/deleted entries.
- Moves in both directions, source draining, destination splitting, merging,
  empty-block removal, and all three rank-cache modes.
- Rejected allocations during index growth, and Store rollback with/without
  cached score text; rounded capacity at the default block size.
- Core and focused tests under AddressSanitizer/UndefinedBehaviorSanitizer,
  plus a Redis differential check (10,127 commands, seed 90210).

## Bounded Wikimedia replay check

The fixed Release core and focused tests also passed on naamah. A fresh replay
of the first 1,000,000 commands completed in 16.596 s for Goblin standard and
16.985 s for Redis 8.8, with zero command errors. Both final digests verified
165,385 members, score sum 1,000,000, maximum score 2,747, and the same
page-id/edit-count mapping. Artifacts are in `smoke-1000000` under the fixed
project. The validation servers exited afterward; the full benchmark remains
stopped. These short-replay timings do not establish full-run throughput.
