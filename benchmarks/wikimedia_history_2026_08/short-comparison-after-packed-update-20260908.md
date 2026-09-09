# Limited Wikimedia comparison after packed updates — September 8, 2026

Packed Goblin is now competitive with standard Goblin and the incumbents in
this limited replay. It finished in **33.504 seconds**, versus **33.815 seconds**
for standard and **33.768–34.574 seconds** for the incumbents. All six engines
passed full verification. These small gaps in one short run do not establish
a statistically reliable ranking.

## Workload and configuration

All six engines ran concurrently on naamah against the same frozen
2,000,000-command Wikimedia page-ID/edit-increment input used in the previous
limited comparison. The input contains 41,237,066 bytes; its checksum was
verified before and after the run. The user's live `redis.cmds` was not used
or changed.

The unchanged Wikimedia/Lichess-style harness uses ordinary reply-per-command
`redis-cli` over Unix sockets. Feed time excludes startup and final digest
generation. There is no explicit CPU pinning (available CPUs `0-127`), matching
the earlier six-engine comparison. RSS is sampled every ten seconds; the
table reports final process RSS before digest generation.

Both Goblin modes use the same tested Release executable from the selected
micro-optimization stage. It retains integer hash mixing and single-lookup
updates, plus leaner Swiss vacancy probes, single-lookup removal and direct
tuple equality. The shared-tree-path, reordered-comparison and dirty-argument
experiments remain excluded. No implementation changes were made for this
rerun.

Packed uses INT32/FLOAT32, numeric member tie order, and merge exponent `0.5`
(512-slot leaves, 23-record dirty threshold). Standard and incumbents use
lexicographic member tie order. Redis/Valkey retain the existing active-defrag
settings and disabled persistence. Dragonfly uses one proactor thread.
Incumbent and client executable hashes match the prior limited run exactly.

## Results

| Engine | Feed seconds | Commands/s | Final RSS (MiB) |
| --- | ---: | ---: | ---: |
| Goblin packed INT32/FLOAT32 | 33.504 | 59,694 | 31.9 |
| Dragonfly | 33.768 | 59,228 | 36.9 |
| Goblin standard | 33.815 | 59,145 | 35.7 |
| Redis 7.2.4 | 33.817 | 59,142 | 36.3 |
| Redis 8.8 | 34.104 | 58,644 | 30.4 |
| Valkey 9.1 | 34.574 | 57,847 | 28.2 |

Packed took 0.9% less time than standard and 0.8–3.1% less than the incumbents
in this run. Standard and Redis 7.2.4 are effectively tied. This measures
end-to-end client/server replay, not maximum server throughput or full-dataset
scaling.

Compared with the [previous concurrent limited run](short-comparison-after-merge-20260908.md),
packed fell from 112.320 to 33.504 seconds: **70.2% less elapsed time**, or
3.35 times the throughput. Standard and the incumbents remained near their
previous times. This before/after comparison includes the hash fix and later
changes; it does not isolate the micro-pass benefit. The
[controlled update measurements](packed-update-comparison-20260908.md) and
[micro-pass measurements](packed-micro-comparison-20260908.md) report those
separately.

Packed's internal zset allocation remains **7,810,016 bytes (7.45 MiB)**,
versus **10,661,794 bytes (10.17 MiB)** for standard: **26.7% less object
memory**. Whole-process RSS was 32,652 KiB versus 36,552 KiB: 10.7% less.
Internal object allocation is distinct from process RSS and is not directly
comparable with incumbent RSS. Packed retained 1,095 leaves, 35 branches,
height 3, 284,712 sorted entries and 11,947 dirty entries.

## Verification and preservation

Every engine has 279,458 members, score sum 2,000,000 and maximum score 11,792.
All mapping digests match, and each engine's required tie order was checked.
Every complete per-engine digest also matches its saved prior-run digest.
All client/error logs are empty, every digest pipeline status is zero, and
`verification_status=0`.

The benchmark servers, feeds and samplers exited after verification. The
preexisting Redis service was left running. All earlier Wikimedia and Lichess
data, artifacts and writeups are preserved in their original locations.

## Provenance and artifacts

- Feeds started at `2026-09-08T19:00:11Z`; verification finished at
  `2026-09-08T19:00:46Z`.
- Goblin executable SHA-256:
  `9513ff0e26399b0c68d6b84032a95e4724cdb6e0d2f4c9def984033f5c3b4e8d`.
- Input SHA-256:
  `3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e`.
- Remote project: `/home/adam/wiki2/wikimedia-short-20260908T185930Z`.
- Remote results: `/home/adam/wiki2/wikimedia-short-20260908T185930Z/run`.
- Local results: `benchmark-results/wikimedia-short-2000000-20260908T185930Z`.

The new remote project preserves its own input and executable copies. Results
include the exact harness, executable hashes, build-selection provenance,
metadata, launch commands, memory reports, samples, logs and full digests.
