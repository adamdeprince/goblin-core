# Goblin Core ARRAY Benchmark

Generated on a quiet dedicated benchmark host at 2026-07-16 23:02:24 UTC.

## Summary

This report compares Goblin Core's Classic and real-time ARRAY implementations with the Redis-compatible servers in the benchmark suite, using only native `AR*` commands over RESP/TCP. It loads `500,000` `16`-byte values into dense and stride-16 sparse arrays, measures population throughput and process RSS, then exercises eight read and mutation workflows at pipeline depth `256`. A separate `500,000`-append, pipeline-1 test reports P90 through P99.99 latency. Engines without native ARRAY commands are marked `X`; hashes are never used as a fallback.

Classic is the memory-oriented implementation for sparse or evolving index ranges. RT trades fixed dense leaves and additional capacity for a more predictable mutation path; its reserved row provisions and prefaults hard index, metadata, and byte limits before timing. Two results stand out: resident-locked reserved RT records `36.11` us P99.99 versus `54.48` us for Redis 8.8; and Classic uses `30.64` RSS-delta bytes/value on the sparse shape versus `169.57` for Redis 8.8.

## Method

- `500,000` logical `16`-byte values in both dense (`stride=1`) and sparse (`stride=16`) shapes.
- Population and throughput rows use RESP/TCP pipeline depth `256`; individual-increment latency uses pipeline depth `1`.
- Goblin Classic uses adaptive sparse/dense leaves and a growable directory. Goblin RT uses fixed-size dense leaves, a fixed-depth directory, and a separate 2x value-arena growth policy.
- Every server receives native `ARSET`, `ARMSET`, `ARGET`, `ARMGET`, `ARCOUNT`, `ARLEN`, `ARDEL`, and `ARINSERT` requests. There is no hash-command fallback.
- Operation rates are medians of `3` native C++ client rounds. Churn counts one delete + restore pair as one logical operation.
- Servers run one at a time on one serving core; Dragonfly uses one proactor and mini-redis-go uses `GOMAXPROCS=1`.
- `X` means the server returned a command error for the required AR operation. Error replies are not timed.
- RSS is sampled from the launched PID (`ps` for mini-redis-go; `/proc` for the others), never from a server-reported RSS field.
- Incumbents are exercised strictly through RESP as black boxes; their source is not inspected.
- Goblin rows were refreshed at `2026-07-16 23:02:24 UTC`; unchanged incumbent rows are retained from the same-host `2026-07-16 21:17:44 UTC` run.

## Individual Increment Latency

Each of the `500,000` samples is one dense `ARSET key index value` append that advances a fresh array's logical length by one. Values are `16` bytes, indexes are sequential, connection setup is excluded, and RESP/TCP pipeline depth is `1`. At this sample count, the P99.99 tail contains approximately `50` commands.
The `goblin-rt-reserved` row issues `GOBLIN.RT.ARRESERVE` before timing, prefaulting its complete index, value-slot metadata, and encoded-byte budget. Reservation time and connection setup are excluded; exhaustion would fail instead of allocating in a measured command.
Immediately after timing, `/proc/<pid>/status` reported `goblin-classic-resp-tcp` 26.12 MiB locked / 0.00 MiB swapped; `goblin-rt-resp-tcp` 25.66 MiB locked / 0.00 MiB swapped; `goblin-rt-reserved-resp-tcp` 28.27 MiB locked / 0.00 MiB swapped. This verifies the benchmarked mappings were resident-locked and not swapped.

| Engine | P90 (us) | P99 (us) | P99.9 (us) | P99.99 (us) |
| --- | ---: | ---: | ---: | ---: |
| `goblin-classic-resp-tcp` | 16.91 | 21.36 | 36.42 | 160.99 |
| `goblin-rt-resp-tcp` | 16.86 | 20.86 | 23.28 | 52.82 |
| `goblin-rt-reserved-resp-tcp` | 16.76 | 20.46 | 22.65 | 36.11 |
| `redis-7.2.4` | X | X | X | X |
| `redis-8.8` | 18.26 | 22.82 | 26.92 | 54.48 |
| `valkey-9.1` | X | X | X | X |
| `dragonfly` | X | X | X | X |
| `mini-redis-go-55178df` | X | X | X | X |

