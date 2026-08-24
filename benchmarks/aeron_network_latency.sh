#!/usr/bin/env bash
# Depth-one RESP/SBE Aeron UDP latency between butterfly (server) and rain
# (client). The lab defaults compare kernel UDP over 10 GbE with XLIO's
# userspace UDP path over the direct 100 GbE link.
#
# rain and butterfly share /home over NFS, so build Goblin and Aeron once and
# point SHARED_PREFIX at that read-only artifact. Aeron directories, process
# logs, and XLIO statistics stay on each host's local /tmp filesystem.
#
# The controller needs passwordless SSH and passwordless sudo on both hosts.
# sudo is used only for the XLIO path, which needs CAP_NET_RAW or root.
#
# Usage:
#   bash benchmarks/aeron_network_latency.sh
#   RUNS=1 SAMPLES=10000 WARMUP=1000 \
#     OUT_DIR=/tmp/aeron-network-smoke \
#     bash benchmarks/aeron_network_latency.sh
#
# To qualify XLIO rather than collect publication-quality latency, set
# XLIO_STATS_DURING_RUN=1. This attaches xlio_stats while the probe is live and
# requires nonzero offloaded UDP traffic on both Media Drivers. Keep it off for
# reported latency because the concurrent statistics process is a perturbation.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

SERVER_HOST="${SERVER_HOST:-butterfly}"
CLIENT_HOST="${CLIENT_HOST:-rain}"
SHARED_PREFIX="${SHARED_PREFIX:-/home/adam/opt/goblin-aeron-benchmark-9ddb3ed-9773cba}"
GOBLIN="${GOBLIN:-$SHARED_PREFIX/bin/goblin-core}"
PROBE="${PROBE:-$SHARED_PREFIX/bin/goblin_core_local_transport_latency_benchmark}"
AERONMD="${AERONMD:-$SHARED_PREFIX/aeron/bin/aeronmd_s}"
XLIO_PREFIX="${XLIO_PREFIX:-/home/adam/opt/goblin-xlio-3.61.2/prefix}"
XLIO_LIBRARY="${XLIO_LIBRARY:-$XLIO_PREFIX/lib/libxlio.so}"
XLIO_STATS="${XLIO_STATS:-$XLIO_PREFIX/bin/xlio_stats}"

TEN_SERVER_ADDRESS="${TEN_SERVER_ADDRESS:-192.168.1.46}"
TEN_CLIENT_ADDRESS="${TEN_CLIENT_ADDRESS:-192.168.1.134}"
TEN_NIC="${TEN_NIC:-eno1}"
TEN_NUMA_NODE="${TEN_NUMA_NODE:-0}"
TEN_SERVER_CPU="${TEN_SERVER_CPU:-0}"
TEN_CLIENT_CPU="${TEN_CLIENT_CPU:-0}"
TEN_DRIVER_CONDUCTOR_CPU="${TEN_DRIVER_CONDUCTOR_CPU:-4}"
TEN_DRIVER_RECEIVER_CPU="${TEN_DRIVER_RECEIVER_CPU:-8}"
TEN_DRIVER_SENDER_CPU="${TEN_DRIVER_SENDER_CPU:-12}"
TEN_AERON_CLIENT_CPU="${TEN_AERON_CLIENT_CPU:-16}"

HUNDRED_SERVER_ADDRESS="${HUNDRED_SERVER_ADDRESS:-10.100.0.1}"
HUNDRED_CLIENT_ADDRESS="${HUNDRED_CLIENT_ADDRESS:-10.100.0.2}"
HUNDRED_NIC="${HUNDRED_NIC:-enp68s0np0}"
HUNDRED_NUMA_NODE="${HUNDRED_NUMA_NODE:-1}"
HUNDRED_SERVER_CPU="${HUNDRED_SERVER_CPU:-5}"
HUNDRED_CLIENT_CPU="${HUNDRED_CLIENT_CPU:-5}"
HUNDRED_DRIVER_CONDUCTOR_CPU="${HUNDRED_DRIVER_CONDUCTOR_CPU:-9}"
HUNDRED_DRIVER_RECEIVER_CPU="${HUNDRED_DRIVER_RECEIVER_CPU:-13}"
HUNDRED_DRIVER_SENDER_CPU="${HUNDRED_DRIVER_SENDER_CPU:-17}"
HUNDRED_AERON_CLIENT_CPU="${HUNDRED_AERON_CLIENT_CPU:-21}"
HUNDRED_XLIO_INTERNAL_CPU="${HUNDRED_XLIO_INTERNAL_CPU:-25}"

RUNS="${RUNS:-2}"
SAMPLES="${SAMPLES:-100000}"
WARMUP="${WARMUP:-10000}"
PATHS="${PATHS:-10gbe 100gbe}"
PROTOCOLS="${PROTOCOLS:-RESP SBE}"
XLIO_STATS_DURING_RUN="${XLIO_STATS_DURING_RUN:-0}"

