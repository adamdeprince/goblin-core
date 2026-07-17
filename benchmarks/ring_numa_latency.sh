#!/usr/bin/env bash
# Local shared-memory ring latency vs NUMA distance.
#
# Fixes the server on one NUMA node (with the ring bound to that node) and moves
# only the client:
#   0 hops — same node as the server
#   1 hop  — adjacent node (ACPI distance 20 on the lab 4-socket box)
#   2 hops — far node      (ACPI distance 30)
#
# Same ops as the RDMA / local ring probes (SBE PING, RESP PING/SET/GET/HSET/HGET).
#
# Defaults target thunder (4× E5-4657L v2, nodes 0–3). Override CPUs if needed:
#   SERVER_NODE=1 SERVER_CPU=5 SAME_CPU=9 HOP1_CPU=0 HOP2_CPU=3 \
#     bash benchmarks/ring_numa_latency.sh
#
# On thunder's distance matrix from node 1: 0 and 2 are 1 hop (20), 3 is 2 hops (30).
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# Prefer Linux NFS path when present
if [[ -d /home/adam/dev/packrat/include/goblin ]]; then
  ROOT=/home/adam/dev/packrat
fi

BUILD_DIR="${BUILD_DIR:-$ROOT/build-rdma}"
if [[ ! -x "$BUILD_DIR/goblin-core" && -x "$ROOT/build-rel/goblin-core" ]]; then
  BUILD_DIR="$ROOT/build-rel"
fi

GOBLIN="${GOBLIN:-$BUILD_DIR/goblin-core}"
PROBE="${PROBE:-$BUILD_DIR/goblin_core_ring_latency_benchmark}"
CLI="${CLI:-$BUILD_DIR/redis-cli-ring}"

# Server fixed on node 1 (also where the HCA lives on this box).
SERVER_NODE="${SERVER_NODE:-1}"
SERVER_CPU="${SERVER_CPU:-5}"     # node 1
SAME_CPU="${SAME_CPU:-9}"         # node 1 — 0 hops
HOP1_CPU="${HOP1_CPU:-0}"         # node 0 — distance 20 from node 1
HOP2_CPU="${HOP2_CPU:-3}"         # node 3 — distance 30 from node 1

RING_SIZE="${RING_SIZE:-64kb}"
SAMPLES="${SAMPLES:-100000}"
WARMUP="${WARMUP:-10000}"
SKIP_BUILD="${SKIP_BUILD:-0}"
OUT_DIR="${OUT_DIR:-/tmp/goblin-ring-numa-$$}"
mkdir -p "$OUT_DIR"

log() { printf '[ring-numa] %s\n' "$*" >&2; }

need() {
  command -v "$1" >/dev/null 2>&1 || { log "need $1"; exit 1; }
}
need taskset
need numactl

if [[ "$SKIP_BUILD" != "1" ]]; then
  log "build ($BUILD_DIR)"
  mkdir -p "$BUILD_DIR"
  CMAKE="${CMAKE:-$(command -v cmake)}"
  if [[ -x "$HOME/.local/lib/python3.10/site-packages/cmake/data/bin/cmake" ]]; then
    CMAKE="$HOME/.local/lib/python3.10/site-packages/cmake/data/bin/cmake"
  fi
  if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    (cd "$BUILD_DIR" && "$CMAKE" "$ROOT" -DCMAKE_BUILD_TYPE=Release \
      -DGOBLIN_CORE_BUILD_BENCHMARKS=ON -DGOBLIN_CORE_BUILD_TESTS=OFF \
      ${GOBLIN_CORE_ENABLE_RDMA:+-DGOBLIN_CORE_ENABLE_RDMA=ON} || \
     "$CMAKE" "$ROOT" -DCMAKE_BUILD_TYPE=Release \
      -DGOBLIN_CORE_BUILD_BENCHMARKS=ON -DGOBLIN_CORE_BUILD_TESTS=OFF)
  fi
  cmake --build "$BUILD_DIR" --target goblin_core_server \
    goblin_core_ring_latency_benchmark goblin_core_ring_cli \
    -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
fi

[[ -x "$GOBLIN" && -x "$PROBE" && -x "$CLI" ]] || {
  log "missing binaries under $BUILD_DIR"
  exit 1
}

log "NUMA map (server node $SERVER_NODE cpu $SERVER_CPU)"
numactl -H | sed -n '1,20p' >&2 || true
echo "distances from node $SERVER_NODE:" >&2
cat "/sys/devices/system/node/node${SERVER_NODE}/distance" >&2 || true

