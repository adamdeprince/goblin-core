#!/usr/bin/env bash
# Matched Goblin RESP2 latency over a local shared-memory ring and a direct
# 100 GbE link, with kernel TCP and native XLIO Ultra endpoints. The same C++
# probe, fixtures, commands, warmup, and sample count are used in every case.
#
# The controller needs passwordless SSH to SERVER_HOST and CLIENT_HOST. The
# source/build directory is expected to be visible at REMOTE_ROOT on both hosts.
#
# Usage:
#   bash benchmarks/xlio_latency.sh
#   SAMPLES=10000 WARMUP=1000 SKIP_BUILD=1 bash benchmarks/xlio_latency.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SERVER_HOST="${SERVER_HOST:-butterfly}"
CLIENT_HOST="${CLIENT_HOST:-rain}"
SERVER_ADDRESS="${SERVER_ADDRESS:-10.100.0.1}"
CLIENT_ADDRESS="${CLIENT_ADDRESS:-10.100.0.2}"
NIC="${NIC:-enp68s0np0}"
SERVER_CPU="${SERVER_CPU:-5}"
RING_CLIENT_CPU="${RING_CLIENT_CPU:-9}"
NETWORK_CLIENT_CPU="${NETWORK_CLIENT_CPU:-5}"
NUMA_NODE="${NUMA_NODE:-1}"
RING_SIZE="${RING_SIZE:-2mb}"
KERNEL_PORT="${KERNEL_PORT:-17600}"
XLIO_PORT="${XLIO_PORT:-17601}"
LOOPBACK_PORT="${LOOPBACK_PORT:-17699}"
SAMPLES="${SAMPLES:-1000000}"
WARMUP="${WARMUP:-100000}"
SKIP_BUILD="${SKIP_BUILD:-0}"

REMOTE_ROOT="${REMOTE_ROOT:-/home/adam/xlio-goblin-core}"
BUILD_DIR="${BUILD_DIR:-$REMOTE_ROOT/build-xlio-native}"
GOBLIN="${GOBLIN:-$BUILD_DIR/goblin-core}"
PROBE="${PROBE:-$BUILD_DIR/goblin_core_xlio_latency_benchmark}"
CLI="${CLI:-$BUILD_DIR/redis-cli-ring}"
XLIO_PREFIX="${XLIO_PREFIX:-/home/adam/opt/goblin-xlio-3.61.2/prefix}"
XLIO_LIBRARY="${XLIO_LIBRARY:-$XLIO_PREFIX/lib/libxlio.so}"
CMAKE="${CMAKE:-/home/adam/.local/lib/python3.10/site-packages/cmake/data/bin/cmake}"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${OUT_DIR:-/tmp/goblin-xlio-latency-$STAMP}"
mkdir -p "$OUT_DIR"

SSH=(ssh -o BatchMode=yes)
SCP=(scp -q)
RING_PATH="/tmp/goblin-xlio-latency-${USER}-$$.ring"
PID_FILE="/tmp/goblin-xlio-latency-${USER}-$$.pid"
REMOTE_LOG="/tmp/goblin-xlio-latency-${USER}-$$.log"
ACTIVE_SERVER=""

log() { printf '[xlio-latency] %s\n' "$*" >&2; }
remote() { "${SSH[@]}" "$1" "$2"; }

verify_numa_endpoint() {
  local host=$1
  local cpu=$2
  if ! remote "$host" \
    "test \"\$(cat '/sys/class/net/$NIC/device/numa_node')\" = '$NUMA_NODE' && test -e '/sys/devices/system/cpu/cpu$cpu/node$NUMA_NODE' && command -v numactl >/dev/null"; then
    log "$host: $NIC and CPU $cpu must both belong to NUMA node $NUMA_NODE"
    remote "$host" \
      "printf 'nic node: '; cat '/sys/class/net/$NIC/device/numa_node'; lscpu -e=CPU,NODE,SOCKET,CORE | awk 'NR == 1 || \$1 == $cpu'" \
      >&2 || true
    exit 1
  fi
}

stop_user_server() {
  remote "$SERVER_HOST" \
    "if test -s '$PID_FILE'; then p=\$(cat '$PID_FILE'); kill -TERM \"\$p\" 2>/dev/null || true; for i in \$(seq 1 50); do kill -0 \"\$p\" 2>/dev/null || break; sleep 0.02; done; kill -KILL \"\$p\" 2>/dev/null || true; fi; rm -f '$PID_FILE' '$RING_PATH'" \
    >/dev/null 2>&1 || true
}

