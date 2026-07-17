# InfiniBand setup

This document brings an InfiniBand link from bare hardware to a verified RDMA
fabric suitable for Goblin Core development. It covers host setup and acceptance
testing. The Goblin wire, flow control, and clients are documented in
[Polled RDMA rings](rdma-rings.md).

## Lab hardware

The development fabric contains `dopey`, `rain`, `thunder`, and `butterfly`.

| Property | Value |
|---|---|
| Adapter | Mellanox MCB191A-FCAT Connect-IB |
| PCI device | MT27600, device ID `15b3:1011` |
| PSID | `MT_1230110019` |
| Link | Single-port QSFP, FDR 56 Gb/s maximum |
| PCIe | PCIe 3.0 x8 |
| PCI address | `41:00.0` on `dopey`, `rain`, and `butterfly`; `42:00.0` on `thunder` |
| NUMA node | 1 on all four hosts |
| Fabric | Mellanox SwitchX; observed link mode is 4x FDR10, 40 Gb/s |
| Original firmware | `10.10.5054`, released March 11, 2015 |
| Updated firmware | `10.16.1200`, released November 27, 2017 |

### July 15, 2026 bring-up record

- The `dopey` and `rain` adapters were backed up, flashed to `10.16.1200`,
  cold-powered off, and verified after boot with their original GUIDs intact.
- `dopey` initialized `ibp65s0` cleanly and joined the SwitchX fabric through
  switch port 3 at 4x FDR10 (40 Gb/s), MTU 4096, LID 1.
- OpenSM is installed and active on `dopey` only.
- `rain` initialized `ibp65s0` cleanly. After its QSFP cable was connected, GUID
  `0002c90300e8cfc0` joined SwitchX port 4 at 4x FDR10 (40 Gb/s), MTU 4096.
- `thunder` is already current: firmware `10.16.1200`, PSID
  `MT_1230110019`, GUID `0002c90300e8dbd0`, device `mlx5_0`, SwitchX port 1,
  4x FDR10, MTU 4096, LID 2. Its `ibp66s0` IPoIB interface is persistent at
  `10.88.88.3/24`.
- `butterfly` reported PSID `MT_1230110019`, firmware `10.10.5054`, and base
  GUID `0002c90300e8d8a0`. Its original flash was saved with SHA-256
  `85d282ba6e14cd535f5b2f5c796ed5dccb0eeb48b0a293f437bfeb2fe040e6d8`.
  The matched `10.16.1200` image burned successfully, signature restoration
  completed, and its GUIDs and MACs remained unchanged. After a cold power
  cycle it initialized `ibp65s0` cleanly and joined SwitchX port 6 at 4x FDR10,
  MTU 4096, LID 4. Its IPoIB interface is persistent at `10.88.88.4/24`.
- `dopey` and `rain` completed 10,000 RC RDMA-write latency iterations at 1.46
  microseconds typical/average, 1.51 microseconds p99, and 2.71-2.95
  microseconds p99.9. RC send latency was 1.52 microseconds typical, 1.62
  microseconds p99, and 3.41 microseconds p99.9.
- Their 64 KiB RDMA-write test sustained 4,454.31 MB/s (35.6 Gb/s) average.
  Both endpoint error-counter sets remained zero after the tests.
- Goblin's receiver-polled ring then passed its RESP2, RESP3, SBE,
  fragmentation, pipeline, and reconnect suite from `rain` to `dopey`. With
  both processes pinned to HCA-local CPU 5, 500,000 depth-one SBE `PING` round
  trips measured 2.495 microseconds p50, 3.170 microseconds p99, and 5.799
  microseconds p99.9. RESP2 over the same RDMA ring measured 2.673, 2.859, and
  7.982 microseconds respectively.
- `thunder` completed 1,000 validated RDMA-CM exchanges with `dopey`. Its
  10,000-iteration, 2-byte RC RDMA-write test measured 1.17 microseconds
  typical, 1.19 microseconds average, 1.45 microseconds p99, and 7.53
  microseconds p99.9. Its 64 KiB test sustained 4,487.81 MB/s (35.9 Gb/s), and
  every standard port-error counter remained zero.
- `butterfly` completed 1,000 validated RDMA-CM exchanges with `dopey`. Its
  matching RC RDMA-write test measured 1.01 microseconds typical, 1.02
  microseconds average, 1.04 microseconds p99, and 3.29 microseconds p99.9. Its
  64 KiB test sustained 4,390.63 MB/s (35.1 Gb/s), and every standard
  port-error counter remained zero.
- IPoIB is persistent at `10.88.88.1/24` on `dopey`, `10.88.88.2/24` on
  `rain`, `10.88.88.3/24` on `thunder`, and `10.88.88.4/24` on `butterfly`.
  ICMP and validated RDMA CM `rping` exchanges pass between the configured
  endpoints.

