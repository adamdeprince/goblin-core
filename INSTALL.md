# Installing Goblin Core on `thunder`

This is the deployment record for the single-node Goblin Core and Redpanda
test installation on `thunder`. Redpanda owns the durable write log; Goblin
Core consumes and produces canonical RESP2 mutations through Kafka.

This procedure was verified on July 20, 2026 with Goblin Core revision
`6791e30` and Redpanda `26.1.13`.

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
  libsodium-dev liblz4-dev libssl-dev libibverbs-dev librdmacm-dev ninja-build \
  redis-tools
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
libraries dynamic, then verify that neither GCC runtime appears in the binary's
direct dynamic dependencies:

```sh
readelf -d /mnt/local/goblin-core-build-<commit>/goblin-core
```

Run the Kafka ingest, replication-state, and plaintext socket replication tests
before starting the server:

```sh
ctest --test-dir /mnt/local/goblin-core-build-<commit> \
  --output-on-failure \
  -R '^(goblin_core_kafka_ingest|goblin_core_replication|goblin_core_replication_socket)$'
```

The TLS replication test opens `[::]`. On `thunder`, that wildcard spans
Ethernet on NUMA node 0, InfiniBand on node 1, and Tailscale with unknown NUMA
placement. Goblin correctly refuses to guess a slice, so the current test
harness cannot start that server until it can pass an explicit `--numa` choice.

## 6. Create the replication topic and run Goblin Core

Goblin requires one total Kafka order, so the topic must have exactly one
partition:

```sh
rpk -X brokers=192.168.1.49:9092 topic create goblin-core-replication \
  --partitions 1 --replicas 1
```

Install the verified binary at a stable path. Before switching services, ask the
currently running Kafka-backed primary to write a native snapshot. The snapshot
contains both the keyspace and the acknowledged Kafka/replication offsets.
`GOBLIN.SAVE` writes a temporary file, fsyncs it, and atomically renames it into
place; wait for the final path to appear before stopping the old process.

```sh
sudo install -o root -g root -m 0755 \
  /mnt/local/goblin-core-build-<commit>/goblin-core \
  /usr/local/bin/goblin-core
sudo install -d -o adam -g adam -m 0750 /mnt/local/goblin-core/state

redis-cli -h 127.0.0.1 -p 6379 \
  GOBLIN.SAVE /mnt/local/goblin-core/state/goblin.snapshot ACCEL
stat /mnt/local/goblin-core/state/goblin.snapshot
```

`thunder` has network hardware on multiple NUMA nodes. Selecting `eno1` puts
Goblin on node 0 with its Ethernet path to Redpanda. Install the following as
`/etc/systemd/system/goblin-core.service`:

```ini
[Unit]
Description=Goblin Core with Redpanda replication journal
Requires=redpanda.service
After=network-online.target redpanda.service
Wants=network-online.target

[Service]
Type=simple
User=adam
Group=adam
WorkingDirectory=/mnt/local/goblin-core
ExecStart=/usr/local/bin/goblin-core --port 6379 --numa eno1 --load /mnt/local/goblin-core/state/goblin.snapshot --kafka kafka://192.168.1.49:9092/goblin-core-replication --kafka-ack-mode broker
Restart=on-failure
RestartSec=1s
TimeoutStopSec=30s
LimitMEMLOCK=infinity
UMask=0027

[Install]
WantedBy=multi-user.target
```

Stop any bootstrap/transient process, then enable the permanent service:

```sh
sudo systemctl daemon-reload
sudo systemctl stop goblin-core-thunder.service
sudo systemctl enable --now goblin-core.service

systemctl status goblin-core.service --no-pager --full
redis-cli -h 127.0.0.1 -p 6379 PING

pid=$(systemctl show goblin-core.service --property=MainPID --value)
grep '^VmLck:' /proc/"$pid"/status
```

With a native snapshot, the single `--kafka` option performs both halves of the
workflow: it consumes the retained journal before readiness, then produces each
new accepted write. The snapshot carries an exact broker offset, so this setup
does not need the timestamp fallback provided by `--kafka-time-buffer`.
`--kafka-ack-mode broker` makes the service recipe wait for Redpanda's delivery
acknowledgement before reporting a write as successful. Omit it, or select
`queued`, when local producer-queue acceptance is the intended lower-latency
contract.

Only the origin primary writes the Kafka journal. A Goblin process configured
with an upstream firehose consumes Kafka for recovery but does not produce a
second copy of an upstream mutation.