AERON_CONDUCTOR_IDLE_STRATEGY="${AERON_CONDUCTOR_IDLE_STRATEGY:-spin}"
AERON_SENDER_IDLE_STRATEGY="${AERON_SENDER_IDLE_STRATEGY:-noop}"
AERON_RECEIVER_IDLE_STRATEGY="${AERON_RECEIVER_IDLE_STRATEGY:-noop}"
AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND="${AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND:-2}"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark-results/aeron-network-$STAMP}"
TOKEN="${TOKEN:-$STAMP-$$}"
SERVER_RUNTIME="/tmp/goblin-aeron-network-${USER:-benchmark}-$TOKEN-server"
CLIENT_RUNTIME="/tmp/goblin-aeron-network-${USER:-benchmark}-$TOKEN-client"
LOOPBACK_PORT="${LOOPBACK_PORT:-17999}"

SSH=(ssh -S none -o BatchMode=yes)

SERVER_PID=""
SERVER_DRIVER_PID=""
CLIENT_DRIVER_PID=""
SERVER_WRAPPER_PID=""
SERVER_DRIVER_WRAPPER_PID=""
CLIENT_DRIVER_WRAPPER_PID=""
PROBE_SSH_PID=""
CASE_ROOT_MODE=0

log() { printf '[aeron-network] %s\n' "$*" >&2; }
remote() { "${SSH[@]}" "$1" "$2"; }

stop_remote_process() {
  local host=$1 pid=$2 root_mode=$3 prefix=""
  [[ -n "$pid" ]] || return 0
  if [[ "$root_mode" == 1 ]]; then prefix="sudo -n "; fi
  remote "$host" \
    "if ${prefix}kill -0 '$pid' 2>/dev/null; then ${prefix}kill -TERM '$pid' 2>/dev/null || true; for _ in \$(seq 1 100); do ${prefix}kill -0 '$pid' 2>/dev/null || exit 0; sleep 0.02; done; ${prefix}kill -KILL '$pid' 2>/dev/null || true; fi" \
    >/dev/null 2>&1 || true
}

stop_case() {
  if [[ -n "$PROBE_SSH_PID" ]]; then
    kill "$PROBE_SSH_PID" 2>/dev/null || true
    wait "$PROBE_SSH_PID" 2>/dev/null || true
    PROBE_SSH_PID=""
  fi
  stop_remote_process "$SERVER_HOST" "$SERVER_PID" "$CASE_ROOT_MODE"
  stop_remote_process "$SERVER_HOST" "$SERVER_WRAPPER_PID" "$CASE_ROOT_MODE"
  stop_remote_process "$CLIENT_HOST" "$CLIENT_DRIVER_PID" "$CASE_ROOT_MODE"
  stop_remote_process "$CLIENT_HOST" "$CLIENT_DRIVER_WRAPPER_PID" "$CASE_ROOT_MODE"
  stop_remote_process "$SERVER_HOST" "$SERVER_DRIVER_PID" "$CASE_ROOT_MODE"
  stop_remote_process "$SERVER_HOST" "$SERVER_DRIVER_WRAPPER_PID" "$CASE_ROOT_MODE"
  SERVER_PID=""
  SERVER_DRIVER_PID=""
  CLIENT_DRIVER_PID=""
  SERVER_WRAPPER_PID=""
  SERVER_DRIVER_WRAPPER_PID=""
  CLIENT_DRIVER_WRAPPER_PID=""
  CASE_ROOT_MODE=0
}

cleanup() { stop_case; }
trap cleanup EXIT INT TERM

select_path() {
  local path=$1
  case "$path" in
    10gbe)
      SERVER_ADDRESS="$TEN_SERVER_ADDRESS"
      CLIENT_ADDRESS="$TEN_CLIENT_ADDRESS"
      NIC="$TEN_NIC"
      NUMA_NODE="$TEN_NUMA_NODE"
      SERVER_CPU="$TEN_SERVER_CPU"
      CLIENT_CPU="$TEN_CLIENT_CPU"
      DRIVER_CONDUCTOR_CPU="$TEN_DRIVER_CONDUCTOR_CPU"
      DRIVER_RECEIVER_CPU="$TEN_DRIVER_RECEIVER_CPU"
      DRIVER_SENDER_CPU="$TEN_DRIVER_SENDER_CPU"
      AERON_CLIENT_CPU="$TEN_AERON_CLIENT_CPU"
      XLIO_INTERNAL_CPU=""
      EXPECTED_SPEED=10000
      NETWORK_STACK=kernel
      USERSPACE=0
      PORT_BASE=40120
      ;;
    100gbe)
      SERVER_ADDRESS="$HUNDRED_SERVER_ADDRESS"
      CLIENT_ADDRESS="$HUNDRED_CLIENT_ADDRESS"
      NIC="$HUNDRED_NIC"
      NUMA_NODE="$HUNDRED_NUMA_NODE"
      SERVER_CPU="$HUNDRED_SERVER_CPU"
      CLIENT_CPU="$HUNDRED_CLIENT_CPU"
      DRIVER_CONDUCTOR_CPU="$HUNDRED_DRIVER_CONDUCTOR_CPU"
      DRIVER_RECEIVER_CPU="$HUNDRED_DRIVER_RECEIVER_CPU"
      DRIVER_SENDER_CPU="$HUNDRED_DRIVER_SENDER_CPU"
      AERON_CLIENT_CPU="$HUNDRED_AERON_CLIENT_CPU"
      XLIO_INTERNAL_CPU="$HUNDRED_XLIO_INTERNAL_CPU"
      EXPECTED_SPEED=100000
      NETWORK_STACK=xlio-userspace
      USERSPACE=1
      PORT_BASE=41120
      ;;
    *)
      log "unknown network path: $path"
      return 2
      ;;
  esac
}