## Dense Indexes

### Population And Memory

| Engine | representation | values/s | RSS MiB | RSS delta MiB | RSS delta B/value | key-reported B/value |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `goblin-classic-resp-tcp` | Classic AR* | 4.75M | 26.22 | 14.64 | 30.70 | 28.35 |
| `goblin-rt-resp-tcp` | RT AR* | 7.09M | 25.75 | 14.17 | 29.71 | 27.37 |
| `redis-7.2.4` | native AR* | X | X | X | X | X |
| `redis-8.8` | native AR* | 6.27M | 24.08 | 16.09 | 33.74 | 34.01 |
| `valkey-9.1` | native AR* | X | X | X | X | X |
| `dragonfly` | native AR* | X | X | X | X | X |
| `mini-redis-go-55178df` | native AR* | X | X | X | X | X |

### Operations

| Operation | `goblin-classic-resp-tcp` | `goblin-rt-resp-tcp` | `redis-7.2.4` | `redis-8.8` | `valkey-9.1` | `dragonfly` | `mini-redis-go-55178df` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| point read hit | 2.10M | 2.38M | X | 1.84M | X | X | X |
| point read miss | 2.39M | 2.76M | X | 2.01M | X | X | X |
| four-index read | 1.09M | 1.14M | X | 952K | X | X | X |
| populated count | 2.53M | 2.80M | X | 2.09M | X | X | X |
| logical length (`ARLEN`) | 2.55M | 2.97M | X | 2.06M | X | X | X |
| same-size update | 1.70M | 2.02M | X | 1.43M | X | X | X |
| delete + restore | 957K | 1.08M | X | 742K | X | X | X |
| allocate next index (`ARINSERT`) | 1.85M | 2.08M | X | 1.49M | X | X | X |

## Sparse Indexes, Stride 16

### Population And Memory

| Engine | representation | values/s | RSS MiB | RSS delta MiB | RSS delta B/value | key-reported B/value |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `goblin-classic-resp-tcp` | Classic AR* | 4.10M | 26.19 | 14.61 | 30.64 | 30.24 |
| `goblin-rt-resp-tcp` | RT AR* | 4.27M | 54.36 | 42.79 | 89.73 | 87.49 |
| `redis-7.2.4` | native AR* | X | X | X | X | X |
| `redis-8.8` | native AR* | 3.37M | 88.85 | 80.86 | 169.57 | 184.03 |
| `valkey-9.1` | native AR* | X | X | X | X | X |
| `dragonfly` | native AR* | X | X | X | X | X |
| `mini-redis-go-55178df` | native AR* | X | X | X | X | X |

### Operations

| Operation | `goblin-classic-resp-tcp` | `goblin-rt-resp-tcp` | `redis-7.2.4` | `redis-8.8` | `valkey-9.1` | `dragonfly` | `mini-redis-go-55178df` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| point read hit | 1.99M | 2.37M | X | 1.85M | X | X | X |
| point read miss | 2.38M | 2.76M | X | 1.99M | X | X | X |
| four-index read | 1.03M | 1.12M | X | 966K | X | X | X |
| populated count | 2.53M | 2.82M | X | 2.10M | X | X | X |
| logical length (`ARLEN`) | 2.53M | 2.97M | X | 2.07M | X | X | X |
| same-size update | 1.63M | 1.98M | X | 1.40M | X | X | X |
| delete + restore | 882K | 1.08M | X | 732K | X | X | X |
| allocate next index (`ARINSERT`) | 1.74M | 2.09M | X | 1.45M | X | X | X |

## Interpretation

Classic and RT solve different deployment problems. Classic is the memory-oriented choice for sparse or evolving index ranges. RT fixes each touched leaf at its final size and never rebuilds its fixed-depth directory; an out-of-range write fails. Its ordinary value arena grows 2x to reduce relocations. `GOBLIN.RT.ARRESERVE` goes further by allocating and prefaulting the declared index, metadata, and byte capacity before serving, then failing closed at any bound instead of allocating from a timed mutation.

All comparison rows exercise native AR commands. A row marked `X` does not expose the required command; the harness never substitutes hashes or times an error reply as useful work.
