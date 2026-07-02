# Goblin Core Source Release Checklist

This is the repeatable pre-release path for source-only releases. Do not publish
compiled binary artifacts at this stage; users should build Goblin Core locally
from the tagged source.

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
```

For Redis-backed differential tests:

```sh
cmake -S . -B build-redis-tests -DGOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS=ON
cmake --build build-redis-tests
ctest --test-dir build-redis-tests --output-on-failure
```

## Publication

Publish:

- The signed or annotated git tag.
- The hosting service's generated source archive for that tag.

Do not publish:

- Compiled `.tar.gz` or `.zip` archives from `build-release/`.
- Homebrew, `.deb`, `.rpm`, container, or service-manager packages.

The source tree should contain public headers, source files, CMake install
rules, root Markdown docs, generated-doc inputs, `LICENSE`, and `NOTICE`.

## Versioning

- Update `project(... VERSION ...)` in `CMakeLists.txt` before a numbered
  release.
- Use pre-1.0 versions until the Redis-compatible surface and operational
  contract are broader.
- Tag the exact commit used for the source release.