verify_cpu() {
  local host=$1 cpu=$2 node=$3
  remote "$host" "test -e '/sys/devices/system/cpu/cpu$cpu/node$node'" || {
    log "$host: CPU $cpu is not on NUMA node $node"
    return 1
  }
}

verify_path() {
  local path=$1 host cpu
  select_path "$path"
  for host in "$SERVER_HOST" "$CLIENT_HOST"; do
    remote "$host" \
      "test \"\$(cat '/sys/class/net/$NIC/device/numa_node')\" = '$NUMA_NODE' && test \"\$(cat '/sys/class/net/$NIC/speed')\" = '$EXPECTED_SPEED' && ip -o -4 address show dev '$NIC' | grep -q '$([[ "$host" == "$SERVER_HOST" ]] && printf %s "$SERVER_ADDRESS" || printf %s "$CLIENT_ADDRESS")'" || {
      log "$host: $NIC must be up at ${EXPECTED_SPEED} Mb/s on NUMA node $NUMA_NODE with the configured address"
      return 1
    }
  done
  for cpu in "$SERVER_CPU" "$DRIVER_CONDUCTOR_CPU" "$DRIVER_RECEIVER_CPU" \
      "$DRIVER_SENDER_CPU" "$AERON_CLIENT_CPU"; do
    verify_cpu "$SERVER_HOST" "$cpu" "$NUMA_NODE"
  done
  for cpu in "$CLIENT_CPU" "$DRIVER_CONDUCTOR_CPU" "$DRIVER_RECEIVER_CPU" \
      "$DRIVER_SENDER_CPU" "$AERON_CLIENT_CPU"; do
    verify_cpu "$CLIENT_HOST" "$cpu" "$NUMA_NODE"
  done
  if [[ "$USERSPACE" == 1 ]]; then
    verify_cpu "$SERVER_HOST" "$XLIO_INTERNAL_CPU" "$NUMA_NODE"
    verify_cpu "$CLIENT_HOST" "$XLIO_INTERNAL_CPU" "$NUMA_NODE"
    remote "$SERVER_HOST" "sudo -n true && test -r '$XLIO_LIBRARY' && test -x '$XLIO_STATS'"
    remote "$CLIENT_HOST" "sudo -n true && test -r '$XLIO_LIBRARY' && test -x '$XLIO_STATS'"
  fi
  remote "$CLIENT_HOST" \
    "ip route get '$SERVER_ADDRESS' from '$CLIENT_ADDRESS' | grep -q 'dev $NIC'"
  remote "$SERVER_HOST" \
    "ip route get '$CLIENT_ADDRESS' from '$SERVER_ADDRESS' | grep -q 'dev $NIC'"
}

