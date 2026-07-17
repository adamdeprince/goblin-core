#!/usr/bin/env bash
# InfiniBand RDMA latency demo: goblin-core server on thunder, client on butterfly.
#
# Default endpoints (IPoIB lab fabric, see docs/infiniband-setup.md):
#   thunder   10.88.88.3   server, CPU 5 (NUMA node 1 / HCA-local)
#   butterfly 10.88.88.4   client, CPU 5
#
# NOTE: Mac ~/dev/packrat is NOT the same tree as Linux NFS /home/adam/dev/packrat.
# Sync source before building if you edit on a laptop:
#   rsync -a --exclude 'build*/' --exclude '.git/' ./ thunder:dev/packrat/
#
# Usage:
#   bash benchmarks/rdma_thunder_butterfly.sh
#   SAMPLES=50000 WARMUP=5000 SKIP_BUILD=1 bash benchmarks/rdma_thunder_butterfly.sh
#
set -euo pipefail

REMOTE_ROOT="${REMOTE_ROOT:-/home/adam/dev/packrat}"
SERVER_HOST="${SERVER_HOST:-thunder}"
CLIENT_HOST="${CLIENT_HOST:-butterfly}"
SERVER_IB_IP="${SERVER_IB_IP:-10.88.88.3}"
CLIENT_IB_IP="${CLIENT_IB_IP:-10.88.88.4}"
RDMA_PORT="${RDMA_PORT:-6380}"
RING_SIZE="${RING_SIZE:-64kb}"
SERVER_CPU="${SERVER_CPU:-5}"
CLIENT_CPU="${CLIENT_CPU:-5}"
SAMPLES="${SAMPLES:-100000}"
WARMUP="${WARMUP:-10000}"
BUILD_DIR="${BUILD_DIR:-$REMOTE_ROOT/build-rdma}"
SKIP_BUILD="${SKIP_BUILD:-0}"

GOBLIN="$BUILD_DIR/goblin-core"
PROBE="$BUILD_DIR/goblin_core_rdma_latency_benchmark"
CLI="$BUILD_DIR/redis-cli-ring"
START="$REMOTE_ROOT/benchmarks/rdma_ib_server_start.sh"
STOP="$REMOTE_ROOT/benchmarks/rdma_ib_server_stop.sh"

log() { printf '[rdma-ib] %s\n' "$*" >&2; }
remote() { ssh -o BatchMode=yes -o ConnectTimeout=15 "$1" "${@:2}"; }

log "preflight: fabric"
remote "$SERVER_HOST" "ibstat 2>/dev/null | sed -n '1,16p'; ip -br a | grep 10.88.88 || true"
remote "$CLIENT_HOST" "ibstat 2>/dev/null | sed -n '1,16p'; ip -br a | grep 10.88.88 || true"
remote "$SERVER_HOST" "ping -c 1 -W 2 $CLIENT_IB_IP >/dev/null && echo IPoIB thunder-\>butterfly OK"
remote "$CLIENT_HOST" "ping -c 1 -W 2 $SERVER_IB_IP >/dev/null && echo IPoIB butterfly-\>thunder OK"

if [[ "$SKIP_BUILD" != "1" ]]; then
  log "build on $SERVER_HOST ($BUILD_DIR)"
  remote "$SERVER_HOST" "bash -s" <<EOF
set -euo pipefail
CMAKE=\$HOME/.local/lib/python3.10/site-packages/cmake/data/bin/cmake
[[ -x "\$CMAKE" ]] || CMAKE=\$(command -v cmake)
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [[ ! -f CMakeCache.txt ]] || ! grep -q 'GOBLIN_HAS_RDMA:INTERNAL=1\\\|GOBLIN_CORE_ENABLE_RDMA:BOOL=ON' CMakeCache.txt; then
  "\$CMAKE" "$REMOTE_ROOT" -DCMAKE_BUILD_TYPE=Release \\
    -DGOBLIN_CORE_ENABLE_RDMA=ON -DGOBLIN_CORE_BUILD_BENCHMARKS=ON -DGOBLIN_CORE_BUILD_TESTS=OFF
fi
cmake --build . --target goblin_core_server goblin_core_rdma_latency_benchmark goblin_core_ring_cli -j"\$(nproc)"
test -x goblin-core && test -x goblin_core_rdma_latency_benchmark && test -x redis-cli-ring
EOF
else
  log "SKIP_BUILD=1"
fi

remote "$SERVER_HOST" "test -x '$GOBLIN' && test -x '$PROBE' && test -x '$CLI'"
remote "$CLIENT_HOST" "test -x '$PROBE' && test -x '$CLI'"

cleanup() {
  log "stop server"
  remote "$SERVER_HOST" "SERVER_PIDFILE=/tmp/goblin-rdma-server.pid bash '$STOP'" || true
}
trap cleanup EXIT

log "start server on $SERVER_HOST"
remote "$SERVER_HOST" "SERVER_IB_IP=$SERVER_IB_IP RDMA_PORT=$RDMA_PORT RING_SIZE=$RING_SIZE SERVER_CPU=$SERVER_CPU BUILD_DIR=$BUILD_DIR REMOTE_ROOT=$REMOTE_ROOT bash '$START'"

log "wait for RDMA accept"
ready=0
for _ in $(seq 1 40); do
  if remote "$CLIENT_HOST" "taskset -c $CLIENT_CPU '$CLI' --rdma $SERVER_IB_IP $RDMA_PORT $RING_SIZE PING 2>/dev/null" | grep -q PONG; then
    ready=1
    break
  fi
  sleep 0.25
done
if [[ "$ready" != 1 ]]; then
  log "not ready; server log:"
  remote "$SERVER_HOST" "tail -40 /tmp/goblin-rdma-server.log" || true
  exit 1
fi

log "sanity commands"
remote "$CLIENT_HOST" "taskset -c $CLIENT_CPU bash -s" <<EOF
set -euo pipefail
C=(taskset -c $CLIENT_CPU $CLI --rdma $SERVER_IB_IP $RDMA_PORT $RING_SIZE)
"\${C[@]}" PING
"\${C[@]}" SET showoff:str hello-from-butterfly
"\${C[@]}" GET showoff:str
"\${C[@]}" HSET showoff:hash field1 value1
"\${C[@]}" HGET showoff:hash field1
EOF

log "latency ($SAMPLES samples, $WARMUP warmup) on $CLIENT_HOST"
echo
remote "$CLIENT_HOST" "taskset -c $CLIENT_CPU '$PROBE' $SERVER_IB_IP $RDMA_PORT $RING_SIZE $SAMPLES $WARMUP"
echo
log "done"