Before the firmware update, Ubuntu 22.04 loaded `mlx5_core` but failed to attach
`mlx5_ib`:

```text
QUERY_HCA_VPORT_CONTEXT ... failed, status bad operation
infiniband mlx5_0: Couldn't create per-port data
mlx5_ib.rdma: probe of mlx5_core.rdma.0 failed with error -12
```

Consequently, `/sys/class/infiniband` was empty and `/dev/infiniband` did not
exist. Installing userspace tools cannot repair this state; the adapter firmware
must load successfully first.

## Safety rules

1. Identify the adapter by PSID, not only by PCI device ID or product name.
2. Use an image whose embedded PSID exactly matches the device PSID.
3. Save the existing flash image before burning a replacement.
4. Never interrupt a burn or power off while it is running.
5. Query the stored image after the burn and confirm that GUIDs were preserved.
6. Cold power-cycle the host after a Connect-IB update. A normal reboot is not a
   substitute because it may leave PCIe slot power applied.
7. Run exactly one subnet manager on a simple fabric unless a deliberate
   redundant-SM configuration has been designed.

## Install the tools

Install diagnostics and development headers on every endpoint:

```bash
sudo apt-get install -y \
  rdma-core rdmacm-utils ibverbs-utils infiniband-diags perftest mstflint \
  libibverbs-dev librdmacm-dev
```

Install OpenSM on the host that will manage the fabric. For this pair, that host
is `dopey`:

```bash
sudo apt-get install -y opensm
```

Do not enable a second OpenSM instance on `rain`.

## Inventory the adapter

Confirm the PCI identity and driver:

```bash
lspci -s 41:00.0 -nnk
sudo mstflint -d 41:00.0 q
journalctl -k -b --no-pager | grep -i -E 'mlx5|infiniband|firmware'
```

The `mstflint` query must be recorded before selecting an image. The verified
`dopey`, `rain`, and `thunder` cards report PSID `MT_1230110019`, but their
GUIDs are intentionally different and must remain different. Query and record
`butterfly` independently before updating it; do not infer its PSID from the
matching PCI identity.

## Acquire and validate firmware

Use NVIDIA's [Connect-IB firmware page](https://network.nvidia.com/support/firmware/connectib/)
and select the image by PSID. The image used for the verified matching adapters
is:

```text
fw-ConnectIB-rel-10_16_1200-MCB191A-FCA_A1.bin
```

The NVIDIA downloader's displayed MD5 was stale when the image was retrieved on
July 15, 2026. Do not accept that metadata as proof. The binary served that day
had this SHA-256 digest:

```text
33cb276104be851caa7d6d9850bcf79c106707e3d66169349a33e5e2270e2eea
```

More importantly, have `mstflint` parse and integrity-check the image itself:

```bash
mstflint -i fw-ConnectIB-rel-10_16_1200-MCB191A-FCA_A1.bin q
```

The result must include both:

```text
FW Version: 10.16.1200
PSID:       MT_1230110019
```

Stop if either value differs from the intended version or the device query.

## Back up and flash

Back up each adapter before writing it:

```bash
sudo mstflint -d 41:00.0 ri connectib-before.bin
sha256sum connectib-before.bin
```

Keep each host's backup separately. The images contain host-specific identifiers
and are not interchangeable recovery files.

Burn the validated image only after the backup and PSID checks are complete:

```bash
sudo mstflint -y -d 41:00.0 \
  -i fw-ConnectIB-rel-10_16_1200-MCB191A-FCA_A1.bin burn
```

Wait for both the firmware write and signature restoration to report `OK`. Then
query the card again:

```bash
sudo mstflint -d 41:00.0 q
```

Immediately after a successful burn and before the cold restart, this is the
expected state:

```text
FW Version:          10.16.1200
FW Version(Running): 10.10.5054
```

Also compare the base GUIDs with the pre-burn query. On July 15, 2026, the
`dopey` and `rain` cards reached this state and retained their original GUIDs
before being powered off.

## Cold restart and verify

Power the host off completely:

```bash
sudo poweroff
```

After restoring power, verify the active firmware and RDMA device:

```bash
sudo mstflint -d 41:00.0 q
ibv_devices
ibv_devinfo
ibstat
rdma link show
ls -l /dev/infiniband
```

The stored and running versions must both be `10.16.1200`. Determine the device
name with `rdma link show` or `ibv_devices`; do not derive it from the PCI slot.
Ubuntu names the `dopey` and `rain` adapters `ibp65s0`, while the equally valid
name on `thunder` is `mlx5_0`. Before a subnet manager assigns a LID, a
physically connected InfiniBand port may remain in `Initializing` rather than
`Active`.

## Bring up the fabric

