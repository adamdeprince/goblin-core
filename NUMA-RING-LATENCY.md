# Engineering note: a remote ring is 3x slower

Goblin Core's lowest-latency path is a shared-memory ring: no kernel on the request
path, just a cache-line handoff between two cores. On a single-socket machine that is
the whole story. On a multi-socket (NUMA) machine there is a second variable that
matters just as much as the algorithm: which node the ring's memory lives on relative
to the two cores touching it. This note measures that, and it is the reason
`--cpu N` treats a non-local ring as a fatal error rather than a tunable.

## The question

A ring round trip is a producer on one core writing a request, a consumer on another
core reading it, and the reply coming back the same way. Each direction is a cache line
moving from one core's L1/L2 into the other's. When both cores are on the same NUMA node
that transfer stays on-socket. When they are on different nodes it crosses the
inter-socket interconnect, and the coherence traffic — the read-for-ownership, the
snoop, the writeback — pays the cross-node distance on every hop.

The ring's entire selling point is a sub-microsecond round trip. A cross-node transfer
is a large fraction of that budget, so the question is not academic: how much does it
actually cost, and is it bounded enough to leave to chance?

## Method

One server and one client, each busy-polling its side of the ring, each pinned to a
fixed core. The ring's pages are first-touched by the server, so they land on the
server's node. Then the client is pinned three ways relative to that node and the same
SBE round trip is timed in a tight loop, reporting the median over a multi-second window
(millions of samples) with the CPU cycle counter:

- local: client on a core on the server's own node
- remote: client on a core on an adjacent node
- far: client on a core on the most distant node

Only the client's node changes; the server, the ring, and the code are identical across
the three. So the delta is purely the cost of reaching the ring's memory across the
interconnect.

Hardware: a 4-socket server — 48 cores / 96 threads across four NUMA nodes, Intel Xeon
E5-4657L v2 (Ivy Bridge-EP, 2.4 GHz). An older, deliberately un-heroic box: it makes the
interconnect cost visible without a fast single core masking it.

## Result

Median SBE ring round trip, microseconds:

| operation | local | remote | far | remote / local |
|---|---:|---:|---:|---:|
| `SET` | 0.545 | 1.823 | 1.703 | 3.3x |
| `GET` | 0.570 | 1.810 | 1.691 | 3.2x |
| `HGET` | 0.630 | 1.827 | 1.682 | 2.9x |
| `ZADD` | 0.635 | 1.928 | 1.850 | 3.0x |
| `ZSCORE` | 0.543 | 1.837 | 1.697 | 3.4x |
| `HSET` (10 fields) | 0.857 | 2.172 | 2.091 | 2.5x |

A local ring round trip is about 0.55 microseconds. Move the client one node away and it
is about 1.8 microseconds — roughly three times slower for doing exactly the same work.
The p90/p99 track the median closely in every case, so this is the steady state, not a
tail artifact.

## Reading it

Two things stand out.

The penalty is large and consistent: ~3x across every operation. Whatever advantage the
no-kernel, no-parse ring design buys, a remote placement gives most of it back. The
sub-microsecond handoff the ring exists for simply is not sub-microsecond once it crosses
a socket.

The cost is "local versus not," not a smooth distance gradient. The remote (adjacent) and
far (most distant) columns are within noise of each other — on this machine's interconnect
the jump from same-node to any-other-node is the whole penalty, and the extra hops beyond
that are close to free. That will vary by interconnect topology, but the actionable shape
is the same everywhere: being on the ring's node is what matters.

For scale, the local number here (~0.55 microseconds) is itself higher than the same
benchmark on a modern single-thread-fast chip (~0.20 microseconds) purely because this is
an older 2.4 GHz core. That does not change the ratio; it is a reminder that the 3x is a
property of the placement, not the CPU.

## Why this drives a fatal error, not a knob

If a remote ring were merely slower it could be a warning. But a silent 3x regression on
the one path a user reached for specifically because it is fast is worse than the server
not starting — the operator gets latency they cannot explain and did not ask for.

So `--cpu N` does not "prefer" locality; it guarantees it. It pins the server to CPU N,
strictly binds the ring's pages to N's NUMA node so the ring is local by construction,
and then verifies every page is actually on that node. If the ring cannot be placed
locally — a full node, an empty huge-page pool on that node — the server refuses to start
with a clear message rather than run a ring that is 3x off target. A dead server is a
problem you notice; a remote ring is a problem you don't.

The arenas are the opposite case and get the opposite policy: `--numa-arena` sets a soft
preference for the same node (best-effort, spills elsewhere when the node is full),
because pinning all of a large server's memory to one node can starve clients co-located
on it. Strict for the small, latency-critical ring; soft for the large, capacity-driven
arenas.

