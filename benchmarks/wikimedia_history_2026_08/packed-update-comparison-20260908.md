# Packed update-path optimization measurements — September 8, 2026

The retained implementation combines integer hash mixing and single-lookup
score updates. Its median 2-million-command replay is **28.510 seconds versus
105.100 seconds for baseline: 3.69× faster, or 72.9% less elapsed time**.
Packed object allocation remains exactly **7,810,016 bytes**.

Two shared-tree-path candidates were implemented and measured separately.
Neither produced a convincing general benefit, so neither is retained in the
working implementation. Both candidates, tests and results are preserved.

## Method

Each cumulative stage was frozen before testing and measurement on naamah.
Three sequential trials replay the same 2,000,000-command, 41,237,066-byte
Wikimedia prefix through ordinary reply-per-command `redis-cli` on a Unix
socket. All packed replay processes inherit CPU affinity `2-5`. Only the packed
INT32/FLOAT32 lane runs, with exponent `0.5`, 512-slot leaves and a 23-record
dirty threshold. Builds and focused tests do not overlap timed replays.

The input is a read-only copy of the previously verified prefix, not the live
file that was rebuilt during an earlier benchmark. SHA-256:
`3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e`.

The baseline already includes the standard-zset fix and all three earlier
packed merge optimizations. Neither the incumbent engines nor standard Goblin
was rerun in this experiment. These isolated, CPU-affined results are not a
direct comparison with the earlier six-engine concurrent runs.

## Replay results

| Cumulative stage | Three trial times (seconds) | Median seconds | Decision |
| --- | --- | ---: | --- |
| Baseline | 105.249, 105.098, 105.100 | 105.100 | Reference |
| 1: integer hash mixer | 28.433, 28.615, 28.443 | 28.443 | Keep |
| 2: single lookup/prepared insertion | 28.528, 28.510, 28.434 | 28.510 | Keep; final implementation |
| 3: shared tree path, first candidate | 28.746, 28.637, 28.463 | 28.637 | Not retained |
| 3b: directional-bound refinement | 28.521, 28.454, 28.511 | 28.511 | Not retained |

The hash change accounts for the clear end-to-end improvement. Changes after
it do not measurably improve this replay. All final RSS measurements round to
31.9 MiB; exact values range from 32,632 to 32,660 KiB across builds. Internal
object allocation is identical across all fifteen trials.

## What changed

### 1. Integer hashing

`PackedIntegerTraits` now avalanches the full 64-bit integer value, using the
same final mixer as the existing UUID hasher. Extracting the shared mixer does
not change UUID hash output. There is no added per-member storage and no
change to member ordering or snapshot representation.

At capacity 1,000,000, the first 100,000 positive IDs now select **95,168 distinct
starting buckets**, rather than one. Regression tests check both bucket bits
and seven-bit fingerprints for INT32 and INT64, including positive/negative
sequences, signed extremes and INT64 values differing only in high bits.

### 2. Score-slot reuse and prepared insertion

Swiss exposes a short-lived lookup/insertion token that finds an existing score
or a vacant slot in one probe sequence. Existing packed updates retain their
score pointer and publish the replacement only after the tree update succeeds.
For accepted new members, reservation happens before tree mutation; a rehash
refreshes the vacant slot. Commit performs no lookup or allocation. Abandoning
a prepared insertion never makes the member visible.

Bulk commands retain map-allocation preflight, counting distinct missing keys
instead of reserving for every item. Bulk preflight uses temporary key scratch
and may perform additional lookups; the single-member hot path needs no such
scratch. Existing-member updates and rejected NX/XX operations do not reserve
new Swiss slots.

### 3. Shared-path experiment — not retained

The first candidate tested the old leaf's routing bounds, reused the common
old/new path prefix, and cancelled common-ancestor count changes. Splits
refreshed both paths. A second candidate used the known direction of the move
to check only the relevant bound, special-cased a one-leaf tree, and restored
the original ordinary-lookup loop to isolate unrelated inserts/deletes.

Both candidates passed correctness and sanitizer tests. After the measurements
below, only these experimental production tree changes were removed. The
working tree header exactly matches the preserved stage-2 header, retaining
all earlier merge optimizations. The new deep-tree regression tests remain.

