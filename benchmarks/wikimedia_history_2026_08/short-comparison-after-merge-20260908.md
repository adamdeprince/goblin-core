# Short Wikimedia comparison after merge optimizations — September 8, 2026

Goblin standard completed this replay in 33.504 seconds, close to the
incumbents. Packed INT32/FLOAT32 took 112.320 seconds: 3.35 times as long as
standard, with 10.8% less whole-process RSS. All six engines passed verification.

## Workload and configuration

One concurrent run of all six engines on naamah, replaying the first 2,000,000
commands from the rebuilt `/home/adam/wiki2/redis.cmds`. The rebuild processes
had finished before this run. A separate read-only snapshot was created and
verified before use, avoiding the live-file truncation that invalidated the
earlier attempt. Its 41,237,066 bytes match the previously benchmarked prefix.

The unchanged Wikimedia harness uses ordinary reply-per-command `redis-cli`
over Unix-domain sockets. Feed time excludes startup and final verification.
There is no explicit CPU pinning, matching the earlier six-engine comparison.
RSS is sampled every ten seconds; the table reports final process RSS before
the full-state digest, including each server's baseline allocation.

Both Goblin modes use the same tested Release executable, containing the
[standard rescore fix](rescore-fix.md) and all three
[packed merge optimizations](packed-merge-comparison-20260908.md). Packed uses
`packed-int32-float32` and merge exponent `0.5`: 512-slot leaves and a
23-record dirty threshold. Numeric packed member tie order is retained;
standard and the incumbents retain lexicographic member tie order.

Redis/Valkey use the harness's existing active-defragmentation settings, with
persistence disabled. Dragonfly uses one proactor thread. No implementation
or threshold changes were made for this rerun. The separate packed integer
hash-clustering issue remains unfixed.

## Results

| Engine | Feed seconds | Commands/s | Final RSS (MiB) |
| --- | ---: | ---: | ---: |
| Dragonfly | 32.678 | 61,203 | 36.8 |
| Goblin standard | 33.504 | 59,694 | 35.7 |
| Valkey 9.1 | 33.700 | 59,347 | 28.2 |
| Redis 7.2.4 | 34.229 | 58,430 | 36.4 |
| Redis 8.8 | 34.513 | 57,949 | 30.4 |
| Goblin packed INT32/FLOAT32 | 112.320 | 17,806 | 31.9 |

Standard's elapsed time was 0.6% below Valkey, 2.1–2.9% below Redis, and 2.5%
above Dragonfly. These small differences from one short concurrent run do not
establish a statistically reliable ranking. This measures end-to-end replay
with the existing client, not maximum server throughput.

Packed's internal zset allocation was 7,810,016 bytes (7.45 MiB), versus
10,661,794 bytes (10.17 MiB) for standard: 26.7% less object memory. These
internal allocation counts are distinct from process RSS and should not be
compared directly with incumbent RSS. Packed retained 1,095 leaves, 35 branch
nodes, height 3, 284,712 sorted entries and 11,947 dirty entries.

Packed finished 16.4% sooner than its 134.304-second result in the
[earlier six-engine run](short-comparison-20260908.md). However, standard and
every incumbent also finished sooner this time. This is not an isolated
measurement of the merge changes' benefit; the separately controlled staged
measurements remain in the merge report.

## Verification and preservation

Every engine has 279,458 members, score sum 2,000,000 and maximum score 11,792.
All page-ID/edit-count mapping digests match; each engine's required tie order
was checked. Packed's complete ordered digest also matches the saved
known-good packed result. All client and command-error logs are empty, all
digest pipeline statuses are zero, and `verification_status=0`.

The snapshot checksum was rechecked after the replay. All benchmark clients,
servers and samplers exited; the preexisting Redis service was left untouched.
The failed live-input attempt and the user-stopped retry are preserved and
excluded from this comparison. Lichess data, artifacts and writeups, earlier
Wikimedia reports and the user's rebuilt input were not overwritten.

## Provenance and artifacts

- Feeds started at `2026-09-08T16:17:33Z`; verification finished at
  `2026-09-08T16:19:26Z`.
- Goblin executable SHA-256:
  `c2a762c21218893d6bb902fbd8b35eae7661c3720a490721416af2ad8fe50937`.
- Input snapshot SHA-256:
  `3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e`.
- Frozen input:
  `/home/adam/wiki2/wikimedia-short-20260908T161700Z/redis-2000000.cmds`.
- Remote results:
  `/home/adam/wiki2/wikimedia-short-20260908T161700Z/run`.
- Local results:
  `benchmark-results/wikimedia-short-2000000-20260908T161700Z`.

Results include the harness, software and incumbent checksums, metadata,
per-engine launch commands, memory reports, samples, logs, summary and digests.
The immutable executable remains under
`/home/adam/wiki2/wikimedia-packed-merge-20260908T143140Z/stages/03-redistribute/bin/goblin-core`.
