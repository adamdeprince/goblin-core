# Goblin Core Release Checklist

Release the source, separate server and C++ client packages, and matching
Python client artifacts. Use the canonical version in root `CMakeLists.txt`.

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

Publish tested Homebrew formulas/bottles at `brew.goblinreactor.com` and
release-specific Debian packages at `apt.goblinreactor.com`. Their roots are
`hail::distribution/brew.goblinreactor.com` and
`hail::distribution/apt.goblinreactor.com`. Preserve existing repository
entries, use its existing signing key, and update the catalog and index.

The `goblin-core` service uses loopback RESP port 6379 and a Unix socket.
Do not enable SBE, rings or other high-performance transports in its default
configuration. `goblin-core-cpp` installs the C++ SDK independently.
See `packaging/README.md` for builds and installed-package validation.

Build the Python sdist and wheels from `python/`; their version is read from
root CMake rather than maintained separately. Check them with `twine check`.
**Do not upload anything to PyPI without Adam's explicit permission.**

The source tree should contain public headers, source files, CMake install
rules, root Markdown docs, generated-doc inputs, `LICENSE`, and `NOTICE`.

## Versioning

- Update `project(... VERSION ...)` in `CMakeLists.txt` before a numbered
  release.
- Use pre-1.0 versions while the supported surface is a subset of Redis.
- Tag the exact commit used for the source release.
