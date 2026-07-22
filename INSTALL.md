# Installing Goblin Core from source

Goblin Core is currently distributed as source. There are no official binary,
container, operating-system package, or service-manager releases. A release is
an annotated Git tag; compile the tag on the machine where it will run, or on a
compatible build host.

Linux is the production platform. macOS is supported for development and the
portable socket/shared-memory test suite. RDMA, XLIO Ultra, HugeTLB, CPU
isolation, and NUMA placement are Linux facilities.

The complete Linux workflow below was verified on Ubuntu 22.04.1 with CMake
3.28.6, GCC 16.1, and Ninja 1.10.1. The default build detected Kafka, OpenSSL
TLS, SBE, and InfiniBand/RDMA support; all 36 tests and the installed-program
smoke test passed.

## 1. Install the build prerequisites

Every build needs:

- CMake 3.25 or newer.
- Ninja or another CMake-supported build tool.
- A compiler and standard library with C++23 support. The current tree is
  verified with GCC 16 and Apple Clang 21; Ubuntu 22.04's GCC 11 and CMake 3.22
  are too old.
- LZ4 development headers and library.
- libsodium development headers and library.
- Python 3 when tests or HTML documentation are enabled.
- OpenSSL development headers when `GOBLIN_CORE_ENABLE_TLS=ON`, which is the
  default.

The scripting engines, SBE codecs, XXH3, fast_float, and the Kafka client are
already vendored in the source tree. The normal build does not fetch source
from the network and does not need a system `librdkafka` package.

### Ubuntu or Debian

On a distribution new enough to provide CMake 3.25 and a C++23 compiler:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build python3 \
  liblz4-dev libsodium-dev libssl-dev
```

For the optional RDMA transport, also install:

```sh
sudo apt-get install -y libibverbs-dev librdmacm-dev rdma-core
```

Ubuntu 22.04 needs a newer CMake and compiler than its base repository ships.
Install a current toolchain separately, then select it with
`-DCMAKE_C_COMPILER=/path/to/gcc` and
`-DCMAKE_CXX_COMPILER=/path/to/g++` during configuration. Put the newer CMake
ahead of `/usr/bin` in `PATH`, and confirm `cmake --version`, `gcc --version`,
and `g++ --version` before configuring a fresh build directory. The verified
Ubuntu 22.04 build used the versions listed above without replacing the system
compiler.

### Fedora

```sh
sudo dnf install \
  gcc gcc-c++ cmake ninja-build python3 \
  lz4-devel libsodium-devel openssl-devel
```

Add `libibverbs-devel librdmacm-devel rdma-core` for RDMA.

### macOS with Homebrew

```sh
xcode-select --install
brew install cmake ninja lz4 libsodium openssl@3
```

Homebrew keeps some libraries outside the compiler's default search path. Pass
`-DCMAKE_PREFIX_PATH="$(brew --prefix)"` in the configure command below.

## 2. Get a source release

Clone the current development tree:

```sh
git clone https://github.com/adamdeprince/goblin-core.git
cd goblin-core
```

The dependencies are committed under `third_party/`; there are no Git
submodules to initialize. For a reproducible deployment, check out an intended
source tag or commit before configuring; remain on `main` for changes made
since the latest tag. The `INSTALL.md` in each checkout is authoritative for
that revision's available build options and installed programs.

## 3. Configure a release build

This is the recommended first build on Linux. It builds the server, clients,
tests, and HTML documentation, but skips benchmark executables:

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF
```

On macOS, make the package prefix explicit and disable the Linux-only RDMA
probe:

```sh
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" \
  -DCMAKE_C_COMPILER="$(xcrun --find clang)" \
  -DCMAKE_CXX_COMPILER="$(xcrun --find clang++)" \
  -DGOBLIN_CORE_ENABLE_RDMA=OFF \
  -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF
```

CMake prints a summary of the enabled integrations. Read it before building.
In particular, confirm the lines for Kafka, TLS, SBE, and RDMA match the binary
you intend to deploy.

## 4. Build and test