path_metadata() {
  local path=$1 host address cpus
  select_path "$path"
  cpus="$SERVER_CPU,$DRIVER_CONDUCTOR_CPU,$DRIVER_RECEIVER_CPU,$DRIVER_SENDER_CPU,$AERON_CLIENT_CPU"
  if [[ "$USERSPACE" == 1 ]]; then cpus="$cpus,$XLIO_INTERNAL_CPU"; fi
  printf '\n[path=%s]\n' "$path"
  printf 'network_stack=%s\ninterface=%s\nnuma_node=%s\n' \
    "$NETWORK_STACK" "$NIC" "$NUMA_NODE"
  printf 'server_address=%s\nclient_address=%s\n' \
    "$SERVER_ADDRESS" "$CLIENT_ADDRESS"
  printf 'server_cpu=%s\nclient_cpu=%s\n' "$SERVER_CPU" "$CLIENT_CPU"
  printf 'driver_cpus=%s,%s,%s\naeron_client_cpu=%s\n' \
    "$DRIVER_CONDUCTOR_CPU" "$DRIVER_RECEIVER_CPU" \
    "$DRIVER_SENDER_CPU" "$AERON_CLIENT_CPU"
  if [[ "$USERSPACE" == 1 ]]; then
    printf 'xlio_internal_cpu=%s\n' "$XLIO_INTERNAL_CPU"
  fi
  for host in "$SERVER_HOST" "$CLIENT_HOST"; do
    if [[ "$host" == "$SERVER_HOST" ]]; then address="$SERVER_ADDRESS"; else address="$CLIENT_ADDRESS"; fi
    printf '\n[%s/%s]\n' "$path" "$host"
    remote "$host" \
      "hostname; uname -srvo; printf 'system='; cat /sys/class/dmi/id/product_name 2>/dev/null || true; lscpu | grep -E 'Model name:|Socket.s.:|Core.s. per socket:|Thread.s. per core:|NUMA node.s.:'; findmnt -T /home/adam -o TARGET,SOURCE,FSTYPE,OPTIONS -n; ip -br address show '$NIC'; ip route get '$([[ "$host" == "$SERVER_HOST" ]] && printf %s "$CLIENT_ADDRESS" || printf %s "$SERVER_ADDRESS")' from '$address'; printf 'nic_numa='; cat '/sys/class/net/$NIC/device/numa_node'; ethtool '$NIC' 2>/dev/null | grep -E 'Speed:|Duplex:|Port:|Link detected:'; ethtool -i '$NIC' 2>/dev/null | grep -E 'driver:|version:|firmware-version:|bus-info:'; ethtool -c '$NIC' 2>/dev/null | grep -E 'Adaptive RX:|rx-usecs:|rx-frames:|tx-usecs:|tx-frames:'; printf 'governor='; cat '/sys/devices/system/cpu/cpu$SERVER_CPU/cpufreq/scaling_governor' 2>/dev/null || true; lscpu -e=CPU,NODE,SOCKET,CORE | awk 'NR == 1 || index(\",$cpus,\", \",\" \$1 \",\")'"
  done
}

resolve_root_child() {
  local host=$1 wrapper_pid=$2
  remote "$host" \
    "for _ in \$(seq 1 100); do child=\$(pgrep -P '$wrapper_pid' | head -n 1 || true); if test -n \"\$child\"; then printf '%s\n' \"\$child\"; exit 0; fi; sudo -n kill -0 '$wrapper_pid' 2>/dev/null || exit 1; sleep 0.02; done; exit 1"
}

start_driver() {
  local role=$1 host=$2 directory=$3 log_file=$4 xlio_log=$5
  local cpu_set command
  cpu_set="$DRIVER_CONDUCTOR_CPU,$DRIVER_RECEIVER_CPU,$DRIVER_SENDER_CPU"
  if [[ "$USERSPACE" == 1 ]]; then
    cpu_set="$cpu_set,$XLIO_INTERNAL_CPU"
    command="nohup sudo -n numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$cpu_set' env XLIO_SPEC=latency XLIO_MEM_ALLOC_TYPE=ANON XLIO_RX_POLL_INIT=-1 XLIO_RX_UDP_POLL_OS_RATIO=0 XLIO_OFFLOADED_SOCKETS=1 XLIO_EXCEPTION_HANDLING=2 XLIO_INTERNAL_THREAD_AFFINITY='$XLIO_INTERNAL_CPU' XLIO_PROGRESS_ENGINE_INTERVAL=0 XLIO_TRACELEVEL=INFO XLIO_LOG_FILE='$xlio_log' LD_LIBRARY_PATH='$XLIO_PREFIX/lib' LD_PRELOAD='$XLIO_LIBRARY' AERON_CONDUCTOR_IDLE_STRATEGY='$AERON_CONDUCTOR_IDLE_STRATEGY' AERON_SENDER_IDLE_STRATEGY='$AERON_SENDER_IDLE_STRATEGY' AERON_RECEIVER_IDLE_STRATEGY='$AERON_RECEIVER_IDLE_STRATEGY' AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND='$AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND' '$AERONMD' '-Daeron.dir=$directory' -Daeron.dir.delete.on.start=true -Daeron.dir.delete.on.shutdown=true -Daeron.threading.mode=DEDICATED '-Daeron.conductor.cpu.affinity=$DRIVER_CONDUCTOR_CPU' '-Daeron.receiver.cpu.affinity=$DRIVER_RECEIVER_CPU' '-Daeron.sender.cpu.affinity=$DRIVER_SENDER_CPU' >'$log_file' 2>&1 </dev/null & echo \$!"
  else
    command="nohup numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' taskset -c '$cpu_set' env AERON_CONDUCTOR_IDLE_STRATEGY='$AERON_CONDUCTOR_IDLE_STRATEGY' AERON_SENDER_IDLE_STRATEGY='$AERON_SENDER_IDLE_STRATEGY' AERON_RECEIVER_IDLE_STRATEGY='$AERON_RECEIVER_IDLE_STRATEGY' AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND='$AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND' '$AERONMD' '-Daeron.dir=$directory' -Daeron.dir.delete.on.start=true -Daeron.dir.delete.on.shutdown=true -Daeron.threading.mode=DEDICATED '-Daeron.conductor.cpu.affinity=$DRIVER_CONDUCTOR_CPU' '-Daeron.receiver.cpu.affinity=$DRIVER_RECEIVER_CPU' '-Daeron.sender.cpu.affinity=$DRIVER_SENDER_CPU' >'$log_file' 2>&1 </dev/null & echo \$!"
  fi
  local wrapper_pid pid
  wrapper_pid="$(remote "$host" "$command")"
  pid="$wrapper_pid"
  if [[ "$USERSPACE" == 1 ]]; then pid="$(resolve_root_child "$host" "$wrapper_pid")"; fi
  if [[ "$role" == server ]]; then
    SERVER_DRIVER_PID="$pid"
    if [[ "$USERSPACE" == 1 ]]; then SERVER_DRIVER_WRAPPER_PID="$wrapper_pid"; fi
  else
    CLIENT_DRIVER_PID="$pid"
    if [[ "$USERSPACE" == 1 ]]; then CLIENT_DRIVER_WRAPPER_PID="$wrapper_pid"; fi
  fi
  printf '%s\n' "$pid"
}

