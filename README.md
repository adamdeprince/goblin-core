# Goblin Core

Goblin Core is a Redis-like server aimed at lower memory overhead and high
throughput. The initial implementation focuses on sorted sets with a
vector-backed layout and a small RESP command surface.

Goblin Core is licensed under the Apache License, Version 2.0. See `LICENSE` and
`NOTICE`.

## Current Commands

- `PING [message]`
- `ZCARD key`
- `ZADD key score member [score member ...]`
- `ZRANGE key start stop [WITHSCORES]`
- `ZRANK key member`
- `ZREVRANGE key start stop [WITHSCORES]`
- `ZREVRANK key member`
- `ZREM key member [member ...]`
- `ZSCORE key member`

The protocol handler accepts RESP array commands and a basic inline command
format for local testing.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

Redis differential tests are optional because they start local Goblin Core and
Redis processes. The test script is deterministic: pass the same seed,
pipeline depth, and workload arguments to reproduce a failure.

```sh
cmake -S . -B build-redis-tests -DGOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS=ON
cmake --build build-redis-tests
ctest --test-dir build-redis-tests --output-on-failure
```

Run the pipelined differential test directly:

```sh
python3 tests/redis_differential.py \
  --goblin-bin build/goblin-core \
  --pipeline-depth 32 \
  --seed 12345
```

Install into a prefix:

```sh
cmake --install build --prefix /usr/local
```

## Run

```sh
./build/goblin-core --port 6379
```

`--rank-cache` enables an optional member-id-to-score-location cache for faster
`ZRANK` lookups at roughly 4 bytes per member. It is off by default because it
adds update/remove maintenance work.

`--score-string-cache` enables an experimental RESP-ready score text cache for
range output benchmarking. It is off by default because it adds a packed side
arena and an 8-byte score-text reference per member; measured default workloads
prefer direct stack-buffer score serialization.

`--max-output-buffer-mib` bounds per-client queued response bytes before the
server pauses reads from that socket. The default is `1`; `0` disables the
limit for comparison runs. Reads resume after queued output drains below one
quarter of the configured limit.

`--initial-output-buffer-kib` reserves per-client response-buffer capacity at
accept time. The default is `0`; keep it off unless a cold first-burst workload
benefits enough to justify the per-connection memory.

Example:

```sh
redis-cli -p 6379 ZADD leaders 42 alice 17 bob
redis-cli -p 6379 ZRANGE leaders 0 -1 WITHSCORES
```

## Benchmark

The scripted benchmark workflow configures a release build, runs the tests,
starts Goblin Core and Redis on temporary localhost ports, drives both over RESP,
and writes JSON results plus a Markdown report. Redis also reports
`INFO memory` `used_memory`. Defaults are intentionally large (`1,000,000`
loaded members and `1,000,000` read/range ops) to reduce the effect of static
code and runtime baseline RSS.

Current Goblin Core-vs-Redis results are summarized in the
[benchmark report](BENCHMARKS.md).

```sh
python3 benchmarks/run_benchmarks.py \
  --redis-server /opt/homebrew/bin/redis-server \
  --report BENCHMARKS.md \
  --name redis-goblin-core-1m \
  --latency-samples 10000
```

`--latency-samples` enables per-command latency rows for `ZSCORE`, `ZRANK`,
`ZREVRANK`, `ZRANGE`, and `ZREVRANGE`. Latency sampling defaults to
single-command round trips with `--latency-pipeline-depth 1` so percentiles are
not hidden by the throughput pipeline.

Redis is required for comparison mode. To run a quick Goblin Core-only validation:

```sh
python3 benchmarks/run_benchmarks.py \
  --target goblin \
  --members 10000 \
  --ops 10000 \
  --remove-members 5000 \
  --latency-samples 1000 \
  --skip-rank-cache
```

For targeted experiments, the lower-level harness remains available:

```sh
python3 benchmarks/zset_benchmark.py --target goblin --goblin-rank-cache
```

Add `--goblin-score-string-cache` to measure the optional cached score-output
path in either `benchmarks/run_benchmarks.py` or `benchmarks/zset_benchmark.py`.
Add `--score-shape integer|short-decimal|long-decimal|random-double` to run
server benchmarks with the same score distributions as the microbench.

The in-process microbenchmark separates data-structure cost from RESP and
socket overhead. It builds a local zset/store fixture, then times raw `ZSet`
lookups/ranks/ranges, RESP serialization, command execution without socket I/O,
and parse+execute dispatch:

```sh
cmake --build build-release --target goblin_core_microbench
./build-release/goblin_core_microbench \
  --members 1000000 \
  --ops 1000000 \
  --score-shape integer \
  --format json \
  --output benchmark-results/microbench.json
```

Use `--score-shape integer|short-decimal|long-decimal|random-double` to
separate score-formatting behavior from zset traversal and output costs.

To regenerate the current microbenchmark summary after running both rank-cache
modes:

