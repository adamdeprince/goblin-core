# Goblin Core List Work-Queue Benchmark

## Summary

This local benchmark compares Goblin Core's newly implemented work-queue
commands with its existing list operations. It measures both the default
segmented backend and the adaptive-PMA backend through the same RESP2/UDS client
at pipeline depth 64. It does not compare other servers and it does not measure
park-to-wake latency; every blocking-command row has data ready so its command
throughput is directly comparable with the non-blocking rows.

The ready `BLPOP` path sustains 2.08M commands/s on segmented and 2.26M on PMA,
83% and 81% of each backend's `LPOP` rate. Ready `BLMOVE` reaches 95% of
`LMOVE` on segmented and 97% on PMA. Segmented leads the single-element rotation
rows, while PMA is the clear batched-pop backend: `LMPOP COUNT 8` reaches 744K
commands/s, or 5.95M returned values/s, 4.25x the segmented command rate.

## Method

- Release build on an Apple M4 Max (`Mac16,5`, 16 logical CPUs), arm64 macOS,
  Darwin 25.5.0.
- One Goblin Core server and one native C++ client on the same machine.
- RESP2 over a Unix domain socket for every row.
- Pipeline depth 64, 200,000 measured commands, at least 4,096 warm-up commands,
  and the median of three trials.
- Read, update, and rotation rows use a 100,000-item list.
- Destructive rows are fully populated before timing. `BLPOP`, `BRPOP`,
  `BLMOVE`, and `BLMPOP` therefore measure their immediate ready path, not nil
  replies and not a producer wake-up.
- `LMPOP` and `BLMPOP` remove eight values per command. Their QPS remains command
  QPS; values/s is eight times the reported rate.
- A counted pop is one endpoint range mutation, not eight scalar pops. A compact
  listpack shifts its remaining bytes once; segmented storage discards whole
  leaves and rewrites at most one partial leaf; PMA clears the occupancy range
  and makes one shrink/compaction decision.
- Standard command names select the server's `--list-implementation` setting.
- This was a local development-machine run, not a quiet dedicated benchmark
  host. Use the relative command costs here; reserve cross-product claims for
  the dedicated-host [100k list benchmark](LIST-BENCHMARK.md).

## Results

Commands per second.

| Operation | Segmented | PMA |
|---|---:|---:|
| `LLEN` | 4,067,928 | 4,036,829 |
| `LINDEX middle` | 3,111,505 | 3,231,590 |
| `LRANGE 16` | 1,299,816 | 1,329,239 |
| `LSET middle` | 3,027,022 | 2,728,452 |
| `LPUSH` | 2,762,733 | 1,937,286 |
| `RPUSH` | 2,764,808 | 1,899,359 |
| `LPOP` | 2,507,390 | 2,804,536 |
| `RPOP` | 2,515,693 | 2,383,609 |
| `LMOVE` same-list rotation | 1,722,817 | 1,297,681 |
| `RPOPLPUSH` same-list rotation | 1,829,110 | 1,342,094 |
| `BLPOP` ready, two keys | 2,079,651 | 2,260,449 |
| `BRPOP` ready, two keys | 1,910,991 | 1,997,429 |
| `BLMOVE` ready, same-list rotation | 1,635,558 | 1,260,686 |
| `LMPOP COUNT 8`, two keys | 174,953 | 744,369 |
| `BLMPOP COUNT 8` ready, two keys | 173,173 | 729,919 |

For the two `COUNT 8` rows, the corresponding value rates are 1.40M and 1.39M
values/s on segmented, and 5.95M and 5.84M values/s on PMA.

## Reproduce

The benchmark is the native C++ target
`goblin_core_list_work_queue_benchmark`:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target goblin_core_server \
  goblin_core_list_work_queue_benchmark -j
build-release/goblin_core_list_work_queue_benchmark \
  "$PWD/build-release/goblin-core" 200000 64
```
