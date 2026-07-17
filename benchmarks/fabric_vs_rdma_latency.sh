#!/usr/bin/env bash
# Show that Goblin InfiniBand RDMA sits next to the machine's own fabric.
#
# 1) Local shared-memory ring, server fixed on one NUMA node, client at:
#      0 hops (same node) · 1 hop · 2 hops   ← machine fabric cost
# 2) RDMA: server on thunder, client on butterfly   ← IB + our RDMA path
#
# Same ops and sample counts throughout. The point of the table is not
# “RDMA vs ideal IPC” alone, but “RDMA vs how expensive *this box already is*
# when the client is one or two QPI hops away.”
#
# Usage:
#   bash benchmarks/fabric_vs_rdma_latency.sh
#   SAMPLES=50000 WARMUP=5000 SKIP_BUILD=1 bash benchmarks/fabric_vs_rdma_latency.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_ROOT="${REMOTE_ROOT:-/home/adam/dev/packrat}"
SAMPLES="${SAMPLES:-50000}"
WARMUP="${WARMUP:-5000}"
SKIP_BUILD="${SKIP_BUILD:-0}"
BUILD_DIR="${BUILD_DIR:-$REMOTE_ROOT/build-rdma}"
OUT_DIR="${OUT_DIR:-/tmp/goblin-fabric-vs-rdma-$$}"
mkdir -p "$OUT_DIR"

log() { printf '[fabric-vs-rdma] %s\n' "$*" >&2; }

log "1/2 NUMA hop sweep (local ring on thunder)"
ssh -o BatchMode=yes thunder \
  "SAMPLES=$SAMPLES WARMUP=$WARMUP SKIP_BUILD=$SKIP_BUILD BUILD_DIR=$BUILD_DIR \
   OUT_DIR=$OUT_DIR/numa \
   bash $REMOTE_ROOT/benchmarks/ring_numa_latency.sh" \
  2>"$OUT_DIR/numa.err" | tee "$OUT_DIR/numa.console"

# ring_numa writes under its OUT_DIR on thunder — copy artifacts back if remote
ssh -o BatchMode=yes thunder \
  "test -d $OUT_DIR/numa && tar -C $OUT_DIR/numa -cf - . 2>/dev/null" \
  2>/dev/null | tar -C "$OUT_DIR" -xf - 2>/dev/null || true

# NUMA script prints artifacts path on thunder; also re-read *.out from thunder
NUMA_DIR=$(ssh -o BatchMode=yes thunder "ls -td /tmp/goblin-ring-numa-* 2>/dev/null | head -1" || true)
if [[ -n "${NUMA_DIR:-}" ]]; then
  mkdir -p "$OUT_DIR/numa"
  scp -q "thunder:${NUMA_DIR}/same-node.out" "thunder:${NUMA_DIR}/one-hop.out" \
      "thunder:${NUMA_DIR}/two-hop.out" "$OUT_DIR/numa/" 2>/dev/null || true
fi

log "2/2 RDMA (thunder server → butterfly client)"
SAMPLES=$SAMPLES WARMUP=$WARMUP SKIP_BUILD=1 REMOTE_ROOT=$REMOTE_ROOT BUILD_DIR=$BUILD_DIR \
  bash "$HERE/rdma_thunder_butterfly.sh" \
  2>"$OUT_DIR/rdma.err" | tee "$OUT_DIR/rdma.out"

python3 - "$OUT_DIR" <<'PY'
import re, sys
from pathlib import Path

out = Path(sys.argv[1])

def parse_file(path: Path) -> dict[str, dict[str, float]]:
    rows = {}
    if not path.is_file():
        return rows
    text = path.read_text(errors="replace")
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
    for m in pat.finditer(text):
        raw = m.group("label").strip()
        if "min " in raw:
            continue
        key = re.sub(r"/(?:ring|RDMA)\b", "", raw)
        key = re.sub(r"\s+", " ", key).strip()
        rows[key] = {k: float(m.group(k)) for k in ("p50", "p99", "mean")}
    return rows

cols = {
    "same-node": parse_file(out / "numa" / "same-node.out"),
    "1-hop": parse_file(out / "numa" / "one-hop.out"),
    "2-hop": parse_file(out / "numa" / "two-hop.out"),
    "RDMA": parse_file(out / "rdma.out"),
}

# stable op order from same-node if present
order = list(cols["same-node"].keys()) or list(cols["RDMA"].keys())

print()
print("## Machine fabric vs InfiniBand RDMA (depth-one round trip)")
print()
print("**Point of the table:** Goblin's polled InfiniBand path should sit *near*")
print("the cost of this multi-socket box talking to itself across QPI — not near")
print("TCP, and not only compared to ideal same-core IPC.")
print()
print("| placement | what it is |")
print("| --- | --- |")
print("| **same-node** | Shared-memory ring; client+server on one NUMA node |")
print("| **1-hop / 2-hop** | Same ring; client on a farther socket (machine fabric) |")
print("| **RDMA** | Polled RDMA ring; thunder → butterfly over InfiniBand |")
print()
print("| op | same-node p50 | 1-hop p50 | 2-hop p50 | **RDMA p50** | RDMA − 2-hop | RDMA − same |")
print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |")

def f(x):
    return f"{x:.3f}"

for key in order:
    def get(col):
        return cols[col].get(key, {}).get("p50")
    s, a, b, r = get("same-node"), get("1-hop"), get("2-hop"), get("RDMA")
    if None in (s, a, b, r):
        continue
    print(
        f"| {key} | `{f(s)}` | `{f(a)}` | `{f(b)}` | **`{f(r)}`** | "
        f"`{f(r - b)}` | `{f(r - s)}` |"
    )

print()
print("**How to read it**")
print()
print("- **same-node** is best-case local IPC (busy-polled shared memory).")
print("- **1-hop / 2-hop** are what *this host already pays* to reach another socket.")
print("- **RDMA − 2-hop** is the interesting column: how much farther InfiniBand")
print("  + Goblin's RDMA path is than the machine's own far fabric hop.")
print("- **RDMA − same** is total cost over ideal local IPC (includes fabric + wire).")
print()
print(f"_Artifacts: `{out}`_")
PY

log "done — artifacts $OUT_DIR"