```sh
python3 benchmarks/report_microbench.py --output MICROBENCHMARKS.md
```

For a server-output stress test that sends large bursts of range commands before
reading responses and records final plus peak RSS:

```sh
python3 benchmarks/range_output_benchmark.py \
  --output-json benchmark-results/range-output.json \
  --report benchmark-results/range-output.md
```

To sweep Redis/Goblin range throughput across multiple range sizes:

```sh
python3 benchmarks/range_size_sweep.py \
  --score-shape integer \
  --range-sizes 1 4 16 64 256 1024 \
  --output-json benchmark-results/range-size-sweep.json \
  --report benchmark-results/range-size-sweep.md
```

To sweep server-output throughput across range sizes while the client skips
RESP payload materialization:

```sh
python3 benchmarks/range_output_sweep.py \
  --score-shape integer \
  --range-sizes 16 64 256 1024 \
  --warmup-ops 4096 \
  --output-json benchmark-results/range-output-sweep.json \
  --report benchmark-results/range-output-sweep.md
```

Use `--warmup-ops` to exclude first-burst output-buffer growth from steady-state
range-output comparisons. Use `--variant-order` when checking for metric-order
effects. Use `--goblin-initial-output-buffer-kib` to test whether pre-reserving
per-client output capacity helps a cold burst.

To sweep `ZADD` load and `ZREM` removal throughput across set sizes:

```sh
python3 benchmarks/update_scaling_sweep.py \
  --member-counts 10000 100000 1000000 \
  --remove-fraction 0.5 \
  --output-json benchmark-results/update-scaling-sweep.json \
  --report benchmark-results/update-scaling-sweep.md
```

To isolate `ZREM` behavior across set sizes and removal fractions:

```sh
python3 benchmarks/zrem_shape_sweep.py \
  --member-counts 50000 100000 200000 \
  --remove-fractions 0.01 0.1 0.5 0.9 \
  --remove-orders load-prefix load-suffix reshuffled \
  --output-json benchmark-results/zrem-shape-sweep.json \
  --report benchmark-results/zrem-shape-sweep.md
```

To measure read throughput after deletion churn:

```sh
python3 benchmarks/post_delete_read_benchmark.py \
  --members 1000000 \
  --ops 1000000 \
  --remove-fraction 0.5 \
  --latency-samples 10000 \
  --output-json benchmark-results/post-delete-read.json \
  --report benchmark-results/post-delete-read.md
```

To run an interleaved leaderboard workload with reads, top ranges, score
updates, and remove/add churn:

```sh
python3 benchmarks/mixed_leaderboard_benchmark.py \
  --members 1000000 \
  --ops 1000000 \
  --range-size 100 \
  --latency-samples 10000 \
  --output-json benchmark-results/mixed-leaderboard.json \
  --report benchmark-results/mixed-leaderboard.md
```

## Package

The current distribution path is a versioned CPack archive containing
`goblin-core`, headers, the static core library, CMake package files, and project
documentation:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target package
```

This produces `.tar.gz` and `.zip` artifacts under `build-release/`. That is
the right first packaging format while Goblin Core is still pre-1.0: it is simple,
scriptable, works for CI release uploads, and does not force an ABI or service
manager contract too early. Once releases stabilize, add a Homebrew formula for
macOS and distro-native `.deb`/`.rpm` packages for Linux deployments.

## HTML Docs

The build converts root Markdown docs into static HTML under `html/`.
`README.md` becomes `html/index.html`, and Markdown links between docs are
rewritten to HTML links with human-visible labels that do not end in `.md`.

```sh
python3 scripts/build_html_docs.py --output html
```

The `html/` directory is generated and ignored. If a binary artifact under
`html/` is intentionally force-added for distribution, `.gitattributes` routes
it through Git LFS.

## Layout

- `include/goblin/core/resp_parser.hpp`: incremental RESP and inline parser
- `include/goblin/core/resp_writer.hpp`: RESP response serialization
- `include/goblin/core/command.hpp`: command parsing and dispatch
- `include/goblin/core/store.hpp`: vector-backed sorted set store
- `include/goblin/core/chunked_sorted_list.hpp`: block-based sorted-list index for zsets
- `include/goblin/core/swiss_table.hpp`: portable Swiss-table hash map
- `include/goblin/core/zset_member_index.hpp`: packed member lookup index for zsets
- `include/goblin/core/zset_score_index.hpp`: packed score/member-id index for zsets
- `include/goblin/core/server.hpp`: nonblocking TCP server
- `include/goblin/core/simd.hpp`: SIMD capability scaffolding
- `scripts/build_html_docs.py`: Markdown to HTML documentation generator
- `tests/redis_differential.py`: Redis-backed zset differential test
- `benchmarks/run_benchmarks.py`: scripted build/test/benchmark/report workflow
- `benchmarks/zset_benchmark.py`: Goblin Core vs Redis zset benchmark