stop_root_server() {
  remote "$SERVER_HOST" \
    "if test -s '$PID_FILE'; then p=\$(cat '$PID_FILE'); c=\$(pgrep -P \"\$p\" | head -1 || true); test -z \"\$c\" || sudo -n kill -TERM \"\$c\" 2>/dev/null || true; for i in \$(seq 1 100); do test -z \"\$c\" || sudo -n kill -0 \"\$c\" 2>/dev/null || break; sleep 0.02; done; test -z \"\$c\" || sudo -n kill -KILL \"\$c\" 2>/dev/null || true; sudo -n kill -TERM \"\$p\" 2>/dev/null || true; fi; rm -f '$PID_FILE'" \
    >/dev/null 2>&1 || true
}

cleanup() {
  if [[ "$ACTIVE_SERVER" == "root" ]]; then
    stop_root_server
  elif [[ "$ACTIVE_SERVER" == "user" ]]; then
    stop_user_server
  fi
}
trap cleanup EXIT INT TERM

copy_server_log() {
  local name=$1
  "${SCP[@]}" "$SERVER_HOST:$REMOTE_LOG" "$OUT_DIR/$name.server.log" \
    2>/dev/null || true
}

start_user_server() {
  local arguments=$1
  stop_user_server
  remote "$SERVER_HOST" \
    "nohup numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$SERVER_CPU' '$GOBLIN' --cpu '$SERVER_CPU' --numa '$NUMA_NODE' --port '$LOOPBACK_PORT' $arguments >'$REMOTE_LOG' 2>&1 </dev/null & echo \$! >'$PID_FILE'"
  ACTIVE_SERVER="user"
}

start_xlio_server() {
  stop_root_server
  remote "$SERVER_HOST" \
    "nohup sudo -n env XLIO_MEM_ALLOC_TYPE=ANON XLIO_TRACELEVEL=WARNING LD_LIBRARY_PATH='$XLIO_PREFIX/lib' LD_PRELOAD='$XLIO_LIBRARY' numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$SERVER_CPU' '$GOBLIN' --cpu '$SERVER_CPU' --numa '$NUMA_NODE' --port '$LOOPBACK_PORT' --xlio '$SERVER_ADDRESS' '$XLIO_PORT' >'$REMOTE_LOG' 2>&1 </dev/null & echo \$! >'$PID_FILE'"
  ACTIVE_SERVER="root"
}

wait_ring() {
  for _ in $(seq 1 100); do
    if remote "$SERVER_HOST" \
      "test -e '$RING_PATH' && numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$RING_CLIENT_CPU' '$CLI' '$RING_PATH' PING 2>/dev/null | grep -q PONG"; then
      return 0
    fi
    sleep 0.05
  done
  remote "$SERVER_HOST" "cat '$REMOTE_LOG'" >&2 || true
  return 1
}

wait_tcp() {
  local port=$1
  for _ in $(seq 1 100); do
    if remote "$CLIENT_HOST" \
      "printf '*1\\r\\n\$4\\r\\nPING\\r\\n' | nc -w 1 '$SERVER_ADDRESS' '$port' 2>/dev/null | grep -q PONG"; then
      return 0
    fi
    sleep 0.05
  done
  remote "$SERVER_HOST" "cat '$REMOTE_LOG'" >&2 || true
  return 1
}

run_probe() {
  local host=$1
  local name=$2
  local command=$3
  log "$name"
  remote "$host" "$command" \
    > >(tee "$OUT_DIR/$name.out") \
    2> >(tee "$OUT_DIR/$name.err" >&2)
}

if [[ "$SKIP_BUILD" != "1" ]]; then
  log "build native XLIO server, client, and probe on $SERVER_HOST"
  remote "$SERVER_HOST" \
    "'$CMAKE' --build '$BUILD_DIR' --target goblin_core_server goblin_core_ring_cli goblin_core_xlio_latency_benchmark -j 8"
fi

for binary in "$GOBLIN" "$PROBE" "$CLI" "$XLIO_LIBRARY"; do
  remote "$SERVER_HOST" "test -e '$binary'" || {
    log "missing remote artifact: $binary"
    exit 1
  }
done

verify_numa_endpoint "$SERVER_HOST" "$SERVER_CPU"
verify_numa_endpoint "$SERVER_HOST" "$RING_CLIENT_CPU"
verify_numa_endpoint "$CLIENT_HOST" "$NETWORK_CLIENT_CPU"