## How the NUMA commands work

Two flags, deliberately asymmetric. Both are Linux-only and use raw `set_mempolicy` /
`mbind` / `move_pages` syscalls directly — no libnuma dependency, so the server drops the
flags to no-ops on a kernel or platform that lacks them rather than failing to build or
run.

**`--cpu N`** does three things, in order:

1. **Pin.** `sched_setaffinity` locks the server thread to CPU `N`, so the busy-poll loop
   never migrates off the core the ring is tuned for.
2. **Resolve the node.** `node_of_cpu(N)` reads `/sys/devices/system/node/node*/cpulist`
   and returns the node whose CPU list contains `N`. (The mapping is usually interleaved:
   on the 4-node box here, node 1 owns CPUs `1,5,9,13,…`, so `--cpu 5` resolves to node 1.)
3. **Place the ring strictly.** Before the ring pages are faulted, the process binds its
   allocation policy to that node with `MPOL_BIND` (strict). First-touch therefore lands
   the ring on the local node. Then it *verifies* — `move_pages` on every ring region — and
   if any page is not on the node, the server **refuses to start**. This is the fatal path
   the 3x measurement above justifies: a strict bind plus an explicit check, not a hope.

Once the rings are placed, the strict bind is lifted and the runtime allocation policy for
everything else (the arenas, the index, client buffers) is set from `--numa-arena`.

**`--numa-arena`** sets `MPOL_PREFERRED` for the pinned node — a *soft* preference. The
arena's `mmap` blocks still fault on the pinned node when it has free memory, but if it
does not, the kernel spills them to another node instead of failing. Without the flag, the
arena runs under the kernel's default policy (local first-touch, spill on pressure). The
flag never aborts: capacity is best-effort by design, because reserving a whole node's RAM
for one server's arenas is a choice only the operator can make.

### An important operational caveat (measured)

A soft preference is only as strong as the node's free memory, and "free" does not mean
what it looks like. On a 4-node box that was busy doing large file I/O, every node reported
only a few hundred MB `MemFree` out of 128 GB — the rest was **reclaimable page cache**.
With the common default `vm.zone_reclaim_mode = 0`, the kernel would rather satisfy an
allocation from a *remote* node than evict *local* page cache, so `MPOL_PREFERRED` (and
plain first-touch) placed a fresh 44 MB arena on a neighbor node even though the pinned
node had 128 GB of cache it could have dropped. The strict `MPOL_BIND` ring, on the same
run, stayed local — because strict binding forces the local reclaim (or fails) that the
soft preference declines to force.

The practical reading: on a dedicated box `--numa-arena` lands the arena local as intended;
on a box also serving files, a soft preference can be quietly overridden by page-cache
pressure. If arena locality matters there, the levers are `vm.zone_reclaim_mode = 1` (make
the kernel reclaim locally) or dropping caches before load — neither is goblin's to set, so
the flag stays honest about being best-effort. The latency-critical ring never depends on
any of this: it is strict, verified, and fatal-if-remote.

Confirm any of it in `/proc/<pid>/numa_maps`: each mapping's line ends with
`N<node>=<pages>` giving the resident page count per node.

## Reproducing

The probe is [`benchmarks/latency_shootout.cpp`](benchmarks/latency_shootout.cpp). It
forks a server, opens the ring as a client, and times each operation. The pinned cores
are overridable so a NUMA run can place the client local to or remote from the server's
node:

```sh
# server on a core of node A; client on a local core (both node A)
SERVER_CORE=<nodeA_core> CLIENT_CORE=<nodeA_core> ./latency_shootout ring ./goblin-core

# same server; client on a core of another node -> the remote column
SERVER_CORE=<nodeA_core> CLIENT_CORE=<nodeB_core> ./latency_shootout ring ./goblin-core
```

Map cores to nodes with `/sys/devices/system/node/node*/cpulist` (the node numbering is
often interleaved, e.g. node 0 = cpus 0,4,8,…). The ring's actual placement can be
confirmed in `/proc/<pid>/numa_maps` — the ring file's line shows `N<node>=<pages>`.

## Caveats

- The median is the figure to read. On a scheduler-managed host the mean is pulled up by
  rare busy-poll preemptions; the median and the tight p90 are the true per-op cost.
- One machine, one interconnect. The 3x ratio and the flat local-vs-remote shape are this
  box's; the direction (local wins, decisively) is general, the magnitude is not.
- This is the ring path. The socket (RESP-over-UDS) path is dominated by the syscall, so
  NUMA placement barely moves it — which is exactly why the ring, being syscall-free, is
  where node locality becomes a first-order concern.