wait_driver() {
  local host=$1 pid=$2 directory=$3 log_file=$4 prefix=""
  if [[ "$USERSPACE" == 1 ]]; then prefix="sudo -n "; fi
  remote "$host" \
    "for _ in \$(seq 1 200); do if ${prefix}test -f '$directory/cnc.dat'; then exit 0; fi; if ! ${prefix}kill -0 '$pid' 2>/dev/null; then cat '$log_file' >&2; exit 1; fi; sleep 0.05; done; cat '$log_file' >&2; exit 1"
}

start_server() {
  local directory=$1 request_endpoint=$2 request_stream=$3
  local response_endpoint=$4 response_stream=$5 log_file=$6 command
  if [[ "$USERSPACE" == 1 ]]; then
    command="nohup sudo -n numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' '$GOBLIN' --cpu '$SERVER_CPU' --numa '$NUMA_NODE' --numa-arena --enable-sbe --port '$LOOPBACK_PORT' --aeron-dir '$directory' --aeron-udp '$request_endpoint' '$request_stream' '$response_endpoint' '$response_stream' >'$log_file' 2>&1 </dev/null & echo \$!"
  else
    command="nohup numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' '$GOBLIN' --cpu '$SERVER_CPU' --numa '$NUMA_NODE' --numa-arena --enable-sbe --port '$LOOPBACK_PORT' --aeron-dir '$directory' --aeron-udp '$request_endpoint' '$request_stream' '$response_endpoint' '$response_stream' >'$log_file' 2>&1 </dev/null & echo \$!"
  fi
  local wrapper_pid
  wrapper_pid="$(remote "$SERVER_HOST" "$command")"
  SERVER_PID="$wrapper_pid"
  if [[ "$USERSPACE" == 1 ]]; then
    SERVER_WRAPPER_PID="$wrapper_pid"
    SERVER_PID="$(resolve_root_child "$SERVER_HOST" "$wrapper_pid")"
  fi
}

wait_server() {
  local log_file=$1 prefix=""
  if [[ "$USERSPACE" == 1 ]]; then prefix="sudo -n "; fi
  remote "$SERVER_HOST" \
    "for _ in \$(seq 1 240); do grep -q 'Aeron UDP .* ready' '$log_file' 2>/dev/null && exit 0; if ! ${prefix}kill -0 '$SERVER_PID' 2>/dev/null; then cat '$log_file' >&2; exit 1; fi; sleep 0.05; done; cat '$log_file' >&2; exit 1"
}

pin_server_threads() {
  local prefix=""
  if [[ "$USERSPACE" == 1 ]]; then prefix="sudo -n "; fi
  remote "$SERVER_HOST" \
    "for task in /proc/'$SERVER_PID'/task/*; do tid=\${task##*/}; cpu='$AERON_CLIENT_CPU'; if test \"\$tid\" = '$SERVER_PID'; then cpu='$SERVER_CPU'; fi; ${prefix}taskset -pc \"\$cpu\" \"\$tid\" >/dev/null; done"
}

capture_affinity() {
  local host=$1 pid=$2 root_mode=$3 output=$4 prefix=""
  if [[ "$root_mode" == 1 ]]; then prefix="sudo -n "; fi
  remote "$host" \
    "for task in /proc/'$pid'/task/*; do tid=\${task##*/}; printf 'tid=%s comm=' \"\$tid\"; ${prefix}cat \"\$task/comm\"; ${prefix}taskset -pc \"\$tid\"; done" \
    >"$output"
}

