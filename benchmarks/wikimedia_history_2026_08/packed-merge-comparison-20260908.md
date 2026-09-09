# Incremental packed-merge comparison — September 8, 2026

Three cumulative changes were implemented and measured separately, with the
INT32/FLOAT32 merge exponent held at `0.5` (512-slot leaves, 23 dirty records).
No integer-hash change is included in these builds.

The tree operations improved substantially, but the end-to-end Wikimedia
replay did not: its final median is 110.187 seconds versus 104.937 seconds for
baseline. A separate integer-hash clustering issue was confirmed on naamah.

## Changes

1. **Base-slot invalidation bitmap.** First modification of a sorted tuple
   locates its old `(score, member)` and marks the slot. Repeated changes reuse
   the existing dirty record. Compaction and reads consult bits instead of
   binary-searching the dirty tail for each base tuple. Bitmap storage is 64
   bytes per INT32/FLOAT32 leaf-arena slot, including reserved inactive slots.
2. **Run-based compaction and merging.** Visit invalidated slots, preserve the
   unchanged prefix and block-move surviving runs. Exponential probing locates
   backward-merge runs. A bounded stack union array constructs only the live
   dirty entries, including for read reconciliation. Deletion-only compactions
   skip the addition merge; ordered additions append to the surviving base.
3. **Direct redistribution.** Compacted neighboring leaves transfer just the
   boundary run. This removes the approximately 8 KiB two-leaf scratch array
   and the copy-out/copy-back sequence. Both transfer directions are tested
   without increasing allocated capacity.

The Swiss member-to-score map, leaf thresholds, tree fanout and numeric packed
member tie order are unchanged. Reads still never trigger compaction. Ordinary
compaction remains within the existing leaf allocation, with bounded stack
scratch and `O(B + D log D)` work for leaf capacity `B` and dirty count `D`.

## Wikimedia replay

Three sequential trials per immutable build on naamah, each replaying the first
2,000,000 commands from `/home/adam/wiki2/redis.cmds`. The existing Wikimedia
harness uses ordinary reply-per-command `redis-cli` over a Unix socket. Only
the packed lane runs; all its processes inherit CPU affinity `2-5`. Builds and
focused benchmarks do not overlap timed replays. Memory is sampled every ten
seconds. Each trial starts with an empty server.

This differs from the earlier concurrent six-engine comparison, so a fresh
packed baseline was measured. Incumbents were not rerun. Their saved known-good
mapping, via the already-verified packed digest, is reused for verification.

| Cumulative build | Trial seconds | Median seconds | Change from previous |
| --- | --- | ---: | ---: |
| Baseline | 111.020, 104.864, 104.937 | 104.937 | — |
| 1: bitmap | 106.692, 107.168, 107.209 | 107.168 | +2.1% |
| 2: run merge | 111.783, 112.195, 110.729 | 111.783 | +4.3% |
| 3: direct redistribution | 110.187, 110.598, 109.165 | 110.187 | −1.4% |

Positive changes mean more elapsed time. The merge changes have not delivered
an end-to-end replay gain across the twelve trials. These are short sequential
experiments on a shared host, not statistically established regression sizes.
A desktop file indexer was observed using CPU during stage 2; it was left
untouched. The focused confirmation rotates frozen build order after all
builds and replays to reduce temporal confounding.

The exact input-prefix SHA-256 is
`3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e`.
Verification compares the complete numeric-order digest against the previously
verified prefix: 279,458 members, score sum 2,000,000, maximum score 11,792 and
ordered SHA-256
`5bbb15683fa301f4c873b6740b978dd23c9c81fa53b9e478323cd8fb8654106e`.

## Focused tree timings

The standalone `benchmarks/packed_merge.cpp` measures tree mutations without
the Swiss map, parsing or protocol work. It uses INT32/FLOAT32 and exponent
`0.5`, with setup, change generation and final mapping/order validation outside
the timed region.

- `local`: 384 members in one leaf, 1,000,000 random rescores.
- `increment`: 20,000 initially tied members, 1,000,000 random increments.
- `random`: 20,000 members, 1,000,000 arbitrary rescores.
- `churn`: 20,000 members, 1,000,000 delete/reinsert pairs; the reported unit is
  one pair, not one individual tree call.
- `rebalance`: 1,953 timed deletions that force redistribution against a full
  neighbor, alternating transfer direction. Initial construction and draining
  are untimed; the timer cost is included.

Each binary gets a warmup followed by three measurements pinned to CPU 2. The
original per-stage results remain under `micro/`. A separate rotated-order
confirmation is saved under `micro-confirmation/`. The table below reports
those confirmed medians in **nanoseconds per timed operation**, with lower
being better. No warmup is included in a median.

