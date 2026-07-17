#!/usr/bin/env bash
# Side-by-side depth-one latency: local shared-memory ring vs InfiniBand RDMA.
#
# Same ops (SBE PING, RESP PING/SET/GET/HSET/HGET), same sample counts, same
# output format — so the delta is “what the wire + RDMA path add” on top of the
# in-process ring path.
#
# Layout (defaults):
#   local ring  — server+client on thunder  (NUMA-local cores)
#   RDMA        — server on thunder, client on butterfly  (IPoIB 10.88.88.x)
#
# Usage:
#   bash benchmarks/ring_vs_rdma_latency.sh
#   SAMPLES=50000 WARMUP=5000 SKIP_BUILD=1 bash benchmarks/ring_vs_rdma_latency.sh
#
# NOTE: If you edit on a laptop whose tree is not the Linux NFS home, rsync first:
#   rsync -a --exclude 'build*/' --exclude '.git/' ./ thunder:dev/packrat/
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_ROOT="${REMOTE_ROOT:-/home/adam/dev/packrat}"
SAMPLES="${SAMPLES:-100000}"
WARMUP="${WARMUP:-10000}"
SKIP_BUILD="${SKIP_BUILD:-0}"
BUILD_DIR="${BUILD_DIR:-$REMOTE_ROOT/build-rdma}"
OUT_DIR="${OUT_DIR:-/tmp/goblin-ring-vs-rdma-$$}"
mkdir -p "$OUT_DIR"

log() { printf '[ring-vs-rdma] %s\n' "$*" >&2; }

log "1/2 local shared-memory ring (on thunder)"
ssh -o BatchMode=yes thunder \
  "SAMPLES=$SAMPLES WARMUP=$WARMUP SKIP_BUILD=$SKIP_BUILD BUILD_DIR=$BUILD_DIR \
   bash $REMOTE_ROOT/benchmarks/ring_local_latency.sh" \
  2>"$OUT_DIR/local.err" | tee "$OUT_DIR/local.out"

log "2/2 RDMA InfiniBand (server thunder, client butterfly)"
ssh -o BatchMode=yes "$(hostname 2>/dev/null || echo localhost)" true 2>/dev/null || true
# Drive harness from this machine (must SSH both hosts).
SAMPLES=$SAMPLES WARMUP=$WARMUP SKIP_BUILD=1 REMOTE_ROOT=$REMOTE_ROOT BUILD_DIR=$BUILD_DIR \
  bash "$HERE/rdma_thunder_butterfly.sh" \
  2>"$OUT_DIR/rdma.err" | tee "$OUT_DIR/rdma.out"

# --- parse probe lines: "LABEL ... p50 X us ... p99 Y us ... mean Z us ..." ---
python3 - "$OUT_DIR/local.out" "$OUT_DIR/rdma.out" <<'PY'
import re, sys
from pathlib import Path

def parse(path: str) -> dict[str, dict[str, float]]:
    # Normalize labels: "SBE/ring PING" vs "SBE/RDMA PING" -> key "SBE PING"
    rows = {}
    text = Path(path).read_text(errors="replace")
    # label is left-padded to 22 cols, then "min ..."
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
        label = m.group("label").strip()
        if "min " in label:  # skip header-ish lines
            continue
        # Collapse transport token for pairing
        key = re.sub(r"/(?:ring|RDMA)\b", "", label)
        key = re.sub(r"\s+", " ", key).strip()
        rows[key] = {k: float(m.group(k)) for k in ("min", "p50", "p90", "p99", "p999", "mean")}
        rows[key]["_raw"] = label
    return rows

local = parse(sys.argv[1])
rdma = parse(sys.argv[2])
keys = [k for k in local if k in rdma]
if not keys:
    # fallback order
    keys = sorted(set(local) | set(rdma))

print()
print("## Local shared-memory ring vs InfiniBand RDMA (depth-one round trip)")
print()
print("Same Goblin server binary and op mix. Local ring is IPC on one host;")
print("RDMA is thunder → butterfly over the polled InfiniBand ring.")
print("**Δ** = RDMA − local (what the fabric + RDMA path add).")
print()
print("| op | local p50 (µs) | RDMA p50 (µs) | Δ p50 (µs) | overhead | local p99 | RDMA p99 | Δ p99 |")
print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")

def fmt(x):
    return f"{x:.3f}"

for key in keys:
    if key not in local or key not in rdma:
        continue
    L, R = local[key], rdma[key]
    d50 = R["p50"] - L["p50"]
    d99 = R["p99"] - L["p99"]
    ov = (d50 / L["p50"] * 100.0) if L["p50"] > 0 else float("nan")
    print(
        f"| {key} | `{fmt(L['p50'])}` | `{fmt(R['p50'])}` | "
        f"`{fmt(d50)}` | `{ov:.0f}%` | `{fmt(L['p99'])}` | `{fmt(R['p99'])}` | `{fmt(d99)}` |"
    )

print()
print("| op | local mean (µs) | RDMA mean (µs) | Δ mean (µs) |")
print("| --- | ---: | ---: | ---: |")
for key in keys:
    if key not in local or key not in rdma:
        continue
    L, R = local[key], rdma[key]
    print(
        f"| {key} | `{fmt(L['mean'])}` | `{fmt(R['mean'])}` | "
        f"`{fmt(R['mean'] - L['mean'])}` |"
    )

print()
print("_Raw probe logs:_")
print(f"- local: `{sys.argv[1]}`")
print(f"- rdma:  `{sys.argv[2]}`")
PY

log "artifacts under $OUT_DIR"
