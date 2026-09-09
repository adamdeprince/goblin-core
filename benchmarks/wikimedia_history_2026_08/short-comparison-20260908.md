# Short Wikimedia comparison — September 8, 2026

The fixed Goblin standard build completed this short replay 2.2–3.0% sooner
than Redis/Valkey. Dragonfly was fastest. Packed int32/float32 used less memory
than Goblin standard, but took 3.63 times as long.

## Workload

One concurrent run of all six engines on naamah, using the first 2,000,000
commands from `/home/adam/wiki2/redis.cmds`. Each command increments a page's
edit count. The run used the existing Wikimedia harness and ordinary
reply-per-command `redis-cli` feeds over Unix-domain sockets. Feed time excludes
server startup and final digest generation. Memory was sampled every ten
seconds; the table reports final process RSS, including the server baseline.

Goblin uses the tested Release build containing the
[standard-zset rescore fix](rescore-fix.md). The packed lane uses
`packed-int32-float32`, merge exponent `0.5`, and numeric member tie order.
Standard Goblin and the incumbents retain lexicographic member tie order.
Redis/Valkey retain the harness's active-defragmentation settings and disabled
persistence; Dragonfly runs with one proactor thread.

## Results

| Engine | Feed seconds | Commands/s | Final RSS (MiB) |
| --- | ---: | ---: | ---: |
| Dragonfly | 34.256 | 58,384 | 36.8 |
| Goblin standard | 37.024 | 54,019 | 35.7 |
| Valkey 9.1 | 37.872 | 52,809 | 28.2 |
| Redis 8.8 | 37.895 | 52,777 | 30.4 |
| Redis 7.2.4 | 38.174 | 52,392 | 36.3 |
| Goblin packed int32/float32 | 134.304 | 14,892 | 31.7 |

Goblin's internal zset allocation reports were 10,661,794 bytes for standard
and 7,678,944 bytes for packed, including the packed member-to-score Swiss
table and ordered tree. These object allocation counts differ from whole
process RSS and are not directly comparable with incumbent process RSS.

All six lanes passed final verification: 279,458 members, score sum 2,000,000,
maximum score 11,792, identical page-id/edit-count mapping digests, and zero
command errors. Each lane's own tie ordering was checked. All benchmark clients
and servers exited after verification; the earlier full replay remains stopped.

This is one short run. The small standard-versus-Redis/Valkey margin is an
observation, not a statistically established speed advantage or a measurement
of full-dataset scaling.

## Provenance and artifacts

- Timed feeds started at `2026-09-08T13:45:59Z`; verification finished at
  `2026-09-08T13:48:14Z`.
- Fixed binary SHA-256:
  `5f3fac83ec8d4db1f3fee1a60de5367122b10c467155ce9154873e0ed2d35eb2`.
- SHA-256 of the exact 2,000,000-line input prefix, computed after the run:
  `3972ab0d6b968c6b9bc117d1085f3e6f10cd85f2aa8ac4fde0dda35abe0c751e`.
- Remote artifacts:
  `/home/adam/wiki2/wikimedia-zset-rescore-fix-20260907T182911Z/short-2000000-20260908T134416Z`.
- Local artifact copy:
  `benchmark-results/wikimedia-short-2000000-20260908T134416Z`.

The directories contain the harness, software hashes, metadata, samples,
per-engine memory reports, logs, summary, and digests. Lichess data and reports
and the earlier Wikimedia runs were preserved.