| Cumulative build | Same-leaf rescore | Tied increment | Random rescore | Delete/reinsert pair | Redistribution |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 282.8 | 246.4 | 515.8 | 503.9 | 556.3 |
| 1: bitmap | 115.5 | 190.3 | 224.6 | 223.4 | 314.7 |
| 2: run merge | 116.6 | 183.0 | 221.0 | 218.4 | 223.3 |
| 3: direct redistribution | 116.6 | 182.1 | 220.9 | 218.7 | 140.9 |

Incremental interpretation:

- The bitmap reduces ordinary tree-mutation time by 23–59% and redistribution
  time by 43% relative to baseline.
- Run merging reduces redistribution time by another 29%. Its ordinary
  mutation changes are small: approximately 1% slower for same-leaf rescores,
  and 2–4% faster for the other shapes. These small changes need caution.
- Direct boundary transfer reduces redistribution time by another 37%;
  ordinary mutation timings are essentially unchanged from stage 2.
- Overall, random rescoring falls from 515.8 to 220.9 ns, while redistribution
  falls from 556.3 to 140.9 ns. These are tree-only timings, not command latency.

## Memory and correctness

All twelve replays passed exact final mapping/order verification with zero
command errors. All focused workload identities/checksums agree across builds.
The new direct-tree tests also pass against the preserved baseline header.

The baseline object allocation was 7,678,944 bytes. All three changed builds
report 7,810,016 bytes: an additional 131,072 bytes for 2,048 reserved leaf-arena slots.
The 1,095 live leaves and all tree-layout counts are unchanged. This remains
below the prior standard-zset allocation of 10,661,794 bytes for the same final
mapping. Internal object allocations are distinct from whole-process RSS.

Local core, packed-command and new direct-tree tests pass in debug and under
AddressSanitizer/UndefinedBehaviorSanitizer. Direct-tree coverage includes all
six layouts at exponents `0`, `0.5` and `1`; repeated updates, deletion/reentry,
bitmap word boundaries, dirty copies, cross-leaf movement, random churn,
infinities, signed zero, rank/score reads, splits, coalescing, root collapse and
redistribution in both directions. Tests assert reads preserve dirty counts
and redistribution preserves allocated capacity. Each changed remote build was
gated on passing Release core/packed/direct-tree tests before benchmarking.

## Separate integer-hash issue

Code inspection found that `PackedIntegerTraits::Hash` is `std::hash<Integer>`,
which returns these positive IDs unchanged in the tested standard libraries.
The Swiss table selects the initial bucket with `(hash * capacity) >> 64`.
Consequently, small positive integer hashes all start probing at bucket zero.
This is a clustering problem in the member-to-score map, independent of merge
thresholds or tree compaction.

`check_integer_hash.cpp` exercises the actual packed hasher and that bucket
formula, without substituting a new hasher. At a capacity of 1,000,000, page IDs
10, 26,323,569 and 84,076,787—and even `INT32_MAX`—all select bucket zero. The
first 100,000 positive IDs select just one distinct initial bucket. The
diagnostic confirmed these values on naamah after the measured comparisons.

This gives a concrete explanation for why tree-only improvements may fail to
move the replay time. No CPU-profile percentage or speedup from fixing hashing
has been measured here. A hash fix is deliberately not mixed into these three
requested merge stages.

## Artifacts and reproduction

Remote project:
`/home/adam/wiki2/wikimedia-packed-merge-20260908T143140Z`.

Local results:
`benchmark-results/packed-merge-20260908T143140Z`.

Each stage preserves its header, executable hashes, test logs, three replay
directories, full final-state digests and focused timing files. Build-stage
scripts also preserve compiler configuration and source fingerprints. The
baseline executable is the previously verified standard-rescore-fix build;
its SHA-256 is
`5f3fac83ec8d4db1f3fee1a60de5367122b10c467155ce9154873e0ed2d35eb2`.

The runners `build_merge_stage.sh`, `run_merge_stage.sh`,
`run_merge_micro.sh` and `run_merge_confirmation.sh` refuse to overwrite
completed output directories. `summarize_merge.py --complete` validates trial
counts and consistent focused-workload checksums. The harness now optionally
accepts `REFERENCE_DIGEST` to reuse a saved known-good result.

Two incomplete baseline setup attempts were retained separately: one lacked
copied harness dependencies; the other completed a correct replay but the old
comparator required a live Redis lane. Neither is included in the results
above. Lichess data, artifacts and writeups, the earlier Wikimedia runs and all
incumbent results were preserved.

All benchmark server/client processes exited after verification. The preexisting
Redis service was left untouched.