{
  printf 'timestamp_utc=%s\n' "$STAMP"
  printf 'git_commit=%s\n' "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
  printf 'server_host=%s\nclient_host=%s\n' "$SERVER_HOST" "$CLIENT_HOST"
  printf 'server_address=%s\nclient_address=%s\ninterface=%s\n' \
    "$SERVER_ADDRESS" "$CLIENT_ADDRESS" "$NIC"
  printf 'server_cpu=%s\nring_client_cpu=%s\nnetwork_client_cpu=%s\nnuma_node=%s\n' \
    "$SERVER_CPU" "$RING_CLIENT_CPU" "$NETWORK_CLIENT_CPU" "$NUMA_NODE"
  printf 'numa_binding=cpunodebind+membind+exact-taskset\n'
  printf 'ring_size_per_direction=%s\nsamples=%s\nwarmup=%s\n' \
    "$RING_SIZE" "$SAMPLES" "$WARMUP"
  printf 'xlio_version=3.61.2\n'
  for host in "$SERVER_HOST" "$CLIENT_HOST"; do
    printf '\n[%s]\n' "$host"
    remote "$host" \
      "hostname; uname -sr; lscpu | grep -E 'Model name|Socket.s.|NUMA node.s.'; ip -br addr show '$NIC'; printf 'nic_numa='; cat '/sys/class/net/$NIC/device/numa_node'; ethtool '$NIC' 2>/dev/null | grep -E 'Speed:|Duplex:|Port:|Link detected:'; ethtool -i '$NIC' 2>/dev/null | grep -E 'driver:|firmware-version:|bus-info:'; ethtool -c '$NIC' 2>/dev/null | grep -E 'Adaptive RX:|rx-usecs:|rx-frames:|tx-usecs:|tx-frames:'; ethtool -g '$NIC' 2>/dev/null | grep -E 'RX:|TX:'; printf 'governor='; cat '/sys/devices/system/cpu/cpu$NETWORK_CLIENT_CPU/cpufreq/scaling_governor' 2>/dev/null || true"
  done
} >"$OUT_DIR/metadata.txt"

log "1/4 local RESP ring: server CPU $SERVER_CPU, client CPU $RING_CLIENT_CPU, node $NUMA_NODE"
start_user_server "--ring '$RING_PATH' '$RING_SIZE'"
wait_ring
run_probe "$SERVER_HOST" "ring" \
  "numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$RING_CLIENT_CPU' '$PROBE' ring '$RING_PATH' ring-resp '$SAMPLES' '$WARMUP'"
copy_server_log ring
stop_user_server
ACTIVE_SERVER=""

log "2/4 kernel TCP server + kernel TCP client"
start_user_server "--trusted-listen '$SERVER_ADDRESS:$KERNEL_PORT'"
wait_tcp "$KERNEL_PORT"
run_probe "$CLIENT_HOST" "kernel-kernel" \
  "numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$NETWORK_CLIENT_CPU' '$PROBE' tcp '$SERVER_ADDRESS' '$KERNEL_PORT' kernel-kernel '$SAMPLES' '$WARMUP' '$CLIENT_ADDRESS'"
copy_server_log kernel-kernel
stop_user_server
ACTIVE_SERVER=""

log "3/4 native XLIO server + ordinary kernel TCP client"
start_xlio_server
wait_tcp "$XLIO_PORT"
run_probe "$CLIENT_HOST" "xlio-kernel" \
  "numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$NETWORK_CLIENT_CPU' '$PROBE' tcp '$SERVER_ADDRESS' '$XLIO_PORT' xlio-kernel '$SAMPLES' '$WARMUP' '$CLIENT_ADDRESS'"
copy_server_log xlio-kernel
stop_root_server
ACTIVE_SERVER=""

log "4/4 native XLIO server + native XLIO client"
start_xlio_server
wait_tcp "$XLIO_PORT"
run_probe "$CLIENT_HOST" "xlio-xlio" \
  "sudo -n env XLIO_MEM_ALLOC_TYPE=ANON XLIO_TRACELEVEL=WARNING LD_LIBRARY_PATH='$XLIO_PREFIX/lib' LD_PRELOAD='$XLIO_LIBRARY' numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$NETWORK_CLIENT_CPU' '$PROBE' xlio '$SERVER_ADDRESS' '$XLIO_PORT' xlio-xlio '$SAMPLES' '$WARMUP' '$CLIENT_ADDRESS'"
copy_server_log xlio-xlio
stop_root_server
ACTIVE_SERVER=""

{
  echo 'label,operation,min_us,p50_us,p75_us,p90_us,p95_us,p99_us,p99_9_us,p99_99_us,max_us,mean_us,qps,samples'
  awk -F, '$1 == "LAT" {sub(/^LAT,/, ""); print}' \
    "$OUT_DIR/ring.out" "$OUT_DIR/kernel-kernel.out" \
    "$OUT_DIR/xlio-kernel.out" "$OUT_DIR/xlio-xlio.out"
} >"$OUT_DIR/latency.csv"

{
  echo 'label,transport,buffers,samples,warmup,protocol,pipeline_depth'
  awk -F, '$1 == "META" {sub(/^META,/, ""); print}' \
    "$OUT_DIR/ring.out" "$OUT_DIR/kernel-kernel.out" \
    "$OUT_DIR/xlio-kernel.out" "$OUT_DIR/xlio-xlio.out"
} >"$OUT_DIR/config.csv"

log "complete: $OUT_DIR"
printf '%s\n' "$OUT_DIR"