run_placement() {
  local label="$1" client_cpu="$2" hops="$3"
  local ring="/tmp/goblin-ring-numa-${hops}hop-$$.ring"
  local logf="$OUT_DIR/${label}.log"
  local outf="$OUT_DIR/${label}.out"

  log "=== $label (client cpu $client_cpu, ~$hops hop(s)) ==="
  rm -f "$ring"

  # Bind server CPU + memory + ring NUMA to SERVER_NODE.
  numactl --cpunodebind="$SERVER_NODE" --membind="$SERVER_NODE" \
    taskset -c "$SERVER_CPU" \
    "$GOBLIN" --cpu "$SERVER_CPU" --numa "$SERVER_NODE" \
    --ring "$ring" "$RING_SIZE" \
    >"$logf" 2>&1 &
  local spid=$!

  local ready=0 i
  for i in $(seq 1 100); do
    if [[ -e "$ring" ]] && \
       taskset -c "$client_cpu" "$CLI" "$ring" PING 2>/dev/null | grep -q PONG; then
      ready=1
      break
    fi
    if ! kill -0 "$spid" 2>/dev/null; then
      log "server died for $label"
      cat "$logf" >&2 || true
      return 1
    fi
    sleep 0.05
  done
  if [[ "$ready" != 1 ]]; then
    log "ring not ready ($label)"
    cat "$logf" >&2 || true
    kill "$spid" 2>/dev/null || true
    wait "$spid" 2>/dev/null || true
    return 1
  fi

  # Client: pin CPU; membind to that CPU's NUMA node so stacks/buffers are local
  # to the client (ring pages stay on the server node via goblin --numa).
  local client_node=""
  if [[ -f /sys/devices/system/cpu/cpu${client_cpu}/topology/physical_package_id ]]; then
    :
  fi
  for n in /sys/devices/system/node/node[0-9]*; do
    [[ -f "$n/cpulist" ]] || continue
    # Expand ranges like 0,4,8-12 into a match for this cpu id.
    if python3 -c "
import sys
cpu=int(sys.argv[1]); s=open(sys.argv[2]).read().strip()
def expand(spec):
  out=set()
  for part in spec.split(','):
    if '-' in part:
      a,b=part.split('-'); out.update(range(int(a),int(b)+1))
    else:
      out.add(int(part))
  return out
sys.exit(0 if cpu in expand(s) else 1)
" "$client_cpu" "$n/cpulist" 2>/dev/null; then
      client_node="${n##*node}"
      break
    fi
  done

  if [[ -n "$client_node" ]]; then
    log "client on node $client_node cpu $client_cpu"
    numactl --cpunodebind="$client_node" --membind="$client_node" \
      taskset -c "$client_cpu" \
      "$PROBE" "$ring" "$SAMPLES" "$WARMUP" | tee "$outf"
  else
    log "client cpu $client_cpu (node unresolved)"
    taskset -c "$client_cpu" "$PROBE" "$ring" "$SAMPLES" "$WARMUP" | tee "$outf"
  fi

  kill "$spid" 2>/dev/null || true
  wait "$spid" 2>/dev/null || true
  rm -f "$ring"
  echo "LABEL=$label HOPS=$hops CLIENT_CPU=$client_cpu" >>"$outf"
}

run_placement "same-node"  "$SAME_CPU" 0
run_placement "one-hop"    "$HOP1_CPU" 1
run_placement "two-hop"    "$HOP2_CPU" 2

python3 - "$OUT_DIR" <<'PY'
import re, sys
from pathlib import Path

out_dir = Path(sys.argv[1])
labels = ["same-node", "one-hop", "two-hop"]
# op key -> {label: p50}
ops = {}
order = []

pat = re.compile(
    r"^(?P<label>.+?)\s+"
    r"min\s+(?P<min>[\d.]+)\s+us\s+"
    r"p50\s+(?P<p50>[\d.]+)\s+us\s+"
    r"p90\s+(?P<p90>[\d.]+)\s+us\s+"
    r"p99\s+(?P<p99>[\d.]+)\s+us\s+"
    r"p99\.9\s+(?P<p999>[\d.]+)\s+us\s+"
    r"mean\s+(?P<mean>[\d.]+)\s+us",
    re.M,
)

for lab in labels:
    text = (out_dir / f"{lab}.out").read_text(errors="replace")
    for m in pat.finditer(text):
        raw = m.group("label").strip()
        if "min " in raw:
            continue
        key = re.sub(r"/(?:ring|RDMA)\b", "", raw)
        key = re.sub(r"\s+", " ", key).strip()
        if key not in ops:
            ops[key] = {}
            order.append(key)
        ops[key][lab] = {
            "p50": float(m.group("p50")),
            "p99": float(m.group("p99")),
            "mean": float(m.group("mean")),
        }

print()
print("## Shared-memory ring latency vs NUMA hops")
print()
print("Server fixed on one NUMA node (`--cpu` + `--numa` + `numactl --membind`).")
print("Client moves: same node → one hop → two hops.")
print()
print("| op | same p50 (µs) | 1-hop p50 | 2-hop p50 | Δ1 (µs) | Δ2 (µs) | same p99 | 1-hop p99 | 2-hop p99 |")
print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")

def f(x):
    return f"{x:.3f}"

for key in order:
    r = ops[key]
    if not all(l in r for l in labels):
        continue
    s, a, b = r["same-node"], r["one-hop"], r["two-hop"]
    d1 = a["p50"] - s["p50"]
    d2 = b["p50"] - s["p50"]
    print(
        f"| {key} | `{f(s['p50'])}` | `{f(a['p50'])}` | `{f(b['p50'])}` | "
        f"`{f(d1)}` | `{f(d2)}` | `{f(s['p99'])}` | `{f(a['p99'])}` | `{f(b['p99'])}` |"
    )

print()
print("| op | same mean | 1-hop mean | 2-hop mean | Δ1 mean | Δ2 mean |")
print("| --- | ---: | ---: | ---: | ---: | ---: |")
for key in order:
    r = ops[key]
    if not all(l in r for l in labels):
        continue
    s, a, b = r["same-node"], r["one-hop"], r["two-hop"]
    print(
        f"| {key} | `{f(s['mean'])}` | `{f(a['mean'])}` | `{f(b['mean'])}` | "
        f"`{f(a['mean']-s['mean'])}` | `{f(b['mean']-s['mean'])}` |"
    )

print()
print(f"_Artifacts: `{out_dir}`_")
print()
print("Δ1 = one-hop − same-node, Δ2 = two-hop − same-node (p50 / mean).")
PY

log "artifacts: $OUT_DIR"