capture_nic_counters() {
  local host=$1 output=$2
  remote "$host" \
    "for counter in rx_packets tx_packets rx_bytes tx_bytes rx_dropped tx_dropped; do printf '%s=' \"\$counter\"; cat '/sys/class/net/$NIC/statistics/'\"\$counter\"; done" \
    >"$output"
}

sample_xlio_stats() {
  local case_dir=$1 server_stats_pid client_stats_pid failed=0
  local stats_file
  remote "$SERVER_HOST" \
    "sudo -n env LD_LIBRARY_PATH='$XLIO_PREFIX/lib' '$XLIO_STATS' -p '$SERVER_DRIVER_PID' -i 0 -c 1 -v 3 -d 1" \
    >"$case_dir/server-driver.xlio-stats.txt" 2>&1 &
  server_stats_pid=$!
  remote "$CLIENT_HOST" \
    "sudo -n env LD_LIBRARY_PATH='$XLIO_PREFIX/lib' '$XLIO_STATS' -p '$CLIENT_DRIVER_PID' -i 0 -c 1 -v 3 -d 1" \
    >"$case_dir/client-driver.xlio-stats.txt" 2>&1 &
  client_stats_pid=$!
  wait "$server_stats_pid" || failed=1
  wait "$client_stats_pid" || failed=1
  if [[ "$failed" != 0 ]]; then
    log "XLIO did not report offloaded UDP sockets on both Media Drivers"
    return 1
  fi
  for stats_file in "$case_dir/server-driver.xlio-stats.txt" \
      "$case_dir/client-driver.xlio-stats.txt"; do
    if ! grep -Eqi 'udp|datagram' "$stats_file" || \
        ! grep -Eq 'Tx Offload:.* / [1-9][0-9]* ' "$stats_file" || \
        ! grep -Eq 'Rx Offload:.* / [1-9][0-9]* ' "$stats_file"; then
      log "XLIO did not report both transmitted and received offloaded UDP packets in $stats_file"
      return 1
    fi
  done
}

copy_case_logs() {
  local case_dir=$1 remote_server_dir=$2 remote_client_dir=$3
  remote "$SERVER_HOST" "cat '$remote_server_dir/server.log'" \
    >"$case_dir/server.log" 2>/dev/null || true
  remote "$SERVER_HOST" "cat '$remote_server_dir/driver.log'" \
    >"$case_dir/server-driver.log" 2>/dev/null || true
  remote "$CLIENT_HOST" "cat '$remote_client_dir/driver.log'" \
    >"$case_dir/client-driver.log" 2>/dev/null || true
  if [[ "$USERSPACE" == 1 ]]; then
    remote "$SERVER_HOST" "sudo -n cat '$remote_server_dir/xlio.log'" \
      >"$case_dir/server-driver.xlio.log" 2>/dev/null || true
    remote "$CLIENT_HOST" "sudo -n cat '$remote_client_dir/xlio.log'" \
      >"$case_dir/client-driver.xlio.log" 2>/dev/null || true
  fi
}

run_probe() {
  local protocol=$1 client_directory=$2 request_endpoint=$3 request_stream=$4
  local response_endpoint=$5 response_stream=$6 case_dir=$7 command
  local mode="aeron-udp-$(printf '%s' "$protocol" | tr '[:upper:]' '[:lower:]')"
  if [[ "$USERSPACE" == 1 ]]; then
    command="sudo -n env SAMPLES='$SAMPLES' WARMUP='$WARMUP' CLIENT_CPU='$CLIENT_CPU' AERON_CLIENT_CPU='$AERON_CLIENT_CPU' numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' '$PROBE' '$mode' '$client_directory' '$request_endpoint' '$request_stream' '$response_endpoint' '$response_stream'"
  else
    command="env SAMPLES='$SAMPLES' WARMUP='$WARMUP' CLIENT_CPU='$CLIENT_CPU' AERON_CLIENT_CPU='$AERON_CLIENT_CPU' numactl --cpunodebind='$NUMA_NODE' --membind='$NUMA_NODE' '$PROBE' '$mode' '$client_directory' '$request_endpoint' '$request_stream' '$response_endpoint' '$response_stream'"
  fi
  remote "$CLIENT_HOST" "$command" \
    >"$case_dir/latency.csv" 2>"$case_dir/probe.log" &
  PROBE_SSH_PID=$!
  if [[ "$USERSPACE" == 1 && "$XLIO_STATS_DURING_RUN" == 1 ]]; then
    sleep 0.75
    sample_xlio_stats "$case_dir"
  fi
  if ! wait "$PROBE_SSH_PID"; then
    PROBE_SSH_PID=""
    cat "$case_dir/probe.log" >&2 || true
    return 1
  fi
  PROBE_SSH_PID=""
}

