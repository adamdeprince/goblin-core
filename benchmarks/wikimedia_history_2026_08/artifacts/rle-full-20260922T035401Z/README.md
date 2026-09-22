# Completed Wikimedia RLE matrix evidence

Run: `wikimedia-rle-matrix-20260922T035401Z` on `naamah`.
All 12 full replays and verification pipelines passed. Completion:
2026-09-22T14:42:37Z. See the
[comparison report](../../rle-full-comparison-20260922.md).

- [Matrix](matrix.json), [controller log](controller.log),
  [exit status](exit.status), and [completion time](finished.utc).
- [Raw summary](run/summary.tsv), [generated comparison](run/comparison.md),
  [verification](run/verification.txt), and [host/run metadata](run/metadata.txt).
- `run/results/`: per-variant timing, RSS, `INFO memory`, and
  `GOBLIN.MEMORY` allocation/compression counters.
- `run/digests/`: normalized full-state digests and both pipeline exit codes.
- `run/logs/`: replay/digest error logs (all empty) and server logs.
- `run/samples/`: five-minute process memory/cardinality samples.
- `run/state/`: exact launch commands, input paths, socket paths, and
  historical process IDs. These are completed-run records, not active PIDs.
- [Executable hashes](preflight/binaries.sha256),
  [harness hashes](preflight/harness.sha256),
  [input hashes before](preflight/payloads.sha256), and
  [input hashes after](input-after.sha256).
- [Build script](preflight/build.sh), [tests](preflight/tests.log),
  [UUID conversion counts](preflight/uuid-conversion.json), and
  [input preparation](preflight/prepare-input.sh).
- `harness/`: frozen runner, controller, digest scripts, and UUID converter.
- `smoke-2000000/`: prefix summary, comparison, and verification.
- [Source preparation](preparation.json), [source file hashes](source-files.sha256),
  and [workspace changes](workspace.patch).

This bundle contains the small result and provenance files. Frozen inputs,
executables, the complete source snapshot, and build remain in the remote
project. The complete local result archive, including all preflight logs,
is in `benchmark-results/wikimedia-rle-matrix-20260922T035401Z/remote-completed`.
