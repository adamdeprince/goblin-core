# Goblin Core packages

The server and C++ SDK are separate packages, both named after their install
targets: `goblin-core` and `goblin-core-cpp`. The Python distribution is built
from `python/` and is never bundled with either system package.

Build the Linux binaries inside the target distribution with CMake 3.25 or
newer. Select Release, disable tests/benchmarks/docs for packaging, and disable
RDMA, libfabric, ExaSock, XLIO, Aeron and Kafka integrations. OpenSSL TLS remains
available for explicit configuration but is not enabled by the service.
Runtime SIMD dispatch remains enabled; do not pass architecture-wide ISA flags.

```sh
cmake -S . -B build-package -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_BUILD_TESTS=OFF -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF \
  -DGOBLIN_CORE_BUILD_HTML_DOCS=OFF -DGOBLIN_CORE_ENABLE_KAFKA=OFF \
  -DGOBLIN_CORE_ENABLE_RDMA=OFF
cmake --build build-package --target goblin_core_server goblin_core_auth -j4
python3 scripts/package_deb.py --build build-package --output packages --suite noble
```

The Debian builder calculates runtime library dependencies in that environment.
It installs a dedicated system user, a systemd unit, the default configuration
and a service wrapper. The C++ SDK exports the header-only `goblin::client`
CMake target. Homebrew recipes are in `packaging/homebrew/`; their source and
bottle hashes must match the immutable artifacts before publication.

Validate installed packages, not just build outputs. Exercise PING and a
SET/GET round trip over both TCP and the default Unix socket. Check socket
permissions and compile a program using the installed SDK's CMake export.
Run tests on disposable ports or inside isolated containers so a local Redis
or Goblin Core instance is unaffected.

Generate project HTML with `python3 scripts/build_html_docs.py --output html`.
Publish with the repository locks on hail, preserve existing packages, sign
APT metadata and catalogs with its existing archive key, and verify a clean
install through each public repository before listing a target as tested.

Build Python artifacts with `python -m build python`. The PEP 517 backend
bundles the required client sources in its sdist and reads the upstream version
from root CMake. Validate a wheel built from that sdist and run `twine check`.
No PyPI upload is authorized until Adam explicitly approves it.
