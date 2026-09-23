# Wikimedia page-edit sorted-set benchmark

This project reuses the Lichess replay methodology without modifying any
Lichess input, result, chart, or write-up.  Its source trace is the English
Wikipedia MediaWiki History dump for 2026-08.  Each revision with a page ID was
converted to one command:

```text
ZINCRBY key 1 <page_id>
```

The latest [full typed-layout and RLE report](rle-full-comparison-20260922.md)
compares all six packed representations with compression enabled and disabled.
Score RLE is now enabled by default for packed zsets; the matrix passes an
explicit on/off flag for every variant, keeping the comparison reproducible.

`run.sh` starts every selected engine concurrently, gives each one its own
Unix-domain socket, and feeds each server the same command file through
`redis-cli` in normal reply-per-command mode.  The default matrix is:

- Goblin Core with `--zset-implementation standard`
- Goblin Core with `--zset-implementation packed-int32-float32` and the default
  `--packed-zset-merge-exponent 0.5`
- Redis 8.8
- Redis 7.2.4
- Valkey 9.1
- Dragonfly with one proactor thread

Redis and Valkey use the same aggressive active-defragmentation settings as the
Lichess benchmark.  RSS and `INFO memory` are sampled during the run, and final
RSS is recorded immediately after the feed drains.

The verifier streams `ZRANGE key 0 -1 WITHSCORES` from each server.  It records
an ordered digest and a 384-bit commutative mapping digest.  The latter permits
an exact workload comparison with the packed integer representation, whose
equal-score tie order is numeric rather than Redis's bytewise string order.  It
also checks sorted order, member count, and that the sum of final edit counts
equals the number of input increments.

The output directory must not already exist.  This makes every smoke or full
run clobber-safe by construction.

```bash
OUTDIR=/new/path \
PAYLOAD=$HOME/wiki2/redis.cmds \
GOBLIN=/path/to/goblin-core \
bash run.sh
```

Set `HEAD_LINES` to a positive integer for a smoke test; its default is `all`.

For large full runs, set `DIGEST_PAGE_SIZE=65536` to verify in bounded rank
pages after all feeds finish. This produces the same ordered member/score
stream without one enormous server reply. The default `0` retains single-reply
verification. This option does not change the timed replay path.

`run_full.sh` is the detached full-run controller. Prepare a new project with
`bin/goblin-core`, a frozen `harness/` copy, a read-only `redis.cmds` snapshot,
and `preflight/commands.txt`, `preflight/input.sha256` and
`preflight/incumbents.sha256`. Launch it in a dedicated tmux session with
`PROJECT_DIR` set. It runs all six engines concurrently, samples every five
minutes, verifies the results, rechecks the input checksum, and writes
`JOB-PASS` or `JOB-FAILED` plus `exit.status` when done. It survives SSH
disconnects; it does not resume across a host restart or automatically retry.
Earlier projects are never reused for a new run.

## Typed layouts with score RLE

`run_rle_matrix.sh` exercises all six packed layouts with score RLE off and
on: INT32, INT64, and UUID members crossed with FLOAT32 and FLOAT64 scores.
It uses the same reply-per-command replay method, with one server and Unix
socket per variant. Merge exponent defaults to 0.5. Each server also receives
a distinct unused loopback TCP port. Final results include `GOBLIN.MEMORY`
allocation and score-compression counters, and verification checks that each
server applied the requested RLE setting.

UUID variants need a second command file, generated before timing:

```sh
c++ -O3 -std=c++23 uuid_page_ids.cpp -o uuid-page-ids
./uuid-page-ids numeric.cmds uuid.cmds
OUTDIR=/new/result/path \
PAYLOAD=/frozen/numeric.cmds \
PAYLOAD_UUID=/frozen/uuid.cmds \
GOBLIN=/frozen/goblin-core \
DIGEST_PAGE_SIZE=65536 \
bash run_rle_matrix.sh
```

The converter maps each nonnegative INT32 page ID to a zero-extended UUID,
preserving member identity and numeric tie order. The verifier decodes these
UUIDs back to decimal IDs before hashing. UUID commands have longer wire
representations, so compare RLE on/off within each layout. `SERVERS` can select
a subset, using names such as `goblin-packed-int32-float32-rle-on`.

`plot_rle_comparison.py` uses Gnuplot to regenerate the full report's standalone
memory and timing SVGs from the archived twelve-variant result table. It also
plots RSS against live entry count for the new RLE result, earlier packed and
standard Goblin, and all four incumbents. That chart uses the saved September 8
and September 22 sample series, labels the runs, and exports its data and source
hashes under `artifacts/memory-growth-20260922/`.

`run_rle_full.py PROJECT_DIR` is the detached controller for the complete
matrix. It requires a frozen `bin/goblin-core`, `harness/` containing the
matrix runner and digest scripts, `input/numeric.cmds`, `input/uuid.cmds`, and
these `preflight/` records: successful `build.status` and
`prepare-input.status`, `payloads.sha256`, `reference-prefix-2000000.json`,
and the prior full-run `reference-numeric.json`. It first verifies a
two-million-command prefix against its independent reference, then runs and
verifies the full dataset, generates `run/comparison.md`, rechecks both input
checksums, and writes `JOB-PASS` or `JOB-FAILED`. The full run uses the
previously verified 1,483,700,913-command dataset and numeric full-state
reference. Launch in a dedicated tmux session to survive SSH disconnects.

## Reports

- [Completed full RLE matrix — September 22, 2026](rle-full-comparison-20260922.md):
  all 12 typed-layout/RLE combinations verified; RLE reduced final RSS by
  8.08–36.52%, with measured replay-time increases below 1%.
- [RLE matrix launch record](rle-full-run-20260922T035401Z.md): frozen input,
  executable, and configuration details for the completed matrix.
- [Completed full comparison — September 8–9, 2026](full-comparison-20260909.md):
  1,483,700,913 increments, 80,798,328 members, all six engines verified.
- [Limited comparison after packed optimizations — September 8, 2026](short-comparison-after-packed-update-20260908.md):
  the preceding two-million-command comparison.
- [Full-run launch record](full-run-20260908T190700Z.md): historical startup
  configuration; consult the completed report above for final results.