validate_case() {
  local case_dir=$1 rows
  rows="$(awk -F, -v samples="$SAMPLES" \
    '$1 == "LAT" && $11 == samples { count++ } END { print count + 0 }' \
    "$case_dir/latency.csv")"
  if [[ "$rows" != 3 ]]; then
    log "expected three complete LAT rows in $case_dir/latency.csv, found $rows"
    return 1
  fi
  if grep -Eqi 'error|failed|exception|warning' \
      "$case_dir/server.log" "$case_dir/server-driver.log" \
      "$case_dir/client-driver.log" "$case_dir/probe.log"; then
    log "an error or warning was recorded in $case_dir"
    grep -Ein 'error|failed|exception|warning' \
      "$case_dir/server.log" "$case_dir/server-driver.log" \
      "$case_dir/client-driver.log" "$case_dir/probe.log" >&2 || true
    return 1
  fi
}

run_case() {
  local run=$1 path=$2 protocol=$3 protocol_lower protocol_index
  local request_port response_port request_stream response_stream
  local server_request_channel client_request_channel
  local server_response_channel client_response_channel case_name case_dir
  local remote_server_dir remote_client_dir
  select_path "$path"
  CASE_ROOT_MODE="$USERSPACE"
  protocol_lower="$(printf '%s' "$protocol" | tr '[:upper:]' '[:lower:]')"
  if [[ "$protocol" == RESP ]]; then protocol_index=0; else protocol_index=1; fi
  request_port=$((PORT_BASE + run * 4 + protocol_index * 2))
  response_port=$((request_port + 1))
  request_stream=$((2000 + run * 10 + protocol_index * 2 + 1))
  response_stream=$((request_stream + 1))
  # The response publication binds this control address on the server. The
  # client subscription uses the same URI as its remote control target; Aeron's
  # response correlation supplies the return destination for response data.
  server_request_channel="aeron:udp?endpoint=$SERVER_ADDRESS:$request_port"
  client_request_channel="$server_request_channel"
  server_response_channel="aeron:udp?control=$SERVER_ADDRESS:$response_port"
  client_response_channel="$server_response_channel"
  case_name="$path-$protocol_lower"
  case_dir="$OUT_DIR/run-$run/$case_name"
  remote_server_dir="$SERVER_RUNTIME/run-$run/$case_name"
  remote_client_dir="$CLIENT_RUNTIME/run-$run/$case_name"
  mkdir -p "$case_dir"
  remote "$SERVER_HOST" "mkdir -p '$remote_server_dir'"
  remote "$CLIENT_HOST" "mkdir -p '$remote_client_dir'"
  {
    printf 'server_request_channel=%s\n' "$server_request_channel"
    printf 'client_request_channel=%s\n' "$client_request_channel"
    printf 'server_response_channel=%s\n' "$server_response_channel"
    printf 'client_response_channel=%s\n' "$client_response_channel"
    printf 'request_stream=%s\nresponse_stream=%s\n' \
      "$request_stream" "$response_stream"
  } >"$case_dir/channels.txt"

  stop_case
  CASE_ROOT_MODE="$USERSPACE"
  log "run $run/$RUNS: $protocol over $path ($NETWORK_STACK)"
  start_driver server "$SERVER_HOST" "$remote_server_dir/aeron" \
    "$remote_server_dir/driver.log" "$remote_server_dir/xlio.log" >/dev/null
  wait_driver "$SERVER_HOST" "$SERVER_DRIVER_PID" \
    "$remote_server_dir/aeron" "$remote_server_dir/driver.log"
  start_driver client "$CLIENT_HOST" "$remote_client_dir/aeron" \
    "$remote_client_dir/driver.log" "$remote_client_dir/xlio.log" >/dev/null
  wait_driver "$CLIENT_HOST" "$CLIENT_DRIVER_PID" \
    "$remote_client_dir/aeron" "$remote_client_dir/driver.log"
  start_server "$remote_server_dir/aeron" "$server_request_channel" \
    "$request_stream" "$server_response_channel" "$response_stream" \
    "$remote_server_dir/server.log"
  wait_server "$remote_server_dir/server.log"
  pin_server_threads
  capture_affinity "$SERVER_HOST" "$SERVER_PID" "$USERSPACE" \
    "$case_dir/server-affinity.txt"
  capture_affinity "$SERVER_HOST" "$SERVER_DRIVER_PID" "$USERSPACE" \
    "$case_dir/server-driver-affinity.txt"
  capture_affinity "$CLIENT_HOST" "$CLIENT_DRIVER_PID" "$USERSPACE" \
    "$case_dir/client-driver-affinity.txt"
  capture_nic_counters "$SERVER_HOST" "$case_dir/server-nic-before.txt"
  capture_nic_counters "$CLIENT_HOST" "$case_dir/client-nic-before.txt"
  run_probe "$protocol" "$remote_client_dir/aeron" "$client_request_channel" \
    "$request_stream" "$client_response_channel" "$response_stream" "$case_dir"
  capture_nic_counters "$SERVER_HOST" "$case_dir/server-nic-after.txt"
  capture_nic_counters "$CLIENT_HOST" "$case_dir/client-nic-after.txt"
  copy_case_logs "$case_dir" "$remote_server_dir" "$remote_client_dir"
  validate_case "$case_dir"
  awk -F, -v run="$run" -v path="$path" -v stack="$NETWORK_STACK" \
    '$1 == "LAT" { print run "," path "," stack "," $0 }' \
    "$case_dir/latency.csv" >>"$OUT_DIR/latency.csv"
  stop_case
}

