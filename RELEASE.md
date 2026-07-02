# Goblin Core Release Checklist

This is the repeatable pre-release path for source and binary archive releases.
The package format is intentionally simple while Goblin Core is pre-1.0.

## Scope

- Supported Redis subset: `PING`, `ZCARD`, `ZADD`, `ZRANGE`, `ZRANK`,
  `ZREVRANGE`, `ZREVRANK`, `ZREM`, and `ZSCORE`.
- Unsupported Redis features are not compatibility bugs unless they affect the
  command subset above.
- `--rank-cache-mode off` is the default release configuration. `exact` and
  `block-hint` are benchmarked tunables, not default policy.

## Validation

Run from a clean source checkout:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
ctest --test-dir build-release --output-on-failure
scripts/benchmark_smoke.sh
cmake --build build-release --target package
```

For Redis-backed differential tests:

```sh
cmake -S . -B build-redis-tests -DGOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS=ON
cmake --build build-redis-tests
ctest --test-dir build-redis-tests --output-on-failure
```

## Artifacts

The release build produces:

- `build-release/goblin-core-<version>-<system>-<arch>.tar.gz`
- `build-release/goblin-core-<version>-<system>-<arch>.zip`

Inspect at least one archive before publishing:

```sh
tar -tzf build-release/goblin-core-*.tar.gz
```

The archive should contain the `goblin-core` executable, public headers, the
static core library, CMake package files, root Markdown docs, generated HTML
docs, `LICENSE`, and `NOTICE`.

## Versioning

- Update `project(... VERSION ...)` in `CMakeLists.txt` before a numbered
  release.
- Use pre-1.0 versions until the Redis-compatible surface and operational
  contract are broader.
- Tag the exact commit used to produce the published archives.
