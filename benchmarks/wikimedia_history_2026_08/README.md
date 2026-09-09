# Wikimedia page-edit sorted-set benchmark

This project reuses the Lichess replay methodology without modifying any
Lichess input, result, chart, or write-up.  Its source trace is the English
Wikipedia MediaWiki History dump for 2026-08.  Each revision with a page ID was
converted to one command:

```text
ZINCRBY key 1 <page_id>
```

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

## Reports

- [Completed full comparison — September 8–9, 2026](full-comparison-20260909.md):
  1,483,700,913 increments, 80,798,328 members, all six engines verified.
- [Limited comparison after packed optimizations — September 8, 2026](short-comparison-after-packed-update-20260908.md):
  the preceding two-million-command comparison.
- [Full-run launch record](full-run-20260908T190700Z.md): historical startup
  configuration; consult the completed report above for final results.
