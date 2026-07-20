# Installing Goblin Core on `thunder`

This is the deployment record for the single-node Goblin Core and Redpanda
test installation on `thunder`. Redpanda owns the durable write log; Goblin
Core consumes and produces canonical RESP2 mutations through Kafka.

The deployed LAN endpoints are:

- `thunder`: `192.168.1.49/24`
- Redpanda Kafka API: `192.168.1.49:9092`
- Redpanda Admin API: `192.168.1.49:9644`
- Goblin Core RESP: `127.0.0.1:6379`

The Kafka and Admin APIs are plaintext services on the private test LAN. Do not
expose them to an untrusted network without adding the appropriate Redpanda
authentication and TLS configuration.

## 1. Prepare the local disk

`/mnt/local` is a persistent local XFS filesystem on the Intel NVMe device. The
NFS-mounted home directory is useful for sharing source and toolchains between
`thunder` and `butterfly`, but it is not the database data path.

```sh
findmnt -T /mnt/local -o TARGET,SOURCE,FSTYPE,OPTIONS
df -hT /mnt/local
sudo fstrim -v /mnt/local
```

Redpanda's default disk alert fires below 5% free space. The first start on this
host correctly reported `high_disk_usage_nodes` because `/mnt/local` was 97%
full. Free enough space before relying on the broker; after cleanup this host
had about 729 GiB free.

```sh
rpk -X brokers=192.168.1.49:9092 \
  -X admin.hosts=192.168.1.49:9644 cluster health
```

Do not lower the watermark merely to make the health report green.

## 2. Give `thunder` a stable LAN address

The local DHCP pool starts at `.128`, so `192.168.1.49` is reserved for the
broker host. Verify that it is unused from at least one other machine before
assigning it:

```sh
ping -c 3 -W 1 192.168.1.49
ip neigh show 192.168.1.49
```

Configure `eno1` through Netplan with:

```yaml
network:
  version: 2
  ethernets:
    eno1:
      addresses:
        - 192.168.1.49/24
      dhcp4: false
      nameservers:
        addresses:
          - 192.168.1.1
      routes:
        - to: default
          via: 192.168.1.1
```

Back up the previous configuration, restrict its permissions, generate it, and
use Netplan's timed rollback before accepting the address:

```sh
sudo cp --preserve=all /etc/netplan/00-installer-config.yaml \
  /etc/netplan/00-installer-config.yaml.pre-static-49
sudo chmod 600 /etc/netplan/*.yaml
sudo netplan generate
sudo netplan try --timeout 60
```

From `butterfly`, verify that the route is the physical LAN and not Tailscale:

```sh
ip route get 192.168.1.49
ping -c 2 192.168.1.49
nc -vz -w 3 192.168.1.49 22
```

Changing a live NFS client's address can leave the existing NFSv4 mount showing
the old `clientaddr` until a full remount or reboot. Reads remained healthy in
this deployment. Check them explicitly and let the next planned reboot create
a fresh session rather than forcing an unmount while jobs use `/home`:

```sh
findmnt -T /home/adam -o TARGET,SOURCE,FSTYPE,OPTIONS
timeout 5 stat /home/adam/CMakeLists.txt
```

## 3. Install Redpanda

`thunder` runs Ubuntu 22.04 on x86-64. Install Redpanda from its signed APT
repository. This deployment reused the already-verified key and source file
from `butterfly`:

```sh
scp adam@192.168.1.46:/usr/share/keyrings/artifact-registry.gpg /tmp/
scp adam@192.168.1.46:/etc/apt/sources.list.d/redpanda.list /tmp/
sudo install -o root -g root -m 0644 /tmp/artifact-registry.gpg \
  /usr/share/keyrings/artifact-registry.gpg
sudo install -o root -g root -m 0644 /tmp/redpanda.list \
  /etc/apt/sources.list.d/redpanda.list
sudo apt-get update
sudo apt-get install -y \
  redpanda=26.1.13-1 redpanda-rpk=26.1.13-1 redpanda-tuner=26.1.13-1
```

The source definition used here is:

```text
deb [signed-by=/usr/share/keyrings/artifact-registry.gpg] https://linux.pkg.redpanda.com/apt redpanda-apt main
```

Create a Redpanda-only directory on the local XFS device:

```sh
sudo install -d -o redpanda -g redpanda -m 0750 \
  /mnt/local/redpanda /mnt/local/redpanda/data
```

Bootstrap one broker, move its data path, and select production mode. Production
mode keeps normal durable fsync behavior; do not use `--unsafe-bypass-fsync` or
development mode for replication testing.

```sh
sudo rpk redpanda config bootstrap \
  --self 192.168.1.49 \
  --advertised-kafka 192.168.1.49:9092
sudo rpk redpanda config set \
  redpanda.data_directory /mnt/local/redpanda/data
sudo rpk redpanda config set redpanda.empty_seed_starts_cluster true
sudo rpk redpanda mode production
```

Use the following resource boundary in `/etc/default/redpanda`. This is a shared
96-thread, four-socket test host; Redpanda should not claim the whole machine.

```sh
START_ARGS="--overprovisioned --smp=4 --memory=8G --default-log-level=info"
```

The production mode command enables tuner selections in the YAML. The host-wide
tuner is deliberately disabled on this shared machine:

```sh
sudo systemctl disable --now redpanda-tuner
sudo systemctl enable --now redpanda
```

Verify durable mode, local storage, and the advertised address:

```sh
rpk -X brokers=192.168.1.49:9092 cluster info
rpk -X brokers=192.168.1.49:9092 cluster config get write_caching_default
rpk -X brokers=192.168.1.49:9092 \
  -X admin.hosts=192.168.1.49:9644 cluster health
findmnt -T /mnt/local/redpanda/data -o TARGET,SOURCE,FSTYPE,OPTIONS
ps -C redpanda -o cmd=
```

`write_caching_default` must be `false`, and the process command line must not
contain `--unsafe-bypass-fsync`.

## 4. Install Goblin Core build dependencies

The system CMake and GCC on Ubuntu 22.04 are too old for this tree. The shared
home already contains GCC 16.1 and CMake 3.28.6:

```text
/home/adam/opt/gcc-16.1/bin/gcc
/home/adam/opt/gcc-16.1/bin/g++
/home/adam/opt/cmake-3.28.6-linux-x86_64/bin/cmake
```

Install the host libraries used by the default Kafka, TLS, compression, and
RDMA build:

```sh
sudo apt-get install -y \
  libsodium-dev liblz4-dev libssl-dev libibverbs-dev librdmacm-dev ninja-build
```

Kafka support uses the BSD-2-Clause librdkafka sources already vendored in the
Goblin Core tree; no system librdkafka package is needed.

## 5. Build a committed revision on local NVMe

Confirm that the source worktree is clean and commit all intended changes
before deployment:

```sh
git status --short --branch
git diff --check
git rev-parse HEAD
```

Export that commit on the development workstation, copy the archive into the
shared home, then extract and build it under `/mnt/local`. Keeping the archive
name tied to the short commit makes the running source unambiguous without
compiling on NFS.

```sh
git archive --format=tar.gz \
  --prefix=goblin-core-<commit>/ \
  --output=/tmp/goblin-core-<commit>.tar.gz HEAD
scp /tmp/goblin-core-<commit>.tar.gz thunder:/home/adam/

# The remaining commands run on thunder.
tar -xzf /home/adam/goblin-core-<commit>.tar.gz -C /mnt/local

/home/adam/opt/cmake-3.28.6-linux-x86_64/bin/cmake \
  -S /mnt/local/goblin-core-<commit> \
  -B /mnt/local/goblin-core-build-<commit> \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/home/adam/opt/gcc-16.1/bin/gcc \
  -DCMAKE_CXX_COMPILER=/home/adam/opt/gcc-16.1/bin/g++ \
  -DGOBLIN_CORE_ARCH=native \
  -DGOBLIN_CORE_ENABLE_KAFKA=ON \
  -DGOBLIN_CORE_ENABLE_RDMA=ON \
  -DGOBLIN_CORE_STATIC_GNU_RUNTIME=ON \
  -DGOBLIN_CORE_BUILD_TESTS=ON \
  -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF \
  -DGOBLIN_CORE_BUILD_HTML_DOCS=OFF

/home/adam/opt/cmake-3.28.6-linux-x86_64/bin/cmake \
  --build /mnt/local/goblin-core-build-<commit> --parallel 24
```

Ubuntu 22.04's system `libstdc++` is older than the runtime required by GCC
16. `GOBLIN_CORE_STATIC_GNU_RUNTIME` is on by default and embeds `libstdc++`
and `libgcc` in each C++ executable. Keep glibc, OpenSSL, and the RDMA provider
libraries dynamic, then verify that the binary has no dependency on either GCC
runtime:

```sh
ldd /mnt/local/goblin-core-build-<commit>/goblin-core
```

Run at least the Kafka unit test before starting the server:

```sh
ctest --test-dir /mnt/local/goblin-core-build-<commit> \
  --output-on-failure -R kafka
```

## 6. Create the replication topic and run Goblin Core

Goblin requires one total Kafka order, so the topic must have exactly one
partition:

```sh
rpk -X brokers=192.168.1.49:9092 topic create goblin-core-replication \
  --partitions 1 --replicas 1
```

Start the committed binary with Redpanda's static LAN address:

```sh
/mnt/local/goblin-core-build-<commit>/goblin-core \
  --port 6379 \
  --kafka 'kafka://192.168.1.49:9092/goblin-core-replication'
```

Only the origin primary writes the Kafka journal. A Goblin process configured
with an upstream firehose consumes Kafka for recovery but does not produce a
second copy of an upstream mutation.

## 7. Verify journal and replay

Write through Goblin and confirm the value appears in both Goblin and the Kafka
record stream:

```sh
redis-cli -h 127.0.0.1 -p 6379 SET thunder:replication-smoke alive
redis-cli -h 127.0.0.1 -p 6379 GET thunder:replication-smoke
rpk -X brokers=192.168.1.49:9092 topic consume goblin-core-replication \
  --offset start --num 1 --format '%v\n'
```

Finally, stop and restart Goblin without a snapshot. Its initial Kafka replay
must finish before the RESP listener opens, and the value must be restored:

```sh
redis-cli -h 127.0.0.1 -p 6379 GET thunder:replication-smoke
```

For a stronger storage check, produce with `acks=all`, restart Redpanda, and
consume the record again. That test was also used during this installation.
