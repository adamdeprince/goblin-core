ARG UBUNTU_VERSION=26.04

FROM docker.io/library/ubuntu:${UBUNTU_VERSION} AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG GOBLIN_CORE_ARCH=

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        gcc \
        libibverbs-dev \
        liblz4-dev \
        librdmacm-dev \
        libsodium-dev \
        libssl-dev \
        ninja-build \
        pkg-config \
        python3 \
        rdma-core \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY LICENSE NOTICE ./
COPY INSTALL.md README.md RELEASE.md ./
COPY BENCHMARKS.md BLUEFIELD-BENCHMARK.md MICROBENCHMARKS.md ./
COPY PERFORMANCE_BRIEF.md PUBSUB-BENCHMARK.md ./
COPY EFA-LATENCY.md AWS-EFA-LATENCY.md XLIO-LATENCY.md ./
COPY cmake/ cmake/
COPY include/ include/
COPY sbe/ sbe/
COPY src/ src/
COPY third_party/ third_party/

RUN cmake -S . -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DGOBLIN_CORE_ARCH="${GOBLIN_CORE_ARCH}" \
        -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF \
        -DGOBLIN_CORE_BUILD_BLUEFIELD=OFF \
        -DGOBLIN_CORE_BUILD_HTML_DOCS=OFF \
        -DGOBLIN_CORE_BUILD_TESTS=OFF \
        -DGOBLIN_CORE_ENABLE_EXASOCK=OFF \
        -DGOBLIN_CORE_ENABLE_KAFKA=ON \
        -DGOBLIN_CORE_ENABLE_RDMA=ON \
        -DGOBLIN_CORE_ENABLE_TLS=ON \
        -DGOBLIN_CORE_ENABLE_XLIO=OFF \
    && cmake --build /build --parallel \
    && cmake --install /build --prefix /stage

FROM docker.io/library/ubuntu:${UBUNTU_VERSION}

ARG DEBIAN_FRONTEND=noninteractive
ARG GOBLIN_CORE_REVISION=unknown
ARG GOBLIN_CORE_VERSION=development

LABEL org.opencontainers.image.description="Memory-efficient Redis semantics with RESP, SBE, Kafka, and RDMA" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.revision="${GOBLIN_CORE_REVISION}" \
      org.opencontainers.image.source="https://github.com/adamdeprince/goblin-core" \
      org.opencontainers.image.title="Goblin Core" \
      org.opencontainers.image.version="${GOBLIN_CORE_VERSION}"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        ibverbs-providers \
        libibverbs1 \
        liblz4-1 \
        librdmacm1t64 \
        libsodium23 \
        libssl3t64 \
        tini \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 --system goblin \
    && useradd --gid goblin --home-dir /data --no-create-home \
        --shell /usr/sbin/nologin --system --uid 10001 goblin \
    && install -d -m 0750 -o goblin -g goblin /data \
    && install -d -m 0755 /usr/share/doc/goblin-core

COPY --from=build \
    /stage/bin/goblin-core \
    /stage/bin/goblin-core-auth \
    /stage/bin/redis-cli-rdma \
    /stage/bin/redis-cli-ring \
    /usr/local/bin/
COPY --from=build /stage/share/doc/goblin-core/ /usr/share/doc/goblin-core/
COPY --chmod=0755 scripts/container-entrypoint.sh \
    /usr/local/bin/goblin-core-container-entrypoint

USER goblin:goblin
WORKDIR /data

EXPOSE 6379
STOPSIGNAL SIGTERM

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/goblin-core-container-entrypoint"]