## 7. Verify journal and replay

The service must load the snapshot first, resume at its inclusive broker offset,
filter the boundary record already represented by the snapshot, and finish
Kafka replay before opening the RESP listener. Confirm the restored state and
offsets:

```sh
redis-cli -h 127.0.0.1 -p 6379 INFO replication
journalctl -u goblin-core.service --no-pager -n 30
```

Write a value after the snapshot, confirm that Goblin publishes its canonical
RESP2 mutation to Kafka, then restart. The value can only be recovered from the
journal because it is not in the snapshot used for this test.

```sh
redis-cli -h 127.0.0.1 -p 6379 \
  SET thunder:replication-live redpanda
rpk -X brokers=192.168.1.49:9092 topic consume goblin-core-replication \
  --offset 1 --num 1 --format '%v\n'
rpk -X brokers=192.168.1.49:9092 topic describe \
  goblin-core-replication -p

sudo systemctl restart goblin-core.service
redis-cli -h 127.0.0.1 -p 6379 GET thunder:replication-live
redis-cli -h 127.0.0.1 -p 6379 INFO replication
```

After that proof, refresh the normal startup snapshot and restart once more:

```sh
redis-cli -h 127.0.0.1 -p 6379 \
  GOBLIN.SAVE /mnt/local/goblin-core/state/goblin.snapshot ACCEL
stat /mnt/local/goblin-core/state/goblin.snapshot
sudo systemctl restart goblin-core.service
```

The verified deployment loaded two keys from the refreshed snapshot, resumed
inclusively at broker offset `1`, retained logical replication offset `2`, and
did not append a duplicate record. Redpanda's high watermark remained `2`.
The running Goblin process reported about 469 MiB in `VmLck`, confirming that
the systemd memlock limit took effect.

The configurable crash-recovery test is kept in
`benchmarks/external_logging_recovery.sh`. `--data-type` accepts `string`,
`hash`, `set`, `zset`, `list`, `array`, or `all`. The runner fills every selected
type to the snapshot boundary, saves, sends `SIGKILL` to the unit's `MainPID`,
and lets systemd recreate the process from its unchanged `ExecStart`. After
startup replay it verifies the recovered half, writes the remaining half, and
exhaustively verifies the final contents. The full unit definition and the
NUL-delimited `/proc/<pid>/cmdline` are byte-compared across the crash.

```sh
cmake --build build --target goblin_core_external_logging_recovery -j
RECOVERY_WORKER="$PWD/build/goblin_core_external_logging_recovery" \
  benchmarks/external_logging_recovery.sh \
    --data-type all --snapshot-at 500000 --count 1000000 --pipeline 512
```

To isolate one native type, change only the selector:

```sh
benchmarks/external_logging_recovery.sh --data-type zset
```

The first Thunder HSET-only validation populated one million fields in 5.09
seconds and recovered every field after a hard crash. The expanded validation
snapshotted 500,000 values in each of all six persistent types, recovered in
5.72 seconds, continued each type to one million values, and matched all six
million final reads. See [EXTERNAL-LOGGING-FIRST-TEST.md](EXTERNAL-LOGGING-FIRST-TEST.md)
and [EXTERNAL-LOGGING-ALL-TYPES-TEST.md](EXTERNAL-LOGGING-ALL-TYPES-TEST.md).

The staggered primary/replica runner is
`benchmarks/primary_replica_recovery.sh`. It starts from that full baseline,
refreshes and copies the snapshot, kills and holds down the primary while it
checks the replica, restarts the primary, appends a logged suffix, then kills
and holds down the replica while it checks the primary. It finally restarts the
replica and checks every value on both processes:

```sh
benchmarks/primary_replica_recovery.sh \
  --data-type all --baseline-count 1000000 --delta-count 1000 --pipeline 512
```

The matching systemd unit template is
`benchmarks/goblin-core-replica-test.service`. Adapt its paths, broker address,
and NUMA selector to the host before installation. The verified Thunder run
sent `SIGKILL` to both processes, one at a time, and matched all 6,006,000 final
values on each server. See
[EXTERNAL-LOGGING-PRIMARY-REPLICA-TEST.md](EXTERNAL-LOGGING-PRIMARY-REPLICA-TEST.md).

For a stronger storage check, produce with `acks=all`, restart Redpanda, and
consume the record again. That test was also used during this installation.
