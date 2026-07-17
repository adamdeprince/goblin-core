# ExaSock (Cisco Nexus SmartNIC sockets)

Goblin Core can use Cisco ExaSock to accelerate ordinary TCP between hosts that
have Nexus SmartNIC / ExaNIC cards (for example the X100 pair on `thunder` and
`butterfly`). Unlike the InfiniBand RDMA ring transport, ExaSock does **not**
introduce a new wire protocol: RESP and SBE still travel as TCP streams. The
SmartNIC stack bypasses the kernel network path for sockets bound to ExaNIC
interfaces when the process is launched under the `exasock` wrapper.

ExaSock is **not vendored**. It must be installed on the build and run hosts
from Cisco/Exablaze packages (headers `exasock/*.h`, `libexasock_ext`, and the
`exasock` launcher).

## Compile-time flags

| CMake option | Default | Effect |
|---|---|---|
| `GOBLIN_CORE_ENABLE_EXASOCK` | **OFF** | When ON, requires the ExaSock SDK and defines `GOBLIN_HAS_EXASOCK` |
| `GOBLIN_CORE_ENABLE_RDMA` | ON | InfiniBand polled-ring transport (`GOBLIN_HAS_RDMA`) when libibverbs + librdmacm are present |

Configure with ExaSock:

```bash
cmake -S . -B build-exasock -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ENABLE_EXASOCK=ON \
  -DGOBLIN_CORE_ENABLE_RDMA=ON
cmake --build build-exasock -j"$(nproc)"
```

Without `-DGOBLIN_CORE_ENABLE_EXASOCK=ON`, the tree builds as usual and does not
link or include ExaSock. Turning the option ON without an installed SDK is a
**configure error** (so missing SDK is never a silent no-op).

## Runtime model and poll order

ExaSock is a **priority polled TCP target**, not a separate wire protocol. It
joins the same busy-poll scan as shared-memory rings and InfiniBand RDMA.
Literal CLI order is strict priority; a busy earlier target starves later ones.
Only when every polled target is idle does the server run the sparse plain-socket
pass (`--bind` / `--port` / `--unixsocket`).

```bash
# Scan order: ring A, ExaSock, RDMA, ring B, then plain sockets
exasock ./goblin-core \
  --ring /tmp/a 64kb \
  --exasock 10.99.99.1 6379 \
  --rdma 10.88.88.1 6380 1mb \
  --ring /tmp/b 1mb
```

| Flag | Meaning |
|---|---|
| `--ring PATH SIZE` | Shared-memory SQ/CQ |
| `--exasock ADDRESS PORT` | Priority TCP listener (SmartNIC bypass under `exasock`) |
| `--rdma ADDRESS PORT SIZE` | Polled RDMA rings |

1. Assign IPs on the ExaNIC interfaces (example: `10.99.99.1/24` on thunder,
   `10.99.99.2/24` on butterfly) and ensure link is up (`exanic-config`, DAC/fiber).
2. Start the server **under** the `exasock` wrapper with `--exasock` on the ExaNIC
   address (required for kernel bypass). Without the wrapper the target still
   occupies its poll slot as ordinary priority TCP.
3. Drive it with the ExaSock-aware clients, also under the wrapper when you want
   acceleration:

```bash
exasock ./redis-cli-exasock 10.99.99.1 6379 PING
exasock ./redis-cli-exasock 10.99.99.1 6379 SET price:IBM 247.31
```

Plain TCP (no wrapper) still works for bring-up; `INFO` then reports
`exasock_loaded:0`.

### C++ clients

RESP (same surface as the ring client):

```cpp
#include "goblin/core/exasock_client.hpp"

auto resp = goblin::core::exasock::ExasockClient::open("10.99.99.1", 6379);
auto reply = resp->command({"GET", "price:IBM"});
```

Typed SBE:

```cpp
#include "goblin/core/sbe_ring_client.hpp"

auto sbe = goblin::core::SbeExasockClient::open(
    std::string_view("10.99.99.1"), std::uint16_t{6379});
double score = sbe->zscore("prices", "IBM").value();
```

CLI (interactive / pipe share the ring redis-cli clone):

```bash
exasock redis-cli-ring --exasock 10.99.99.1 6379 PING
exasock redis-cli-ring --exasock 10.99.99.1 6379 -f cmds.txt
```

Optional connect options:

- `ConnectOptions::require_loaded` — fail open if not under `exasock`
- `ConnectOptions::ate_id` — use Accelerated TCP Engine when the card supports it

When ExaSock ≥ 2.2.0 is loaded, the client may issue `MSG_EXA_WARM` before a real
send to keep the TX path hot.

### Python

Build the extension with `-DGOBLIN_CORE_ENABLE_EXASOCK=ON`. Then:

```python
from goblin_core import ExasockRedis, HAS_EXASOCK
assert HAS_EXASOCK
r = ExasockRedis("10.99.99.1", 6379, decode_responses=True)
r.ping()
```

Launch the interpreter under `exasock` for SmartNIC acceleration.

## INFO fields

| Field | Meaning |
|---|---|
| `rdma_support` | `1` if this binary was built with `GOBLIN_HAS_RDMA` |
| `exasock_support` | `1` if this binary was built with `GOBLIN_HAS_EXASOCK` |
| `exasock_loaded` | `1` if the process is running under the `exasock` preload |
| `exasock_version` | ExaSock version string when loaded |

## Permissions and devices

`/dev/exanic*` must be readable by the process (udev rule or group). Example:

```bash
echo 'KERNEL=="exanic*", MODE="0666"' | sudo tee /etc/udev/rules.d/99-exanic.rules
sudo udevadm control --reload-rules
```

## Relation to RDMA rings

| | ExaSock (`--exasock`) | RDMA (`--rdma`) |
|---|---|---|
| Medium | ExaNIC Ethernet TCP | InfiniBand RC |
| Wire | RESP / SBE over TCP | RESP / SBE over polled one-sided rings |
| Server flag | `--exasock ADDRESS PORT` (under `exasock` wrapper) | `--rdma ADDRESS PORT SIZE` |
| Poll order | Same ordered busy-poll scan as rings | Same ordered busy-poll scan as rings |
| CMake | `GOBLIN_CORE_ENABLE_EXASOCK` (opt-in) | `GOBLIN_CORE_ENABLE_RDMA` |

Both may be enabled in the same binary and freely interleaved with `--ring` on
the command line. They share the priority scan; they do not share transport code
beyond RESP/SBE dispatch.
