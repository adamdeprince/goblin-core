# Wikimedia full replay — public evidence

This is the small, publishable evidence bundle for the
[September 8–9, 2026 full comparison](../../full-comparison-20260909.md).
All six engines passed: 1,483,700,913 increments, 80,798,328 final members,
and matching full-state mapping digests.

- [Raw result table](run/summary.tsv)
- [Full-state verification output](run/verification.txt)
- [Hardware, versions, and run metadata](run/metadata.txt)
- [Goblin and verifier executable/source fingerprints](run/software.sha256)
- [Incumbent and client executable fingerprints](run/incumbents.sha256)
- [Input fingerprint before the run](preflight/input.sha256)
- [Matching input fingerprint after the run](input-after.sha256)
- [Packed object allocation](run/results/goblin-packed-int32-float32.goblin-memory.txt)
- [Standard object allocation](run/results/goblin-standard.goblin-memory.txt)
- [Dragonfly startup output and allocator warning](run/dragonfly.server-log.txt)

## Per-engine evidence

Each row links the exact launch command, full-state digest, final memory
report, and five-minute memory/cardinality samples. Samples are not a
continuous peak-memory measurement.

| Engine | Command | Digest | Memory | Samples |
| --- | --- | --- | --- | --- |
| Packed INT32/FLOAT32 | [Launch](run/state/goblin-packed-int32-float32.command) | [JSON](run/digests/goblin-packed-int32-float32.json) | [INFO](run/results/goblin-packed-int32-float32.info-memory.txt) | [TSV](run/samples/goblin-packed-int32-float32.tsv) |
| Standard | [Launch](run/state/goblin-standard.command) | [JSON](run/digests/goblin-standard.json) | [INFO](run/results/goblin-standard.info-memory.txt) | [TSV](run/samples/goblin-standard.tsv) |
| Dragonfly | [Launch](run/state/dragonfly.command) | [JSON](run/digests/dragonfly.json) | [INFO](run/results/dragonfly.info-memory.txt) | [TSV](run/samples/dragonfly.tsv) |
| Valkey 9.1.0 | [Launch](run/state/valkey-9.1.command) | [JSON](run/digests/valkey-9.1.json) | [INFO](run/results/valkey-9.1.info-memory.txt) | [TSV](run/samples/valkey-9.1.tsv) |
| Redis 8.8.0 | [Launch](run/state/redis-8.8.command) | [JSON](run/digests/redis-8.8.json) | [INFO](run/results/redis-8.8.info-memory.txt) | [TSV](run/samples/redis-8.8.tsv) |
| Redis 7.2.4 | [Launch](run/state/redis-7.2.4.command) | [JSON](run/digests/redis-7.2.4.json) | [INFO](run/results/redis-7.2.4.info-memory.txt) | [TSV](run/samples/redis-7.2.4.tsv) |

## Frozen harness

The original [replay script](harness/run.sh),
[full-run controller](harness/run_full.sh),
[streaming digest generator](harness/zset_digest.py), and
[digest comparator](harness/compare_digests.py) are preserved unchanged.
They expect the binaries, input, and project layout described in the report;
this bundle is not a self-contained replay installation.

[Build selection](selection.json) describes the preceding optimization and
limited-run selection. Its input hash and small-run memory values are not the
full run's input or results. Use the full-run files linked above.

The complete project remains at
`adam@naamah:/home/adam/wiki2/wikimedia-full-20260908T190700Z`.
The 33.25 GB input, executable, and database shutdown snapshot are intentionally
excluded from the public website. Earlier Wikimedia and Lichess results are
unchanged.