Run one OpenSM instance on `dopey`:

```bash
sudo systemctl enable --now opensm
systemctl status opensm --no-pager
```

Verify both ends:

```bash
ibstat
sudo iblinkinfo
sudo ibnetdiscover
```

Acceptance requires both ports to report `State: Active`, the expected FDR rate,
nonzero LIDs, and a topology containing both host GUIDs.

Inspect counters before and after testing. Link errors, symbol errors, recovery
events, or excessive retransmission must be resolved rather than hidden by an
average bandwidth number:

```bash
sudo perfquery -x
```

If the direct PortInfo query reports `PhysLinkState: Polling`, the HCA is enabled
but is not receiving physical signal:

```bash
sudo ibportstate -C ibp65s0 -P 1 -D 0 1 query
sudo ibportstate -C ibp65s0 -P 1 -D 0 1 on
sudo ibportstate -C ibp65s0 -P 1 -D 0 1 reset
```

If it remains in `Polling` after the reset, inspect and reseat the QSFP cable at
both ends and check that the selected SwitchX port is enabled. Do not keep
changing firmware or driver settings: `Polling` at this point is a physical-path
or switch-port condition.

## Prove RDMA communication

The perftest programs use a normal TCP connection only to exchange queue-pair
metadata; their measured data path is native InfiniBand verbs.

Run each server command on `dopey`, followed by its client command on `rain`.
Substitute the device name reported by each host where it differs:

```bash
# RC send round-trip latency
ib_send_lat -d ibp65s0 -i 1 -F
ib_send_lat -d ibp65s0 -i 1 -F dopey

# RDMA write round-trip latency
ib_write_lat -d ibp65s0 -i 1 -F
ib_write_lat -d ibp65s0 -i 1 -F dopey

# RDMA write bandwidth
ib_write_bw -d ibp65s0 -i 1 -F
ib_write_bw -d ibp65s0 -i 1 -F dopey
```

Record firmware, link state, MTU, message size, queue depth, CPU affinity, NUMA
placement, median latency, tail latency where available, bandwidth, and the
post-test error counters.

## Optional IPoIB and RDMA CM check

Goblin's RDMA ring uses `rdma_cm` for connection setup only. Give the
InfiniBand interfaces temporary addresses so `rping` can validate that path:

```bash
sudo modprobe ib_ipoib
ls /sys/class/infiniband/DEVICE/device/net
```

Using the interface name printed above, assign a dedicated fabric subnet:

```bash
# dopey
sudo ip address replace 10.88.88.1/24 dev INTERFACE
sudo ip link set INTERFACE up

# rain
sudo ip address replace 10.88.88.2/24 dev INTERFACE
sudo ip link set INTERFACE up
```

First verify IPoIB, then RDMA CM:

```bash
ping 10.88.88.2

# dopey
rping -s -a 10.88.88.1 -v

# rain
rping -c -a 10.88.88.1 -v
```

Only make the addresses persistent after the temporary setup and RDMA tests are
clean.

The lab address plan is:

| Host | IPoIB address |
|---|---|
| `dopey` | `10.88.88.1/24` |
| `rain` | `10.88.88.2/24` |
| `thunder` | `10.88.88.3/24` |
| `butterfly` | `10.88.88.4/24` |

For persistent configuration, add an optional physical interface to netplan so
an absent fabric does not delay boot. Use the actual IPoIB netdevice name, not
necessarily the verbs-device name:

```yaml
network:
  version: 2
  ethernets:
    ibp65s0:
      addresses:
        - 10.88.88.1/24
      dhcp4: false
      dhcp6: false
      accept-ra: false
      optional: true
```

Validate and apply it:

```bash
sudo netplan generate
sudo netplan apply
```

## Goblin polling contract

Shared-memory rings and polled RDMA channels belong to one ordered poll-target
list. Their order is the order in which their options appear on the Goblin Core
command line. For targets `A`, `B`, and `C`, the server scans:

```text
A -> B -> C -> socket pass
```

When one target makes progress, the scan restarts at `A`, matching the current
strict-priority ring behavior. Sockets are serviced only after every polled
target is empty, subject to the existing idle-pass cadence. RDMA must not be
placed in a separate all-RDMA phase before or after all rings, because that would
discard command-line priority across transport types.

See [Polled RDMA rings](rdma-rings.md) for the implemented command-line syntax,
registered-memory layout, and client APIs.

Goblin treats ordinary NICs and InfiniBand devices as NUMA placement targets.
Specific bind addresses are resolved automatically. When all selected hardware
is local to one node, that node is used; when a NIC and HCA resolve to different
nodes, startup prints the topology and requires `--numa NODE|DEVICE` or an exact
`--cpu CPU` choice. This makes the unavoidable remote hop an operator decision.
