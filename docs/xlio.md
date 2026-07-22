# XLIO Ultra qualification

Goblin Core vendors a pinned NVIDIA XLIO Ultra stack as the dependency baseline
for a future kernel-bypass TCP transport. This document records the source and
license audit, the two-host ConnectX-5 qualification, and the constraints that
must be resolved before XLIO is exposed as a Goblin Core runtime option.

XLIO is vendored but **not yet connected to the Goblin Core build or server**.
The tests below qualify the dependency and hardware, not a released transport.

## Why XLIO Ultra

XLIO Ultra provides a directly polled, event-based, zero-copy TCP API over
NVIDIA Ethernet adapters. It accelerates the host implementation without
inventing a new wire protocol: the peer still sees an ordinary TCP stream. That
property lets an accelerated Goblin server or client communicate with an
unmodified kernel TCP peer using RESP or SBE.

The intended Goblin integration will use the Ultra API directly rather than
only applying XLIO's POSIX `LD_PRELOAD` interposer. A polling group can then join
Goblin's existing ordered polling loop while preserving ordinary TCP on the
wire.

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
| Firmware at qualification | `16.33.1300` | `16.26.1040` |

The direct link passed bidirectional IP traffic; a five-packet
`butterfly`-to-`rain` ping averaged about 195 microseconds. That was a reachability
check, not a latency benchmark.

NVIDIA's XLIO 3.61 support matrix names newer ConnectX devices rather than
ConnectX-5. The two cards are therefore empirically qualified here, not claimed
as vendor-certified. [NVIDIA's firmware
table](https://docs.nvidia.com/networking/display/MFT/Supported%2BAdapter%2BCards%2BFirmware%2BVersions)
listed legacy ConnectX-5 firmware `16.35.4030` at the audit date, so both lab
cards are behind that release. Do not flash either card without a separate
PSID, backup, and rollback review.

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

The XLIO tree uses an Autotools-generated Makefile. Running `autogen.sh`
generates build-system files inside the source tree; use a disposable source
copy when verifying that the vendored tree still differs from upstream only by
the documented discovery patch.

XLIO needs `CAP_NET_RAW` or root to create the offloaded path. It also expects a
sufficient locked-memory limit and normally uses huge pages. The smoke tests
used `XLIO_MEM_ALLOC_TYPE=ANON` because they validated functionality rather than
production memory placement.

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
an XLIO-speaking endpoint remains a normal TCP peer on the wire. It does not yet
measure Goblin command latency or throughput.

## Integration gates

The dependency baseline is ready. Runtime support still needs deliberate work:

1. Add an opt-in CMake target that builds the pinned DPCP/XLIO stack without
   changing ordinary Goblin builds.
2. Wrap Ultra polling groups, sockets, callbacks, RX buffer release, registered
   TX memory, and completion ownership behind a Goblin transport boundary.
3. Put each XLIO polling target into the existing literal command-line poll
   order, followed by the sparse ordinary-socket pass.
4. Add RESP and SBE server/client tests plus mixed XLIO/kernel interoperability
   tests using real Goblin commands.
5. Resolve snapshot behavior before enabling the transport. XLIO 3.61 documents
   `fork()` as supported only when no Ultra polling group exists, while
   `GOBLIN.SAVE` and `GOBLIN.DUMPWORLD` currently fork after listeners are live.
   The integration must define a safe snapshot path rather than relying on
   undefined behavior.
6. Tune memory registration and device selection. The default XLIO anonymous
   allocator registered a 2 GiB virtual pool on every discovered Ethernet verbs
   device on `rain`; production configuration should constrain that footprint
   and avoid registering unused adapters.