```sh
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

The socket tests bind loopback ports. Run them in an environment that permits
local listeners. Tests involving real Redis are deliberately separate; enable
them with `GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS` only when a `redis-server`
binary is installed.

## 5. Install

For a user-local installation:

```sh
cmake --install build-release --prefix "$HOME/.local"
```

Or install system-wide:

```sh
sudo cmake --install build-release --prefix /usr/local
```

The base installation provides:

| Program | Purpose |
|---|---|
| `goblin-core` | Server |
| `goblin-core-auth` | Offline libsodium password-file editor |
| `redis-cli-ring` | RESP/SBE shared-memory ring client; also the XLIO client in an XLIO build |
| `redis-cli-rdma` | RDMA client, installed only when RDMA was compiled |

Headers, the CMake package, licenses, and documentation are installed under the
same prefix.

## 6. Smoke-test the installed programs

The server listens on plaintext `127.0.0.1:6379` by default. This example also
creates a local ring so the installation can be checked without a separate
Redis client:

```sh
"$HOME/.local/bin/goblin-core" \
  --ring /tmp/goblin-install-test.ring 1mb
```

In another terminal:

```sh
"$HOME/.local/bin/redis-cli-ring" \
  /tmp/goblin-install-test.ring PING
```

The reply must be `PONG`. Stop the server with `Ctrl-C`. An interrupted process
can leave the ring backing file behind; a later server invocation reinitializes
it, or it can be removed after confirming that no process is using it.

Goblin Core attempts to lock its current and future memory. A development shell
with a low `RLIMIT_MEMLOCK` may produce a warning but can still be used for a
functional smoke test. A production service should grant an unlimited locked
memory limit, for example `LimitMEMLOCK=infinity` in systemd.

## CMake option reference

Pass CMake booleans as `-DNAME=ON` or `-DNAME=OFF`. Changing an option in an
existing build directory is supported, but a separate directory per build
profile makes the resulting binary easier to audit.

| Option | Default | Effect |
|---|---:|---|
| `CMAKE_BUILD_TYPE` | unset | Use `Release` for deployment; single-config Ninja/Make builds otherwise lack release optimization. |
| `GOBLIN_CORE_BUILD_TESTS` | `ON` | Builds the C++ and socket integration tests and enables CTest. Requires Python 3 for script-driven tests. |
| `GOBLIN_CORE_BUILD_BENCHMARKS` | `ON` | Builds benchmark workers and probes. Turn it off for a smaller/faster installation build. |
| `GOBLIN_CORE_BUILD_HTML_DOCS` | `ON` | Converts the Markdown tree into the static site under `html/`. Requires Python 3. |
| `GOBLIN_CORE_ENABLE_KAFKA` | `ON` | Compiles vendored librdkafka and enables Kafka journaling/replay. No system librdkafka is needed. |
| `GOBLIN_CORE_ENABLE_TLS` | `ON` | Enables OpenSSL TLS for ordinary non-loopback TCP listeners and TLS replica connections. Requires OpenSSL development files. |
| `GOBLIN_CORE_ENABLE_RDMA` | `ON` | Enables polled RDMA rings on Linux when libibverbs and librdmacm are found. Missing libraries or a non-Linux host disable it with a CMake status message. |
| `GOBLIN_CORE_ENABLE_XLIO` | `OFF` | Enables native NVIDIA XLIO Ultra TCP on Linux. The vendored headers compile with Goblin, but the pinned DPCP/XLIO runtime must also be built and preloaded. |
| `GOBLIN_CORE_STATIC_GNU_RUNTIME` | `ON` | With GCC, links `libstdc++` and `libgcc` statically while leaving libc and device libraries dynamic. It has no effect with Clang/MSVC. |
| `GOBLIN_CORE_REDIS_DIFFERENTIAL_TESTS` | `OFF` | Adds sequential and pipelined compatibility tests against an installed `redis-server`. This is a test dependency, never a server runtime dependency. |
| `GOBLIN_CORE_ARCH` | empty | Selects additional ISA tuning: `native`, `avx2`, `avx512`, `lsx`, or `lasx`. Empty is the distribution-friendly choice. |

### Architecture selection

`GOBLIN_CORE_ARCH=native` lets GCC or Clang use every instruction available on
the build host and is appropriate when compiling on the deployment machine.
The explicit `avx2` and `avx512` profiles require matching x86 CPUs; `lsx` and
`lasx` target LoongArch. A binary built for an unsupported ISA can terminate
with an illegal-instruction fault. Leave the option empty when one binary must
run on unlike machines.

### Minimal portable build

For a localhost/UDS/shared-memory development server without Kafka, TLS, RDMA,
tests, benchmarks, or generated docs:

```sh
cmake -S . -B build-minimal -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ENABLE_KAFKA=OFF \
  -DGOBLIN_CORE_ENABLE_TLS=OFF \
  -DGOBLIN_CORE_ENABLE_RDMA=OFF \
  -DGOBLIN_CORE_BUILD_TESTS=OFF \
  -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF \
  -DGOBLIN_CORE_BUILD_HTML_DOCS=OFF
