# Full Wikimedia sorted-set benchmark — September 8–9, 2026

Goblin packed INT32/FLOAT32 finished this full replay fastest and with the
lowest final process RSS: **7h 08m 02s and 2.10 GiB**, versus **7h 34m 35s and
3.46 GiB** for Goblin standard. Packed took **5.8% less time** and used
**39.2% less RSS** than standard. Against Dragonfly, the fastest incumbent,
packed took **3.3% less time** and used **53.6% less RSS**.

All six engines completed **1,483,700,913 increments** and passed full-state
verification with **80,798,328 members** and matching page-ID → edit-count
mapping digests. There were no command errors. This is one concurrent,
single-client-per-engine replay, not a maximum-throughput or statistical
performance claim.

## Final results

All six engines ran concurrently on `naamah`. Each received the same complete
input, in the same order, using its own server process and Unix-domain socket.

| Engine | Replay time (h:mm:ss) | Increments/s | Final RSS (GiB) | RSS bytes/live member |
| --- | ---: | ---: | ---: | ---: |
| Goblin packed INT32/FLOAT32 | 7:08:02 | 57,771 | 2.10 | 27.93 |
| Dragonfly, one proactor | 7:22:34 | 55,876 | 4.53 | 60.22 |
| Goblin standard | 7:34:35 | 54,399 | 3.46 | 45.97 |
| Valkey 9.1.0 | 8:19:20 | 49,523 | 5.21 | 69.23 |
| Redis 8.8.0 | 8:25:41 | 48,900 | 5.47 | 72.63 |
| Redis 7.2.4 | 8:52:13 | 46,463 | 7.55 | 100.36 |

Times are displayed to the nearest second. Rates and comparisons use the
millisecond-resolution feed times in the [raw summary](artifacts/full-20260908T190700Z/run/summary.tsv).
Throughput is total input increments divided by feed seconds; it is not a
sampled instantaneous server counter. GiB means 2³⁰ bytes; recorded OS RSS is
in KiB. RSS per member includes whole-server overhead, not just zset storage.

Packed took 14.3% less time than Valkey, 15.4% less than Redis 8.8, and 19.6%
less than Redis 7.2.4. Its final RSS was 59.7%, 61.5%, and 72.2% lower,
respectively.

Standard also finished ahead of Valkey and both Redis versions: 9.0%, 10.1%,
and 14.6% less time, respectively. It took 2.7% more time than Dragonfly but
used 23.7% less RSS. These are final, equal-member-count comparisons, unlike
the unequal-progress snapshots reported while the run was active.

## Workload and timing

