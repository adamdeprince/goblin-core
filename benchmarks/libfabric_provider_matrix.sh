#!/usr/bin/env bash
# Matched Goblin latency across libfabric FI_EP_RDM providers, wire protocols,
# and transmit paths. The controller needs passwordless SSH to SERVER_HOST and
# CLIENT_HOST. Both hosts must see REMOTE_ROOT.
#
# Usage:
#   bash benchmarks/libfabric_provider_matrix.sh
#   SAMPLES=1000 WARMUP=100 SKIP_BUILD=1 \
#     bash benchmarks/libfabric_provider_matrix.sh
#   PROVIDERS=efa SKIP_KERNEL_TCP=1 \
#     bash benchmarks/libfabric_provider_matrix.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SERVER_HOST="${SERVER_HOST:-butterfly}"
CLIENT_HOST="${CLIENT_HOST:-rain}"
SERVER_ADDRESS="${SERVER_ADDRESS:-10.100.0.1}"
CLIENT_ADDRESS="${CLIENT_ADDRESS:-10.100.0.2}"
NIC="${NIC:-enp68s0np0}"
SERVER_CPU="${SERVER_CPU:-5}"
CLIENT_CPU="${CLIENT_CPU:-5}"
NUMA_NODE="${NUMA_NODE:-1}"
FABRIC_PORT="${FABRIC_PORT:-17401}"
KERNEL_PORT="${KERNEL_PORT:-17397}"
LOOPBACK_PORT="${LOOPBACK_PORT:-17699}"
SAMPLES="${SAMPLES:-200000}"
WARMUP="${WARMUP:-20000}"
SKIP_BUILD="${SKIP_BUILD:-0}"
SKIP_KERNEL_TCP="${SKIP_KERNEL_TCP:-0}"

REMOTE_ROOT="${REMOTE_ROOT:-/home/adam/dev/packrat-libfabric-codex}"
BUILD_DIR="${BUILD_DIR:-$REMOTE_ROOT/build-libfabric}"
GOBLIN="${GOBLIN:-$BUILD_DIR/goblin-core}"
PROBE="${PROBE:-$BUILD_DIR/goblin_core_libfabric_latency_benchmark}"
LIBFABRIC_PREFIX="${LIBFABRIC_PREFIX:-/home/adam/opt/libfabric-2.4.0amzn5.0-x86_64}"
CMAKE="${CMAKE:-cmake}"

IFS=',' read -r -a PROVIDER_LIST <<<"${PROVIDERS:-tcp,verbs;ofi_rxm}"
IFS=',' read -r -a SEND_MODE_LIST <<<"${SEND_MODES:-auto,send}"
IFS=',' read -r -a WIRE_LIST <<<"${WIRES:-resp,sbe}"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${OUT_DIR:-/tmp/goblin-libfabric-matrix-$STAMP}"
mkdir -p "$OUT_DIR"

SSH=(ssh -o BatchMode=yes)
SCP=(scp -q)
PID_FILE="/tmp/goblin-libfabric-matrix-${USER}-$$.pid"
REMOTE_LOG="/tmp/goblin-libfabric-matrix-${USER}-$$.log"
ACTIVE_SERVER=0

log() { printf '[libfabric-matrix] %s\n' "$*" >&2; }
remote() { "${SSH[@]}" "$1" "$2"; }

slug() {
  printf '%s' "$1" | tr ';/' '--'
}

verify_numa_endpoint() {
  local host=$1
  local cpu=$2
  if ! remote "$host" \
    "nic_node=\$(cat '/sys/class/net/$NIC/device/numa_node'); node_count=\$(find /sys/devices/system/node -maxdepth 1 -type d -name 'node[0-9]*' | wc -l); { test \"\$nic_node\" = '$NUMA_NODE' || { test \"\$nic_node\" = -1 && test '$NUMA_NODE' = 0 && test \"\$node_count\" = 1; }; } && test -e '/sys/devices/system/cpu/cpu$cpu/node$NUMA_NODE' && command -v numactl >/dev/null"; then
    log "$host: $NIC and CPU $cpu must both belong to NUMA node $NUMA_NODE"
    remote "$host" \
      "printf 'nic node: '; cat '/sys/class/net/$NIC/device/numa_node'; printf 'system nodes: '; find /sys/devices/system/node -maxdepth 1 -type d -name 'node[0-9]*' | wc -l; lscpu -e=CPU,NODE,SOCKET,CORE | awk 'NR == 1 || \$1 == $cpu'" \
      >&2 || true
    exit 1
  fi
}