## Focused measurements

`packed_update.cpp` measures the complete packed index, including numeric member
parsing, Swiss lookup and tree work, but excluding client/server protocol.
Setup, update generation and full final-state validation are outside the timer.
Each binary receives a warmup followed by three measured trials on CPU 2, with
stage order rotated between rounds. Warmups are excluded from medians.

The first four-stage rotated confirmation (`confirmation/`) gives these
medians in **nanoseconds per operation**, with lower being better:

| Stage | Same-leaf rescore | Tied increment | Random rescore | New insertion |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 205.7 | 1,657.0 | 1,699.2 | 9,057.8 |
| Hash mixer | 165.4 | 236.2 | 280.1 | 197.8 |
| Single lookup | 151.7 | 227.6 | 279.8 | 181.5 |

Same-leaf tests use 384 members; increment and random tests use 20,000 members
and 200,000 updates. Insertion tests time 50,000 new members. Step 2 reduces
same-leaf and insertion time by about 8%, and increment time by about 4%, in
this confirmation; random rescoring is effectively unchanged. These gains do
not translate into a measurable replay improvement.

An additional all-candidate rotated confirmation is preserved in
`directional-confirmation/`. Several trials had conspicuous timing spikes,
including unchanged control workloads. A desktop indexer was present on the
shared host and was left untouched; its contribution was not measured.

To resolve the marginal stage-3 result, an **eight-trial alternating paired
check** compared the retained stage 2 against refined stage 3b (`final-pair-check/`):

| Full-index workload | Stage 2 ns/op | Stage 3b ns/op | Elapsed-time change |
| --- | ---: | ---: | ---: |
| Same-leaf rescore | 151.912 | 153.916 | +1.32% |
| Tied increment | 227.621 | 225.894 | −0.76% |
| Random rescore | 278.401 | 281.442 | +1.09% |
| New insertion | 182.919 | 185.746 | +1.55% |

Positive changes mean slower. These small differences do not establish robust
regression sizes, but they offer no compelling overall reason to retain the
extra shared-path code. Tree-only increment time improved by about 0.8%; other
ordinary tree operations were essentially flat or slightly slower. Complete
tree-only rescore, churn and redistribution results are preserved alongside
the full-index measurements. No claim about the replay's CPU-time breakdown is
inferred by subtracting these different workloads.

## Correctness and artifacts

All fifteen replays verified the same 279,458 members, score sum 2,000,000,
maximum score 11,792 and exact saved numeric-order digest. Error logs are empty
and digest pipelines exited successfully. Focused workload checksums match
across variants. Numeric packed tie order, merge policy and all six layouts are
unchanged.

Local debug and ASan/UBSan core, packed-command and direct-tree suites passed
for each implemented stage and again for the retained final code. Each remote
candidate passed the corresponding Release tests before measurement. New
coverage includes wrapped collision probes, tombstones, token cancellation and
rehashing, updates at the load ceiling, duplicate bulk members, rejected map
growth, failed new/existing-member tree splits, and deep routing/rank checks
across all six representations.

- Remote project: `/home/adam/wiki2/wikimedia-packed-update-20260908T173900Z`.
- Local artifacts: `benchmark-results/packed-update-20260908T173900Z`.
- Selected binary: `stages/02-single-probe/bin/goblin-core` inside the remote project.
- Selected binary SHA-256:
  `5f49fab7c30fc1a9bd81b61bd7e2ceae1eca39b0f5ff757200c0f0791ea9864c`.
- Baseline binary SHA-256:
  `c2a762c21218893d6bb902fbd8b35eae7661c3720a490721416af2ad8fe50937`.

Frozen stage directories retain the tested headers, source/executable hashes,
test/build logs, replay artifacts, memory reports and focused measurements.
Linux binaries and the frozen input remain on naamah; the local artifact copy
omits them. The build/replay/micro runners refuse to overwrite result
directories. `summarize_update.py` validates trial counts, mapping identity,
error logs and focused checksums. Earlier Wikimedia results and all Lichess
data, artifacts and writeups were preserved. All benchmark processes exited;
the preexisting Redis service was left untouched.
