#!/usr/bin/env bash
# Local shared-memory ring latency: same ops as the RDMA thunder/butterfly probe.
#
# Starts goblin-core with --ring on this host, runs the depth-one client
# (PING / SET / GET / HSET / HGET for RESP + SBE PING) on the same machine.
#
# Usage:
#   bash benchmarks/ring_local_latency.sh
#   SAMPLES=50000 WARMUP=5000 SKIP_BUILD=1 bash benchmarks/ring_local_latency.sh
#   BUILD_DIR=./build-rel SERVER_CPU=2 CLIENT_CPU=3 bash benchmarks/ring_local_latency.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT/build-rel}"
# Prefer an RDMA-enabled Linux build if present (still works for local rings).
if [[ ! -x "$BUILD_DIR/goblin-core" && -x "$ROOT/build-rdma/goblin-core" ]]; then
  BUILD_DIR="$ROOT/build-rdma"
fi

GOBLIN="${GOBLIN:-$BUILD_DIR/goblin-core}"
PROBE="${PROBE:-$BUILD_DIR/goblin_core_ring_latency_benchmark}"
CLI="${CLI:-$BUILD_DIR/redis-cli-ring}"
RING_PATH="${RING_PATH:-/tmp/goblin-ring-lat-$$.ring}"
RING_SIZE="${RING_SIZE:-64kb}"
# Default: both cores on the same NUMA node (thunder node 1 = cpus 1,5,9,...).
# Cross-node costs ~+1 µs/hop — see benchmarks/ring_numa_latency.sh.
SERVER_CPU="${SERVER_CPU:-5}"
CLIENT_CPU="${CLIENT_CPU:-9}"
SERVER_NODE="${SERVER_NODE:-}"  # if set, pass --numa and numactl membind
SAMPLES="${SAMPLES:-100000}"
WARMUP="${WARMUP:-10000}"
SKIP_BUILD="${SKIP_BUILD:-0}"

log() { printf '[ring-local] %s\n' "$*" >&2; }

if [[ "$SKIP_BUILD" != "1" ]]; then
  log "build ($BUILD_DIR)"
  mkdir -p "$BUILD_DIR"
  if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    CMAKE="${CMAKE:-$(command -v cmake)}"
    if [[ -x "$HOME/.local/lib/python3.10/site-packages/cmake/data/bin/cmake" ]]; then
      CMAKE="$HOME/.local/lib/python3.10/site-packages/cmake/data/bin/cmake"
    fi
    (cd "$BUILD_DIR" && "$CMAKE" "$ROOT" -DCMAKE_BUILD_TYPE=Release \
      -DGOBLIN_CORE_BUILD_BENCHMARKS=ON -DGOBLIN_CORE_BUILD_TESTS=OFF)
  fi
  cmake --build "$BUILD_DIR" --target goblin_core_server \
    goblin_core_ring_latency_benchmark goblin_core_ring_cli -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"
fi

if [[ ! -x "$GOBLIN" || ! -x "$PROBE" ]]; then
  log "missing binaries: GOBLIN=$GOBLIN PROBE=$PROBE"
  exit 1
fi

TASKSET_S=()
TASKSET_C=()
if command -v taskset >/dev/null 2>&1; then
  TASKSET_S=(taskset -c "$SERVER_CPU")
  TASKSET_C=(taskset -c "$CLIENT_CPU")
fi
if [[ -n "$SERVER_NODE" ]] && command -v numactl >/dev/null 2>&1; then
  TASKSET_S=(numactl --cpunodebind="$SERVER_NODE" --membind="$SERVER_NODE" taskset -c "$SERVER_CPU")
fi

rm -f "$RING_PATH"
LOG="/tmp/goblin-ring-local-$$.log"
log "start server --ring $RING_PATH $RING_SIZE (cpu ${SERVER_CPU}${SERVER_NODE:+ node $SERVER_NODE})"
NUMA_ARGS=()
if [[ -n "$SERVER_NODE" ]]; then
  NUMA_ARGS=(--numa "$SERVER_NODE")
fi
"${TASKSET_S[@]}" "$GOBLIN" --cpu "$SERVER_CPU" "${NUMA_ARGS[@]}" \
  --ring "$RING_PATH" "$RING_SIZE" \
  >"$LOG" 2>&1 &
SERVER_PID=$!

cleanup() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  rm -f "$RING_PATH"
}
trap cleanup EXIT

# Wait for ring file + PING
ready=0
for _ in $(seq 1 80); do
  if [[ -e "$RING_PATH" ]] && \
     "${TASKSET_C[@]}" "$CLI" "$RING_PATH" PING 2>/dev/null | grep -q PONG; then
    ready=1
    break
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    log "server died; log:"
    cat "$LOG" >&2 || true
    exit 1
  fi
  sleep 0.05
done
if [[ "$ready" != 1 ]]; then
  log "ring not ready; log:"
  cat "$LOG" >&2 || true
  exit 1
fi

log "sanity (local client)"
"${TASKSET_C[@]}" "$CLI" "$RING_PATH" PING
"${TASKSET_C[@]}" "$CLI" "$RING_PATH" SET showoff:str hello-local
"${TASKSET_C[@]}" "$CLI" "$RING_PATH" GET showoff:str
"${TASKSET_C[@]}" "$CLI" "$RING_PATH" HSET showoff:hash field1 value1
"${TASKSET_C[@]}" "$CLI" "$RING_PATH" HGET showoff:hash field1

log "latency ($SAMPLES samples, $WARMUP warmup) on client cpu ${CLIENT_CPU}"
echo
"${TASKSET_C[@]}" "$PROBE" "$RING_PATH" "$SAMPLES" "$WARMUP"
echo
log "done (server log $LOG)"