stop_server() {
  remote "$SERVER_HOST" \
    "if test -s '$PID_FILE'; then p=\$(cat '$PID_FILE'); kill -TERM \"\$p\" 2>/dev/null || true; for i in \$(seq 1 100); do kill -0 \"\$p\" 2>/dev/null || break; sleep 0.02; done; kill -KILL \"\$p\" 2>/dev/null || true; fi; rm -f '$PID_FILE'" \
    >/dev/null 2>&1 || true
  ACTIVE_SERVER=0
}

cleanup() {
  if [[ "$ACTIVE_SERVER" == "1" ]]; then
    stop_server
  fi
}
trap cleanup EXIT INT TERM

start_fabric_server() {
  local provider=$1
  local send_mode=$2
  local force_send=""
  if [[ "$send_mode" == "send" ]]; then
    force_send="--libfabric-force-send"
  fi
  stop_server
  remote "$SERVER_HOST" \
    "nohup env LD_LIBRARY_PATH='$LIBFABRIC_PREFIX/lib' numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$SERVER_CPU' '$GOBLIN' --cpu '$SERVER_CPU' --numa '$NUMA_NODE' --port '$LOOPBACK_PORT' --enable-sbe --no-auth-libfabric --efa-heartbeat-timeout-ms 0 --libfabric '$provider' '$SERVER_ADDRESS' '$FABRIC_PORT' $force_send >'$REMOTE_LOG' 2>&1 </dev/null & echo \$! >'$PID_FILE'"
  ACTIVE_SERVER=1
  for _ in $(seq 1 200); do
    if remote "$SERVER_HOST" \
      "grep -q 'ready (FI_EP_RDM' '$REMOTE_LOG' 2>/dev/null"; then
      return 0
    fi
    sleep 0.05
  done
  remote "$SERVER_HOST" "cat '$REMOTE_LOG'" >&2 || true
  return 1
}

start_kernel_server() {
  stop_server
  remote "$SERVER_HOST" \
    "nohup numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$SERVER_CPU' '$GOBLIN' --cpu '$SERVER_CPU' --numa '$NUMA_NODE' --port '$LOOPBACK_PORT' --enable-sbe --trusted-listen '$SERVER_ADDRESS:$KERNEL_PORT' >'$REMOTE_LOG' 2>&1 </dev/null & echo \$! >'$PID_FILE'"
  ACTIVE_SERVER=1
  for _ in $(seq 1 200); do
    if remote "$CLIENT_HOST" \
      "printf '*1\\r\\n\$4\\r\\nPING\\r\\n' | nc -w 1 '$SERVER_ADDRESS' '$KERNEL_PORT' 2>/dev/null | grep -q PONG"; then
      return 0
    fi
    sleep 0.05
  done
  remote "$SERVER_HOST" "cat '$REMOTE_LOG'" >&2 || true
  return 1
}

run_probe() {
  local name=$1
  local command=$2
  log "$name"
  remote "$CLIENT_HOST" "$command" \
    > >(tee "$OUT_DIR/$name.out") \
    2> >(tee "$OUT_DIR/$name.err" >&2)
}

copy_server_log() {
  local name=$1
  "${SCP[@]}" "$SERVER_HOST:$REMOTE_LOG" "$OUT_DIR/$name.server.log"
}

if [[ "$SKIP_BUILD" != "1" ]]; then
  log "build server and benchmark on $SERVER_HOST"
  remote "$SERVER_HOST" \
    "'$CMAKE' --build '$BUILD_DIR' --target goblin_core_server goblin_core_libfabric_latency_benchmark -j 8"
fi

for binary in "$GOBLIN" "$PROBE"; do
  remote "$SERVER_HOST" "test -x '$binary'" || {
    log "missing remote executable: $binary"
    exit 1
  }
done

verify_numa_endpoint "$SERVER_HOST" "$SERVER_CPU"
verify_numa_endpoint "$CLIENT_HOST" "$CLIENT_CPU"

