# Building and running the Goblin Core container

The repository includes a multi-stage `Containerfile` for building a local OCI
image with Podman. Podman is daemonless and does not require Docker Engine or
Docker Desktop. There is not yet an official registry-hosted Goblin Core image.

The image build enables Kafka, TLS, SBE, and RDMA, while leaving XLIO Ultra and
ExaSock disabled. Tests, benchmarks, generated HTML, compilers, and development
headers stay out of the runtime stage. The default build has no host-specific
ISA tuning, so it can move between compatible machines of the same
architecture. `/usr/share/doc/goblin-core` contains the project license and
notice plus the selected licenses for every dependency linked into the image.

## Build with Podman

Install Podman with the host package manager. On Ubuntu:

```sh
sudo apt-get update
sudo apt-get install -y podman
```

From the repository root:

```sh
podman build --format oci \
  --build-arg GOBLIN_CORE_VERSION=0.10.3 \
  --build-arg GOBLIN_CORE_REVISION="$(git rev-parse HEAD)" \
  --file Containerfile \
  --tag localhost/goblin-core:0.10.3 \
  .

podman tag localhost/goblin-core:0.10.3 localhost/goblin-core:latest
```

Pass `--build-arg GOBLIN_CORE_ARCH=native` only when the image will stay on
machines with the same instruction set as the build host. `avx2`, `avx512`,
`lsx`, and `lasx` are also accepted. The portable empty default is safer for an
image that will move between hosts.

## Run a local server

Goblin Core runs as the non-root `goblin` user (UID/GID 10001), works from
`/data`, and listens on container port 6379. When no listener is supplied, the
image entrypoint resolves its concrete container address and creates a
plaintext trusted listener there; Goblin Core also preserves its mandatory
`127.0.0.1` listener inside the container. A container's internal loopback
listener is not reachable through port publishing, so bind the published host
port to loopback:

```sh
podman volume create goblin-core-data

podman run --detach \
  --name goblin-core \
  --publish 127.0.0.1:6379:6379 \
  --volume goblin-core-data:/data:U \
  --ulimit memlock=-1:-1 \
  --cap-add IPC_LOCK \
  localhost/goblin-core:0.10.3
```

Confirm the server from the host with any RESP2 client:

```sh
redis-cli -h 127.0.0.1 -p 6379 PING
```

The response must be `PONG`. Stop and remove the container without removing its
data volume:

```sh
podman stop goblin-core
podman rm goblin-core
```

The Podman `:U` volume option assigns the named volume to the image's non-root
UID/GID. Do not apply `:U` casually to a host bind mount because Podman will
change ownership recursively on the host.

`--ulimit memlock=-1:-1` lets Goblin Core lock current and future mappings.
`--cap-add IPC_LOCK` makes that intent explicit inside the container. The host
must permit the requested locked-memory limit.

## Enforce both memory boundaries

`--maxmemory` is Goblin Core's deterministic allocation ceiling and never
evicts data. A container memory limit is a separate last line of defense and
must leave room for executable mappings, client buffers, the allocator, Kafka,
and snapshot copy-on-write pages:

```sh
podman run --detach \
  --name goblin-core \
  --memory 4g \
  --publish 127.0.0.1:6379:6379 \
  --volume goblin-core-data:/data:U \
  --ulimit memlock=-1:-1 \
  --cap-add IPC_LOCK \
  localhost/goblin-core:0.10.3 \
  --maxmemory 3gb
```

Unless the arguments already contain a TCP listener, the entrypoint prepends
its container-address trusted listener. Custom storage, memory, Kafka, or
replication options therefore do not require a dynamically discovered address.
An explicit `--listen`, `--trusted-listen`, `--tcp-listen`, `--bind`, or
`--port` takes precedence.

## Persistence

Mount persistent storage at `/data`. `SAVE /data/goblin.snapshot` writes a
synchronous native snapshot; `BGSAVE /data/goblin.snapshot` forks and writes it
in the background. Restart with:

```sh
podman run --detach \
  --name goblin-core \
  --publish 127.0.0.1:6379:6379 \
  --volume goblin-core-data:/data:U \
  --ulimit memlock=-1:-1 \
  --cap-add IPC_LOCK \
  localhost/goblin-core:0.10.3 \
  --load /data/goblin.snapshot
```

Arena HugeTLB mode deliberately disables fork-based `BGSAVE`; use synchronous
`SAVE` in that configuration. See the
[SAVE](commands/SAVE.md), [BGSAVE](commands/BGSAVE.md), and
[Kafka recovery](kafka.md) documentation for the durability contracts.

## Network exposure and TLS

The default `--trusted-listen 0.0.0.0:6379` is plaintext. Keep the published
port on host loopback or an isolated trusted network. For a non-loopback
boundary, mount the certificate and key read-only and replace the default
listener:

```sh
podman run --detach \
  --name goblin-core \
  --publish 6379:6379 \
  --volume goblin-core-data:/data:U \
  --volume ./certs:/certs:ro \
  --ulimit memlock=-1:-1 \
  --cap-add IPC_LOCK \
  localhost/goblin-core:0.10.3 \
  --listen 0.0.0.0:6379 \
  --tls-cert-file /certs/server.crt \
  --tls-key-file /certs/server.key
```

Authentication, SBE, ring buffers, Kafka, replication, NUMA placement, and
transport flags are ordinary `goblin-core` arguments. The focused references
are linked from the [documentation index](index.md).

## RDMA and host devices

The runtime contains the userspace verbs and RDMA libraries, but a container
does not automatically receive host RDMA devices. Pass the required
`/dev/infiniband/*` devices and the corresponding network interface according
to the host's Podman and RDMA configuration. Pin the process with `--numa` or
`--cpu` after exposing the device. RDMA, HugeTLB, and low-latency production
placement should be qualified on the target host rather than assumed from a
generic container launch.

Inspect the image without running it:

```sh
podman image inspect localhost/goblin-core:0.10.3
podman history localhost/goblin-core:0.10.3
```
