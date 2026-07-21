# Goblin Core Source Release Checklist

The repeatable path for a source-only release. A release is a git tag and the
hosting service's source archive; users build Goblin Core from the tagged
source.

## Scope

- The supported Redis-compatible and Goblin-specific command surfaces are
  maintained in `README.md` and `docs/commands/`. Check both before release;
  pre-1.0 versions intentionally remain a growing subset rather than claiming
  complete Redis compatibility.
- Transport, authentication, replication, persistence, and operational limits
  are part of the release contract. Their canonical documents live under
  `docs/` and must match the parser's current command-line behavior.
- Unsupported Redis features are not compatibility bugs unless they affect the
  documented command subset.

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
- Use pre-1.0 versions while the supported surface is a subset of Redis.
- Tag the exact commit used for the source release.