mkdir -p "$OUT_DIR"
printf '%s\n' \
  'run,path,network_stack,kind,transport,protocol,operation,min_us,p50_us,p90_us,p99_us,p99_9_us,mean_us,samples' \
  >"$OUT_DIR/latency.csv"

for host in "$SERVER_HOST" "$CLIENT_HOST"; do
  for binary in "$GOBLIN" "$PROBE" "$AERONMD"; do
    remote "$host" "test -x '$binary'" || {
      log "$host cannot execute shared artifact: $binary"
      exit 1
    }
  done
done

for path in $PATHS; do verify_path "$path"; done

{
  printf 'timestamp_utc=%s\n' "$STAMP"
  printf 'controller_git_commit=%s\n' \
    "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
  printf 'controller_git_status_short=%s\n' \
    "$(git -C "$ROOT" status --short 2>/dev/null | tr '\n' ';')"
  printf 'server_host=%s\nclient_host=%s\n' "$SERVER_HOST" "$CLIENT_HOST"
  printf 'shared_prefix=%s\nserver_runtime=%s\nclient_runtime=%s\n' \
    "$SHARED_PREFIX" "$SERVER_RUNTIME" "$CLIENT_RUNTIME"
  printf 'runs=%s\nsamples=%s\nwarmup=%s\nprotocols=%s\npaths=%s\n' \
    "$RUNS" "$SAMPLES" "$WARMUP" "$PROTOCOLS" "$PATHS"
  printf 'goblin_cpp_client_conductor_idle_strategy=spin\n'
  printf 'aeron_driver_conductor_idle_strategy=%s\n' \
    "$AERON_CONDUCTOR_IDLE_STRATEGY"
  printf 'aeron_driver_sender_idle_strategy=%s\n' \
    "$AERON_SENDER_IDLE_STRATEGY"
  printf 'aeron_driver_receiver_idle_strategy=%s\n' \
    "$AERON_RECEIVER_IDLE_STRATEGY"
  printf 'aeron_network_publication_max_messages_per_send=%s\n' \
    "$AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND"
  printf 'xlio_spec=latency\nxlio_mem_alloc_type=ANON\n'
  printf 'xlio_rx_poll_init=-1\nxlio_rx_udp_poll_os_ratio=0\n'
  printf 'xlio_progress_engine_interval=0\n'
  printf 'xlio_stats_during_run=%s\n' "$XLIO_STATS_DURING_RUN"
  remote "$SERVER_HOST" "cat '$SHARED_PREFIX/BUILD-METADATA.txt'; sha256sum '$GOBLIN' '$PROBE' '$AERONMD' '$XLIO_LIBRARY'; '$XLIO_STATS' --version 2>&1 || true"
  for path in $PATHS; do path_metadata "$path"; done
} >"$OUT_DIR/metadata.txt"

for run in $(seq 1 "$RUNS"); do
  for path in $PATHS; do
    for protocol in $PROTOCOLS; do
      run_case "$run" "$path" "$protocol"
    done
  done
done

awk -F, '
  NR == 1 { next }
  $4 == "LAT" {
    key = $2 "/" $6 "/" $7
    count[key]++
    p50[key] += $9
    p99[key] += $11
    p999[key] += $12
  }
  END {
    print "path,protocol,operation,runs,p50_mean_us,p99_mean_us,p99_9_mean_us"
    split("10gbe 100gbe", paths, " ")
    split("RESP SBE", protocols, " ")
    split("PING SET GET", operations, " ")
    for (p = 1; p <= 2; ++p) for (r = 1; r <= 2; ++r) for (o = 1; o <= 3; ++o) {
      key = paths[p] "/" protocols[r] "/" operations[o]
      if (count[key]) printf "%s,%s,%s,%d,%.4f,%.4f,%.4f\n", paths[p], protocols[r], operations[o], count[key], p50[key] / count[key], p99[key] / count[key], p999[key] / count[key]
    }
  }
' "$OUT_DIR/latency.csv" >"$OUT_DIR/aggregate.csv"

log "complete: $OUT_DIR"
printf '%s\n' "$OUT_DIR"