The source is the user-provided English Wikipedia
[MediaWiki History 2026-08 dataset](https://dumps.wikimedia.org/other/mediawiki_history/2026-08/enwiki/),
converted to a command stream of page-ID edit increments:

```text
ZINCRBY key 1 <page_id>
```

The benchmark uses one sorted set, `key`. Page IDs are used directly; there
is no title-to-ID dictionary or other key translation table in the benchmark.
The complete frozen input contains **33,245,740,607 bytes (30.96 GiB)** and
**1,483,700,913 commands**. Starting from empty sets, 80,798,328 commands
introduce a member and the remaining 1,402,902,585 update an existing member:
approximately **94.6% existing-member updates** overall. The replay has no
deletions or application read-query mix and does not reproduce event-arrival
timing; it consumes the saved command order as quickly as this client allows.

The full file was copied from `/home/adam/wiki2/redis.cmds` into an independent,
read-only Btrfs copy-on-write snapshot before launch. Its line count and SHA-256
were checked before the run, and its SHA-256 matched again after completion.
No limited-prefix input or saved limited-run result was substituted for the
full workload.

The harness reuses the Lichess-style methodology: ordinary reply-per-command
`redis-cli --raw` over a Unix socket, one client per engine, with reply output
scanned for errors. It does **not** use `redis-cli --pipe`. Feed timing covers
the client/server replay and reply drain, excluding server startup, input
counting/hashing, final verification, and cleanup. This measures end-to-end
replay performance, including client, protocol, scheduling, and error-filter
costs, rather than an isolated zset operation cost.

RSS, `INFO memory`, and `ZCARD` were sampled approximately every 300 seconds.
Final RSS was recorded immediately after each engine's feed drained, before
the full-state verification queries. It excludes replay-client memory and
the filesystem page cache. It is **not peak RSS**: periodic sampling cannot
capture every transient allocation or resize. No post-load forced compaction
or common idle interval was added before the final measurements.

## Hardware and implementation settings

The archived [metadata](artifacts/full-20260908T190700Z/run/metadata.txt)
records `naamah` as an AMD Ryzen Threadripper PRO 5995WX, 64 cores / 128 hardware
threads, one NUMA node, approximately 991 GiB usable RAM, and no swap. The
kernel is Linux `7.0.0-30-generic`, x86-64. All six engines were launched
concurrently without explicit CPU affinity; they shared the host and input
page cache. The input was read during preflight, so this is not a controlled
cold-cache storage test.

Both Goblin modes use the **same frozen optimized Release executable** as the
successful [limited comparison](short-comparison-after-packed-update-20260908.md).
The retained changes include 64-bit integer hash mixing, reuse of the located
score slot, prepared insertion, leaner Swiss vacancy/tombstone probing,
single-lookup point deletion, and direct live-tuple equality. The experimental
shared-tree-path rescore change remains excluded. The full run does not
isolate individual optimization effects; those are covered by the earlier
[update](packed-update-comparison-20260908.md) and
[micro-optimization](packed-micro-comparison-20260908.md) measurements.

The representation and server settings were:

- **Packed:** `--zset-implementation packed-int32-float32` and
  `--packed-zset-merge-exponent 0.5`. Members are stored as binary INT32 and
  scores as FLOAT32. Member → score uses the Swiss table; score order uses
  the arena-indexed B+ tree with leaf-local dirty records. The selected
  layout has 512-entry leaves and a merge threshold of
  `ceil(512^0.5) = 23` dirty records per leaf, not `sqrt(total members)`.
  Equal-score members are ordered numerically.
- **Standard:** `--zset-implementation standard`. Final diagnostics report
  adaptive `i32` score storage, rank cache off, and no score-string cache.
  Members retain string/lexicographic semantics. The reported memory result
  already benefits from standard's adaptive integer score storage.
- **Redis / Valkey:** versions 8.8.0, 7.2.4, and 9.1.0, respectively, reporting
  jemalloc 5.3.0. Persistence is disabled with `--save '' --appendonly no`.
  The inherited active-defrag settings are `--activedefrag yes`,
  `--active-defrag-ignore-bytes 1mb`, `--active-defrag-threshold-lower 5`,
  and `--active-defrag-cycle-max 75`.
- **Dragonfly:** `--proactor_threads=1 --maxmemory=0`. Its version output is
  `dev-0000000`, so the archived executable hash identifies the tested build;
  no numbered release is inferred. The shutdown snapshot is outside the
  timed replay and final RSS measurements. Its
  [server log](artifacts/full-20260908T190700Z/run/dragonfly.server-log.txt)
  contains a mimalloc aligned-allocation fallback warning; this was not a
  command failure, and its performance impact was not isolated.
- **Client:** the same Redis 8.8.0 `redis-cli` executable for every engine.
  Incumbent and client hashes match those used for the preceding limited run.

The [exact launch commands](artifacts/full-20260908T190700Z/README.md#per-engine-evidence)
and [executable fingerprints](artifacts/full-20260908T190700Z/run/incumbents.sha256)
are preserved. Goblin does not provide a working `--version` flag in this
build; its executable fingerprint below is the build identifier.

## Object memory and packed-tree state

`GOBLIN.MEMORY key` reports the following allocations immediately after replay:

| Representation | Allocated bytes | Allocated GiB | Bytes/live member |
| --- | ---: | ---: | ---: |
| Packed INT32/FLOAT32 | 2,183,937,408 | 2.034 | 27.03 |
| Standard | 3,614,846,068 | 3.367 | 44.74 |

Packed uses **39.6% less internally accounted object memory**, saving
1,430,908,660 bytes. This is separate from the **39.2% process-RSS reduction**;
internal allocation counters and OS residency measure different things.
Incumbent RSS is not compared with Goblin's smaller object-only counter.

Standard's allocation consists of 1,980,125,336 bytes for member storage and
its references/capacity, 443,356,336 bytes for the member index, and
1,191,363,860 bytes for the score index. Actual member-string payload is
637,876,833 bytes, already included in the member-storage allocation, not
an additional allocation. Spare capacity therefore contributes materially
to this endpoint; memory ratios should not be assumed constant at every size.

Packed finishes with **288,451 leaves**, **12,419 branches**, and **tree height
5**. Diagnostics report 81,806,916 sorted-base records and 3,171,191 dirty
records. These are physical tree records, including deferred update/deletion
state, not extra live members: the verified live count is 80,798,328. No
final merge was forced to produce the reported footprint.

The original [packed memory report](artifacts/full-20260908T190700Z/run/results/goblin-packed-int32-float32.goblin-memory.txt),
[standard memory report](artifacts/full-20260908T190700Z/run/results/goblin-standard.goblin-memory.txt),
per-engine `INFO memory` reports, and complete five-minute sample series are
included in the result archive.

## Correctness and compatibility

After **all** feeds finished, each entire zset was read in 65,536-member rank
pages using `ZRANGE ... WITHSCORES`. Paging bounded server replies and did not
alter the timed replay. Verification found, for every engine:

- 80,798,328 members and score sum **1,483,700,913**, matching the input count.
- All scores integral, with minimum 1 and maximum **2,162,914**.
- Numeric member range **0–84,112,066** and zero ordering violations.
- Matching count and all three 128-bit commutative mapping fingerprints.
- Identical ordered SHA-256 for standard and all four incumbents. Packed's
  ordered digest is intentionally different because its equal-score tie
  ordering is numeric; its mapping fingerprints still match.

These IDs fit INT32. Every observed score is below 2²⁴ = 16,777,216, so the
integer increments remain exactly representable in FLOAT32. This workload
does **not** establish equivalence for larger counts, fractional scores,
arbitrary string members, or Redis lexicographic tie ordering. Packed is
deliberately a narrower representation here.

All replay-client, command-error, digest-client, and digest-error logs are
empty, and every digest pipeline recorded `0 0`. The controller recorded
`JOB-PASS`, `exit.status=0`, and `verification_status=0`. See the complete
[verification output](artifacts/full-20260908T190700Z/run/verification.txt)
and [per-engine digests](artifacts/full-20260908T190700Z/README.md#per-engine-evidence).

## Scope of the conclusions

Packed has the best measured time and final RSS in this run. Standard also
finishes ahead of Redis and Valkey, but behind Dragonfly. The strongest memory
comparison is now at equal final state, rather than estimated from different
input-progress snapshots.

There is only one full trial per engine, with no confidence intervals. All
engines share the host; CPU scheduling, cache behavior, allocator growth,
and concurrent activity can affect timing. The few-percent gaps between
packed, Dragonfly, and standard should be confirmed with repeated controlled
trials before being treated as stable rankings.

The workload strongly exercises repeated positive increments of numeric IDs.
It does not time application reads, arbitrary rescoring, deletes, mixed
workloads, pipelined or multi-client saturation, persistence, or recovery.
The final verification scan is a correctness check, not a query-performance
benchmark. Dragonfly's single-proactor result is not a claim about its
multi-proactor scaling. No conclusion about the other five packed layouts
is drawn from this INT32/FLOAT32 run.

## Provenance, timeline, and artifacts

Run ID: `wikimedia-full-20260908T190700Z`.

- Concurrent feeds started: **2026-09-08 19:14:27 UTC** (3:14:27 p.m. EDT).
- Last feed completed; verification began: **2026-09-09 04:06:40 UTC**
  (12:06:40 a.m. EDT).
- Full-state verification completed: **2026-09-09 04:09:43 UTC**.
- Controller completed after cleanup and input rehash:
  **2026-09-09 04:11:01 UTC** (12:11:01 a.m. EDT).

Input SHA-256:

```text
234506a2217a83580d4b47a644818a9f41a51e342d2e0acadfb0ed2e6ced66b1
```

Goblin executable SHA-256:

```text
9513ff0e26399b0c68d6b84032a95e4724cdb6e0d2f4c9def984033f5c3b4e8d
```

The full remote project is
`adam@naamah:/home/adam/wiki2/wikimedia-full-20260908T190700Z`.
Its frozen input, executable, headers, harness, raw results, and checksums
remain in place. The new completed-run local archive,
`benchmark-results/wikimedia-full-completed-20260908T190700Z`,
contains the result tables, memory samples, logs, digests, frozen harness and
headers, build-selection provenance, preflight test results, and completion
records. The 33.25 GB input and executable were not copied into that archive;
they remain on `naamah`. The untimed Dragonfly shutdown snapshot is included.

A separate [public evidence bundle](artifacts/full-20260908T190700Z/README.md)
contains the small result files, verification digests, samples, launch
commands, and frozen replay scripts. It is served alongside this report;
the input, executable, and shutdown snapshot are not website downloads.

`selection.json` records the preceding micro-optimization/limited-run
provenance: its input hash and small-run memory values are **not** the full
run's input or results. Use `preflight/input.sha256`, `run/input.sha256`,
`input-after.sha256`, and `run/results/` for this full run. Frozen harness
hashes and saved result checks were revalidated when preparing this report.

To recheck the archived mapping comparison without starting any servers:

```bash
python3 benchmark-results/wikimedia-full-completed-20260908T190700Z/run/compare_digests.py \
  benchmark-results/wikimedia-full-completed-20260908T190700Z/run/digests \
  1483700913 goblin-standard goblin-packed-int32-float32 \
  redis-8.8 redis-7.2.4 valkey-9.1 dragonfly
```

The earlier [launch report](full-run-20260908T190700Z.md) and launch-only local
archive are preserved as historical snapshots, not updated in place. No
Lichess input, artifact, chart, or writeup was changed. Preparing this report
did not rerun the benchmark or change production code.
