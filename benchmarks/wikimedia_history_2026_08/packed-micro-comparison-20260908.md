# Packed micro-optimization pass — September 8, 2026

This pass starts with the retained integer hash mixer and prepared-slot update
implementation from the [earlier update experiment](packed-update-comparison-20260908.md).
It does not include either rejected shared-tree-path candidate. All earlier
Wikimedia and Lichess data, artifacts and writeups are preserved.

## Selected changes

- Swiss lookup/insertion skips tombstone masks when the table has none, and
  stops searching for a tombstone after finding the first one. Copy and rehash
  insert into a fresh table with an empty-only probe specialization.
- Packed point removal retains its found Swiss slot. After the tree erase
  succeeds, slot erasure destroys the value and updates the control byte and
  counts without another hash or probe. It uses the existing transient token,
  not an additional persistent member pointer.
- Live tuple equality is `score == score && key == key`, avoiding two full
  ordering comparisons. Live scores/fences cannot be NaN; signed zero and
  infinities retain their existing semantics. The ordering comparator itself
  is unchanged.

The selected source is stage `06-equality`, rebuilt with the expanded tests as
`07-selected`. No storage layout, precision, numeric/UUID ordering, split or
merge policy, exponent, or allocation preflight was changed. The earlier hash
mixer and single-lookup/prepared-insertion implementation remain intact.

## Focused method and results

All timings were measured on naamah, CPU 2, with `c++ -O3 -DNDEBUG -std=c++23`.
Each stage is compiled against frozen headers and the identical benchmark
source. Builds do not overlap timed runs. Trials run sequentially and stage
order rotates between rounds. Round zero is a warmup, excluded from results.
This is a shared workstation, not a dedicated noise-free benchmark machine.

The final isolation run has seven measured rounds, four candidate/control
builds, and twelve workloads. Full-index workloads include parsing, Swiss and
tree operations, but not protocol/client overhead. Each uses 1,000,000 timed
operations except new insertion and sequential deletion, which use 200,000.
Local rescoring uses 384 members; increment, random, churn and no-op workloads
use 20,000. A churn operation is one removal followed by one reinsertion.
Setup, input generation and final mapping/invariant checks are untimed.

Median **nanoseconds per full-index operation**, lower is better:

| Workload | Retained baseline | Selected | Median paired time change |
| --- | ---: | ---: | ---: |
| Same-leaf rescore | 152.021 | 144.317 | -4.97% |
| Tied increment | 233.202 | 230.600 | -1.02% |
| Random rescore | 281.090 | 275.956 | -2.01% |
| New insertion | 205.689 | 199.558 | -3.15% |
| Random delete/reinsert pair | 299.622 | 296.601 | -0.97% |
| Sequential deletion | 50.400 | 48.958 | -2.81% |
| No-op update | 51.953 | 52.104 | +0.51% |

The percentage is the median of each round's paired percentage change, not
the ratio of the two medians. Small differences, especially near 1%, should
not be treated as strong general performance guarantees. All workload
identities, final checksums, member counts and allocated bytes match across
every measured stage and warmup.

Tree-only measurements expose a tradeoff hidden by looking only at full-index
results. The selected build improves local rescoring by 3.82% but slows the
tree-only tied-increment workload by 3.13%. Other tree-only changes are small:
random -0.41%, churn -0.32%, redistribution +0.61%. The increment generator in
the tree-only benchmark differs from the full-index generator; these are not
timings that can be subtracted to attribute time to Swiss versus the tree.

## Candidate isolation

Each candidate and its measurements remain in the new project:

| Stage | Change relative to preceding relevant stage | Decision |
| --- | --- | --- |
| `00-retained` | Prior hash mixer plus prepared slots | Baseline |
| `01-vacancy` | Fewer Swiss tombstone masks, empty-only copy/rehash | Keep |
| `02-erase-slot` | Single-probe point removal | Keep |
| `03-tuple-compare` | Equality-first ordering plus direct tuple equality | Drop ordering change |
| `04-dirty-args` | Direct stored score and borrowed retired tuple | Not retained |
| `05-equality-dirty` | Restore original ordering comparator | Isolation only |
| `06-equality` | Restore original dirty-tail arguments | Selected source |
| `07-selected` | Same production headers, final expanded tests/server build | Final verification |

The first five-round screen showed roughly 2.5% less insertion time from the
vacancy changes. The next screen showed about 2.6% less sequential deletion
time and 1.4% less churn time after adding slot erasure. Reordering the score
comparison helped some rescoring cases but hurt increment, churn and deletion
workloads, so it was removed. Changing dirty-tail argument passing did not
provide a convincing overall improvement over the smaller selected patch.

## Replay and verification

The selected server is checked against the frozen retained server using an
alternating three-round replay of the same 2,000,000-command Wikimedia prefix.
It uses ordinary reply-per-command `redis-cli`, a Unix socket, CPU affinity
`2-5`, INT32/FLOAT32, and exponent `0.5` (512-slot leaves, 23-record threshold).
Only packed Goblin runs; standard and incumbents are not rerun. These results
are not directly comparable with earlier concurrent six-engine results.

Input: 41,237,066 bytes, SHA-256
`3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e`.
The live input file is not used. Every replay must match the previously
verified full mapping and numeric-order digest, not just a score sum.

| Build | Three replay times (seconds) | Median seconds |
| --- | --- | ---: |
| Retained baseline | 28.608, 28.424, 28.453 | 28.453 |
| Selected micro-optimizations | 28.513, 28.413, 28.421 | 28.421 |

The 0.11% median time reduction is **effectively unchanged end to end**, not
evidence of a meaningful replay speedup. All six replays have zero command
errors and match the complete known-good 279,458-member result. Final object
allocation is exactly **7,810,016 bytes** and process RSS **32,652 KiB** in
every trial. Input hashes match before every run and after the last run.

The final selected code passes all three targeted suites in local Debug,
local ASan/UBSan, and naamah Release builds. Tests cover all six key/score
layouts, allocation-failure behavior, existing update/command semantics,
deep routing and random churn, signed-zero/infinite-score no-ops, token
erasure/destruction, copy/rehash with tombstones, and wraparound collision
probes with Swiss group widths 1, 8, 16 and 64. No benchmark servers or feeds
remain running.

## Reproduction and artifacts

Remote project: `/home/adam/wiki2/wikimedia-packed-micro-20260908T183500Z`.
Local artifact copy: `benchmark-results/packed-micro-20260908T183500Z`.
All candidates freeze the three production headers and both focused benchmark
sources and record source/executable hashes.

Selected server SHA-256:
`9513ff0e26399b0c68d6b84032a95e4724cdb6e0d2f4c9def984033f5c3b4e8d`.
Its three headers match the measured `06-equality` stage exactly, and both
focused benchmark executables rebuilt in `07-selected` are byte-identical to
those measured in `06-equality`. Server executables remain on naamah; the
local archive omits binary directories but includes frozen source and hashes.

Use `freeze_micro_stage.sh` for screening builds, `run_micro_pass.sh` for
rotated focused measurements, and `summarize_micro_pass.py` for strict
cross-stage workload/result/memory checks. The selected server uses
`build_update_stage.sh` and `run_micro_replay.sh` for final verification.
`summarize_micro_replay.py` validates every input hash, completion/status,
error log, result digest and object allocation before summarizing replays.

```sh
python3 source/benchmarks/wikimedia_history_2026_08/summarize_micro_pass.py . \
  --label final-isolation --stages 00-retained 02-erase-slot \
  05-equality-dirty 06-equality --trials 7
```