cmake --build build-minimal --parallel
```

Retain the platform-specific compiler and `CMAKE_PREFIX_PATH` arguments from
the recommended configure command when they apply to the host.

LZ4 and libsodium remain required: compact values and credential-file support
are core features even when network TLS and Kafka are disabled.

### Optimized same-host build

To optimize for the machine doing the compile:

```sh
cmake -S . -B build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ARCH=native \
  -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF
cmake --build build-native --parallel
```

Do not copy that binary to an older or architecturally different CPU without
rebuilding it.

## Optional integrations

### Kafka durability

The default build includes Kafka support. Runtime configuration starts with
`--kafka CONNECTION`; `--kafka-ack-mode queued|broker` chooses local producer
queue acknowledgement or broker-confirmed writes. See
[Kafka write log and recovery](docs/kafka.md). A complete lab deployment with
Redpanda is preserved separately in the
[Thunder durability deployment record](docs/thunder-redpanda-deployment.md).

### TLS and authentication

Non-loopback ordinary TCP listeners require TLS. Supply
`--tls-cert-file` and `--tls-key-file`, and use `goblin-core-auth` plus
`--auth-file` when RESP clients must authenticate. See
[TCP listeners and TLS](docs/tls.md) and
[authentication](docs/authentication.md).

### Shared-memory rings and SBE

Rings require no optional build flag. Add repeatable `--ring PATH SIZE` runtime
targets. SBE is compiled from the checked-in generated headers and is accepted
only when the server starts with `--enable-sbe`. See
[shared-memory rings](docs/ring-buffers.md) and the
[SBE protocol](docs/sbe-protocol.md).

### RDMA

Install libibverbs/librdmacm, leave `GOBLIN_CORE_ENABLE_RDMA=ON`, and confirm
CMake reports `GOBLIN_HAS_RDMA`. Runtime targets use
`--rdma ADDRESS PORT SIZE`. See [polled RDMA rings](docs/rdma-rings.md) and the
[InfiniBand setup guide](docs/infiniband-setup.md).

### XLIO Ultra

Set `GOBLIN_CORE_ENABLE_XLIO=ON` only after building the pinned DPCP and XLIO
runtime. Goblin must run with `libxlio.so` preloaded; runtime listeners use
`--xlio ADDRESS PORT`. See [native XLIO Ultra TCP](docs/xlio.md) for the exact
dependency build and launch environment.

## Troubleshooting

- **CMake rejects the project version:** install CMake 3.25 or newer and make
  sure that executable comes first in `PATH`.
- **The compiler rejects C++23 syntax:** select a newer compiler with
  `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER`, then configure a fresh build
  directory.
- **CMake names a compiler that is not installed:** clear stale `CC` and `CXX`
  environment variables, or override both compilers explicitly in a fresh
  build directory.
- **LZ4 or sodium is not found:** install the development package, not only the
  runtime library. Use `CMAKE_PREFIX_PATH` for a nonstandard prefix.
- **OpenSSL is not found:** install its development package, provide its prefix,
  or deliberately configure `GOBLIN_CORE_ENABLE_TLS=OFF` for a local-only build.
- **RDMA says disabled:** CMake did not find both verbs and RDMA-CM on Linux.
  Install their development packages and reconfigure.
- **`mlockall` warns at startup:** raise the shell or service locked-memory
  limit. This is an operating-system limit, not a compile failure.
- **The process reports an illegal instruction:** rebuild with an empty
  `GOBLIN_CORE_ARCH` or the correct explicit ISA.

For runtime listener, memory, replication, and storage options, continue with
the [documentation index](docs/index.md).
