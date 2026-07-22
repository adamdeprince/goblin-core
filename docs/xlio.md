# Native XLIO Ultra TCP

Goblin Core has a native NVIDIA XLIO Ultra transport for busy-polled TCP on
NVIDIA Ethernet adapters. It uses the Ultra API directly, participates in the
same literal command-line priority order as rings, RDMA, and ExaSock, and keeps
ordinary TCP on the wire. An accelerated endpoint can therefore exchange RESP
or SBE with another accelerated endpoint, while an ordinary kernel TCP client
can still talk to an XLIO-backed Goblin server.

The support is opt-in at build time because it requires the pinned XLIO/DPCP
stack and Linux. This document covers the runtime contract, source and license
audit, ConnectX-5 qualification, build, and measured command latency.

## Why XLIO Ultra

XLIO Ultra provides a directly polled, event-based, zero-copy TCP API over
NVIDIA Ethernet adapters. It accelerates the host implementation without
inventing a new wire protocol: the peer still sees an ordinary TCP stream. That
property lets an accelerated Goblin server or client communicate with an
unmodified kernel TCP peer using RESP or SBE.

Goblin uses the Ultra API directly rather than routing socket calls through
XLIO's POSIX interposer. Each `--xlio` target owns its own polling group, which
preserves literal command-line priority across transport types. `libxlio.so`
must still be preloaded so the Ultra symbols and runtime are available.

## Pinned source and license selection

The dependency audit was performed on July 22, 2026.

| Component | Pinned source | Selected license | Reason |
|---|---|---|---|
| NVIDIA XLIO | `3.61.2`, commit `ae821447658c274e800d64592b6fadd747624ed3` | BSD-2-Clause option | Latest non-prerelease release at the audit date; includes the stable server-side Ultra API |
| NVIDIA DPCP | `1.1.61`, commit `4cc43b3047367383d3eb8d9c63e8637dfcea5d70` | BSD-3-Clause | Mandatory XLIO packet-control dependency; exceeds XLIO's `1.1.58` minimum |
| XLIO json-c copy | Source included by XLIO | MIT | Upstream dependency retained with its notice |

XLIO is dual licensed `GPL-2.0-only OR BSD-2-Clause`; Goblin Core explicitly
uses the BSD-2-Clause branch. A small XLIO event-worker file set offers a
BSD-3-Clause alternative, and DPCP's CMake support is Apache-2.0. XLIO also
retains permissively licensed json-c, lwIP/FreeBSD TCP, and test-only GoogleTest
sources. The complete notices remain in `third_party/xlio/`,
`third_party/libdpcp/`, and the top-level `NOTICE`.

Upstream references:

- [NVIDIA XLIO source and releases](https://github.com/Mellanox/libxlio)
- [NVIDIA DPCP source](https://github.com/Mellanox/libdpcp)
- [XLIO 3.61 documentation](https://docs.nvidia.com/networking/display/xliov361)
- [XLIO Ultra API](https://docs.nvidia.com/networking/display/xliov361/xlio-ultra-api)
- [ConnectX-5 firmware downloads](https://network.nvidia.com/support/firmware/connectx5en)
- [ConnectX-5 firmware 16.35.8008 LTS release notes](https://docs.nvidia.com/networking/display/connectx5firmwarev16358008lts/release-notes-history)

## ConnectX-5 lab inventory

Both hosts run Ubuntu 22.04, kernel `5.15.0-186-generic`, the inbox `mlx5_core`
driver, `rdma-core` 39.0, and use NUMA node 1 for the target adapter.

| Property | `butterfly` | `rain` |
|---|---|---|
| Adapter | Mellanox/NVIDIA ConnectX-5 MCX515A-CCAT | Mellanox/NVIDIA ConnectX-5 MCX515A-CCAT |
| PCI identity | MT27800, `15b3:1017`, `0000:44:00.0` | MT27800, `15b3:1017`, `0000:44:00.0` |
| Linux interface / verbs device | `enp68s0np0` / `rocep68s0` | `enp68s0np0` / `rocep68s0` |
| Direct-link address | `10.100.0.1/30` | `10.100.0.2/30` |
| Link | 100 Gb/s full duplex, DAC | 100 Gb/s full duplex, DAC |
| Firmware before upgrade | `16.33.1300` | `16.26.1040` |
| Qualified firmware | `16.35.8008` | `16.35.8008` |

The direct link passed bidirectional IP traffic after the upgrade; a five-packet
`butterfly`-to-`rain` ping averaged about 253 microseconds. That was a
reachability check, not a latency benchmark.

NVIDIA's XLIO 3.61 support matrix names newer ConnectX devices rather than
ConnectX-5. The two cards are therefore empirically qualified here, not claimed
as vendor-certified.

## Firmware qualification and upgrade

Both adapters have PSID `MT_0000000011`. They were upgraded one at a time to
NVIDIA's ConnectX-5 firmware `16.35.8008` LTS U8, released March 1, 2026. The
selected image was the PSID-specific file:

```text
fw-ConnectX5-rel-16_35_8008-MCX515A-CCA_Ax_Bx-UEFI-14.29.15-FlexBoot-3.6.902.bin.zip
```

The downloaded archive matched NVIDIA's published SHA-256 digest
`7f48e6ba919ac6fc9b63ff0414ee523f52476f444da69ea0e5abca1ac6ea0d91`.
The extracted firmware image had SHA-256 digest
`a378a4199e4ab36fb6a49a06435b90f3db89e2ca4714d59134fa1b608bfab855`.
`mstflint` confirmed the image's PSID, FS4 format, firmware version, UEFI
`14.29.15`, and FlexBoot `3.6.902` before either card was written.

A full 16 MiB device image was captured from each card before the upgrade. The
rollback artifacts live outside the repository in
`$HOME/firmware/connectx5/backups/2026-07-22/`:

| Host | Backup image | SHA-256 |
|---|---|---|
| `butterfly` | `butterfly-MT_0000000011-fw16.33.1300.bin` | `8a6df63180d59b6516c6b0ff44a34d7ef3ba606e68e98a463ce0a00e079e6a94` |
| `rain` | `rain-MT_0000000011-fw16.26.1040.bin` | `c107e5a47a0ca6897315fa6bdcd72e40096f8bf865321d771104f0bd9a00581b` |

Each backup was parsed independently before proceeding and retained its host's
original firmware, PSID, base GUID, and base MAC. Each card was then burned and
activated before the other host was changed. The old systems required a full
power cycle rather than a warm reboot to return reliably.

After activation, `mstflint` reported running and stored firmware `16.35.8008`
on both hosts. The original PSID, GUID, and MAC remained intact. The direct-link
addresses are persisted in `/etc/netplan/70-connectx5-direct.yaml`; DHCP, IPv6
router advertisements, and DHCPv6 are disabled on that isolated `/30` link.
Both interfaces negotiated 100 Gb/s full duplex after reboot. The target
device's firmware, fatal-firmware, TX, and RX devlink health reporters were
healthy with zero errors and recoveries, and the matching `ethtool` hardware
error counters remained zero.

## Mixed-fabric discovery patch

Both hosts also expose an InfiniBand-only Connect-IB device. `rain` additionally
has two Ethernet ConnectX-4 ports. Unmodified XLIO tried to open the Connect-IB
device through DPCP and aborted before discovering the target ConnectX-5.

The local patch in
`third_party/xlio/src/core/dev/ib_ctx_handler_collection.cpp` queries every verbs
device and skips devices whose physical ports are all InfiniBand link-layer
ports. It does not filter Ethernet devices, and it does not key off a lab host,
PCI address, or interface name.

The patched build initialized on both mixed-fabric hosts. On `rain` it discovered
all three Ethernet verbs devices, mapped `10.100.0.2` to
`enp68s0np0`/`rocep68s0`, and created the active TX and RX queues on that
ConnectX-5 device.

## Reproducing the build

The verified build used GCC 11.4, CMake 3.22, GNU Autotools, libibverbs, libnl3,
and libcap. On Ubuntu, install the development prerequisites first:

```bash
sudo apt-get install -y \
  autoconf automake cmake g++ libcap-dev libibverbs-dev libnl-3-dev \
  libnl-route-3-dev libtool make pkg-config rdma-core
```

Build DPCP first, then point XLIO at the same private prefix:

```bash
prefix="$PWD/build/xlio-prefix"

cmake -S third_party/libdpcp -B build/libdpcp \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build build/libdpcp -j"$(nproc)"
cmake --install build/libdpcp

(cd third_party/xlio && ./autogen.sh)
mkdir -p build/xlio
(cd build/xlio && ../../third_party/xlio/configure \
  --prefix="$prefix" --with-dpcp="$prefix")
make -C build/xlio -j"$(nproc)"
make -C build/xlio install
```

Configure Goblin Core against the native API headers:

```bash
cmake -S . -B build-xlio \
  -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ENABLE_XLIO=ON
cmake --build build-xlio -j"$(nproc)"
```

The XLIO tree uses an Autotools-generated Makefile. Running `autogen.sh`
generates build-system files inside the source tree; use a disposable source
copy when verifying that the vendored tree still differs from upstream only by
the documented discovery patch.

XLIO needs `CAP_NET_RAW` or root to create the offloaded path. It also expects a
sufficient locked-memory limit and normally uses huge pages. The smoke tests
used `XLIO_MEM_ALLOC_TYPE=ANON` because they validated functionality rather than
production memory placement.

## Running Goblin Core over XLIO

`--xlio ADDRESS PORT` adds one native XLIO listener and may be repeated. For
example, on a host where CPU 5 and `10.100.0.1` are both on NUMA node 1:

```bash
prefix=/opt/goblin-xlio
sudo env \
  XLIO_MEM_ALLOC_TYPE=ANON \
  LD_LIBRARY_PATH="$prefix/lib" \
  LD_PRELOAD="$prefix/lib/libxlio.so" \
  numactl --cpunodebind=1 --membind=1 taskset -c 5 \
  ./build-xlio/goblin-core \
    --cpu 5 --numa 1 --xlio 10.100.0.1 6379
```

The native client uses the same runtime:

```bash
sudo env \
  XLIO_MEM_ALLOC_TYPE=ANON \
  LD_LIBRARY_PATH="$prefix/lib" \
  LD_PRELOAD="$prefix/lib/libxlio.so" \
  numactl --cpunodebind=1 --membind=1 taskset -c 5 \
  ./build-xlio/redis-cli-ring --xlio 10.100.0.1 6379 PING
```

RESP2 is the default. RESP3 is selected with `HELLO 3`. SBE uses the same TCP
stream after the `GOBLINS!` handshake and requires `--enable-sbe`; SBE clients
and servers must be exactly the same Goblin Core version.

RESP authentication applies to XLIO listeners when `--auth-file` is present.
`--no-auth-xlio` explicitly trusts RESP on this fabric. SBE is deliberately
unauthenticated and must remain behind a trusted infrastructure boundary.

## Polling, buffers, and snapshots

Polled targets are scanned in literal command-line order. A sequence such as

```text
--ring /tmp/a 64kb --xlio 10.100.0.1 6379 \
--rdma 10.88.88.1 6380 1mb --ring /tmp/b 1mb
```

scans the first ring, XLIO group, RDMA target, and second ring in that exact
order. Progress restarts the scan at the first target. A continuously ready
earlier target may therefore starve every later target by design. Ordinary
kernel sockets receive their sparse pass only when all polled targets are idle.

The server retains XLIO-owned receive buffers until command parsing and dispatch
finish, so the common RX path is zero-copy. Replies use Ultra inline-copy TX.
XLIO transports a TCP byte stream rather than fixed ring slots, so commands are
not constrained by a local ring's message-size limit.

XLIO 3.61 does not permit a process with a live Ultra polling group to use the
normal fork-based snapshot path. Goblin Core therefore runs file `SAVE`
synchronously while XLIO is active and rejects `GOBLIN.DUMPWORLD` with a clear
fork-safety error.

## TCP interoperability result

The NVIDIA Ultra ping-pong example was compiled against the pinned stack. Each
case exchanged the literal TCP payloads `ping\n` and `pong\n`; the peer labeled
"kernel" used Python's ordinary `socket` module with no XLIO preload.

| Accelerated endpoint | Ordinary endpoint | Result |
|---|---|---|
| `butterfly` XLIO Ultra server | `rain` kernel TCP client | Pass |
| `butterfly` XLIO Ultra client | `rain` kernel TCP server | Pass |
| `rain` XLIO Ultra server | `butterfly` kernel TCP client | Pass |
| `rain` XLIO Ultra client | `butterfly` kernel TCP server | Pass |

This proves the required asymmetric compatibility in both roles on both cards:
an XLIO-speaking endpoint remains a normal TCP peer on the wire.

After the firmware upgrade, the server direction was repeated with an XLIO
Ultra server on `butterfly` and a kernel TCP client on `rain`. The client
direction was repeated with a kernel TCP server on `butterfly` and an XLIO Ultra
client on `rain`. Both exchanged the expected payloads, and XLIO reported
zero-copy completion on the accelerated endpoint.

## Goblin command latency

The native implementation was measured on the direct 100 Gb/s link above using
the same C++ RESP2 client, fixtures, commands, warmup, sample count, and
pipeline depth for every network mode. Both processes were hard-bound to NUMA
node 1, which owns each ConnectX-5, and to exact node-local CPUs. Native XLIO on
both endpoints delivered 4.98-8.37 microsecond median round trips across `PING`,
`SET`, `GET`, `HSET`, `HGET`, `ZADD`, and `ZSCORE`.

See [XLIO Ultra command latency](../XLIO-LATENCY.md) for the complete comparison
against ordinary kernel TCP, the asymmetric compatibility case, local RESP
rings, percentile tables, raw results, and reproduction command.