{
  printf 'timestamp_utc=%s\n' "$STAMP"
  printf 'git_commit=%s\n' \
    "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
  printf 'server_host=%s\nclient_host=%s\n' "$SERVER_HOST" "$CLIENT_HOST"
  printf 'server_address=%s\nclient_address=%s\ninterface=%s\n' \
    "$SERVER_ADDRESS" "$CLIENT_ADDRESS" "$NIC"
  printf 'server_cpu=%s\nclient_cpu=%s\nnuma_node=%s\n' \
    "$SERVER_CPU" "$CLIENT_CPU" "$NUMA_NODE"
  printf 'providers=%s\nsend_modes=%s\nwires=%s\n' \
    "${PROVIDERS:-tcp,verbs;ofi_rxm}" "${SEND_MODES:-auto,send}" \
    "${WIRES:-resp,sbe}"
  printf 'endpoint=FI_EP_RDM\ncaps=FI_MSG+FI_SOURCE\n'
  printf 'rx_api=fi_recv\ncompletion_api=fi_cq_read+fi_cq_readfrom\n'
  printf 'samples=%s\nwarmup=%s\n' "$SAMPLES" "$WARMUP"
  printf 'libfabric='
  remote "$SERVER_HOST" \
    "'$LIBFABRIC_PREFIX/bin/fi_info' --version | head -1"
  printf 'goblin_sha256='
  remote "$SERVER_HOST" "sha256sum '$GOBLIN' | awk '{print \$1}'"
  printf 'probe_sha256='
  remote "$SERVER_HOST" "sha256sum '$PROBE' | awk '{print \$1}'"
  for host in "$SERVER_HOST" "$CLIENT_HOST"; do
    printf '\n[%s]\n' "$host"
    remote "$host" \
      "hostname; uname -sr; lscpu | grep -E 'Model name|Socket.s.|NUMA node.s.'; ip -br addr show '$NIC'; printf 'nic_numa='; cat '/sys/class/net/$NIC/device/numa_node'; ethtool '$NIC' 2>/dev/null | grep -E 'Speed:|Duplex:|Port:|Link detected:'; ethtool -i '$NIC' 2>/dev/null | grep -E 'driver:|firmware-version:|bus-info:'"
  done
} >"$OUT_DIR/metadata.txt"

for provider in "${PROVIDER_LIST[@]}"; do
  provider_slug="$(slug "$provider")"
  for send_mode in "${SEND_MODE_LIST[@]}"; do
    if [[ "$send_mode" != "auto" && "$send_mode" != "send" ]]; then
      log "unsupported send mode: $send_mode"
      exit 2
    fi
    start_fabric_server "$provider" "$send_mode"
    server_name="$provider_slug-$send_mode"
    copy_server_log "$server_name"
    for wire in "${WIRE_LIST[@]}"; do
      if [[ "$wire" != "resp" && "$wire" != "sbe" ]]; then
        log "unsupported wire protocol: $wire"
        exit 2
      fi
      label="$provider_slug-$wire-$send_mode"
      run_probe "$label" \
        "env LD_LIBRARY_PATH='$LIBFABRIC_PREFIX/lib' numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$CLIENT_CPU' '$PROBE' 'fabric-$wire' '$provider' '$SERVER_ADDRESS' '$FABRIC_PORT' '$CLIENT_ADDRESS' '$label' '$SAMPLES' '$WARMUP' '$send_mode'"
    done
    stop_server
  done
done

if [[ "$SKIP_KERNEL_TCP" != "1" ]]; then
  start_kernel_server
  copy_server_log kernel-tcp
  run_probe kernel-tcp-resp \
    "numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$CLIENT_CPU' '$PROBE' tcp-resp '$SERVER_ADDRESS' '$KERNEL_PORT' '$CLIENT_ADDRESS' kernel-tcp-resp '$SAMPLES' '$WARMUP'"
  run_probe kernel-tcp-sbe \
    "numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$CLIENT_CPU' '$PROBE' tcp-sbe '$SERVER_ADDRESS' '$KERNEL_PORT' kernel-tcp-sbe '$SAMPLES' '$WARMUP'"
  stop_server
fi

{
  echo 'label,operation,min_us,p50_us,p75_us,p90_us,p95_us,p99_us,p99_9_us,p99_99_us,max_us,mean_us,qps,samples'
  awk -F, '$1 == "LAT" {sub(/^LAT,/, ""); print}' "$OUT_DIR"/*.out
} >"$OUT_DIR/latency.csv"

{
  echo 'label,transport,protocol,pipeline,endpoint,buffers,samples,warmup'
  awk -F, '$1 == "META" {sub(/^META,/, ""); print}' "$OUT_DIR"/*.out
} >"$OUT_DIR/config.csv"

log "complete: $OUT_DIR"
printf '%s\n' "$OUT_DIR"
