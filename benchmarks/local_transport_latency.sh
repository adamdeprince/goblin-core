#!/usr/bin/env bash
# Comparable depth-one RESP/SBE latency over Goblin's local transports.
#
# Each of the eight protocol/transport cases gets a fresh server. Aeron IPC
# shares one local Media Driver; Aeron loopback UDP uses separate server/client
# drivers so packets cross the kernel loopback UDP path. The probe measures
# identical PING, SET, and GET operations with rdtscp on x86.
#
# Intended benchmark host:
#   bash benchmarks/local_transport_latency.sh
#
# Useful overrides:
#   AERON_PREFIX=$HOME/opt/aeron-1.51.0-x86_64
#   BUILD_DIR=$PWD/build-aeron-rel SKIP_BUILD=1
#   SAMPLES=200000 WARMUP=20000
#   SERVER_CPU=2 CLIENT_CPU=3 OUT_DIR=/tmp/goblin-local-results
#   AERON_SENDER_IDLE_STRATEGY=backoff (trade latency for idle CPU)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$ROOT/build-aeron-rel}"
AERON_PREFIX="${AERON_PREFIX:-$HOME/opt/aeron-1.51.0-$(uname -m)}"
AERONMD="${AERONMD:-$AERON_PREFIX/bin/aeronmd_s}"
GOBLIN="${GOBLIN:-$BUILD_DIR/goblin-core}"
PROBE="${PROBE:-$BUILD_DIR/goblin_core_local_transport_latency_benchmark}"
SAMPLES="${SAMPLES:-100000}"
WARMUP="${WARMUP:-10000}"
SKIP_BUILD="${SKIP_BUILD:-0}"

# Aeron's DEDICATED threading mode assigns separate agents but does not make
# them continuously poll: the C Media Driver otherwise defaults each agent to
# exponential backoff. This latency benchmark follows Aeron's low-latency C
# driver profile. Callers may override these when measuring another policy.
AERON_CONDUCTOR_IDLE_STRATEGY="${AERON_CONDUCTOR_IDLE_STRATEGY:-spin}"
AERON_SENDER_IDLE_STRATEGY="${AERON_SENDER_IDLE_STRATEGY:-noop}"
AERON_RECEIVER_IDLE_STRATEGY="${AERON_RECEIVER_IDLE_STRATEGY:-noop}"
AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND="${AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND:-2}"

# naamah has 64 physical cores numbered 0-63, followed by their SMT siblings.
# Keep the measuring threads, Aeron client conductors, and Media Driver agents
# on distinct physical cores by default.
SERVER_CPU="${SERVER_CPU:-2}"
CLIENT_CPU="${CLIENT_CPU:-3}"
DRIVER_SERVER_CONDUCTOR_CPU="${DRIVER_SERVER_CONDUCTOR_CPU:-4}"
DRIVER_SERVER_RECEIVER_CPU="${DRIVER_SERVER_RECEIVER_CPU:-5}"
DRIVER_SERVER_SENDER_CPU="${DRIVER_SERVER_SENDER_CPU:-6}"
DRIVER_CLIENT_CONDUCTOR_CPU="${DRIVER_CLIENT_CONDUCTOR_CPU:-7}"
DRIVER_CLIENT_RECEIVER_CPU="${DRIVER_CLIENT_RECEIVER_CPU:-8}"
DRIVER_CLIENT_SENDER_CPU="${DRIVER_CLIENT_SENDER_CPU:-9}"
SERVER_AERON_CLIENT_CPU="${SERVER_AERON_CLIENT_CPU:-10}"
CLIENT_AERON_CLIENT_CPU="${CLIENT_AERON_CLIENT_CPU:-11}"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_DIR="${OUT_DIR:-$ROOT/benchmark-results/local-transports-$(hostname)-$STAMP}"
RUN_DIR="$(mktemp -d /tmp/goblin-local-transports.XXXXXX)"
RESULTS="$OUT_DIR/latency.csv"
METADATA="$OUT_DIR/metadata.txt"

SERVER_PID=""
DRIVER_SERVER_PID=""
DRIVER_CLIENT_PID=""

log() { printf '[local-transports] %s\n' "$*" >&2; }

stop_pid() {
  local pid="$1"
  if [[ -n "$pid" ]]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}

stop_case() {
  stop_pid "$SERVER_PID"
  stop_pid "$DRIVER_CLIENT_PID"
  stop_pid "$DRIVER_SERVER_PID"
  SERVER_PID=""
  DRIVER_CLIENT_PID=""
  DRIVER_SERVER_PID=""
}

cleanup() {
  stop_case
  rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

if [[ ! -x "$AERONMD" ]]; then
  log "Aeron Media Driver not found: $AERONMD"
  log "build it with: $ROOT/scripts/build-aeron.sh $AERON_PREFIX"
  exit 1
fi

if [[ "$SKIP_BUILD" != "1" ]]; then
  log "configure release build in $BUILD_DIR"
  cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DGOBLIN_CORE_ENABLE_AERON=ON \
    -DGOBLIN_CORE_AERON_ROOT="$AERON_PREFIX" \
    -DGOBLIN_CORE_ENABLE_KAFKA=OFF \
    -DGOBLIN_CORE_BUILD_BENCHMARKS=ON \
    -DGOBLIN_CORE_BUILD_TESTS=OFF
  cmake --build "$BUILD_DIR" --parallel \
    --target goblin_core_server goblin_core_local_transport_latency_benchmark
fi

if [[ ! -x "$GOBLIN" || ! -x "$PROBE" ]]; then
  log "missing benchmark binaries: GOBLIN=$GOBLIN PROBE=$PROBE"
  exit 1
fi

mkdir -p "$OUT_DIR"
printf '%s\n' \
  'kind,transport,protocol,operation,min_us,p50_us,p90_us,p99_us,p99_9_us,mean_us,samples' \
  >"$RESULTS"

{
  printf 'timestamp_utc=%s\n' "$STAMP"
  printf 'hostname=%s\n' "$(hostname)"
  printf 'kernel=%s\n' "$(uname -srvo 2>/dev/null || uname -a)"
  printf 'architecture=%s\n' "$(uname -m)"
  printf 'git_commit=%s\n' "$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || printf unknown)"
  printf 'git_status_short=%s\n' "$(git -C "$ROOT" status --short 2>/dev/null | tr '\n' ';')"
  printf 'compiler=%s\n' "$(c++ --version 2>/dev/null | head -n 1 || true)"
  printf 'cmake=%s\n' "$(cmake --version | head -n 1)"
  printf 'aeronmd=%s\n' "$AERONMD"
  aeronmd_hash="$(
    sha256sum "$AERONMD" 2>/dev/null | awk '{print $1}' ||
      shasum -a 256 "$AERONMD" | awk '{print $1}'
  )"
  printf 'aeronmd_sha256=%s\n' "$aeronmd_hash"
  printf 'samples=%s\n' "$SAMPLES"
  printf 'warmup=%s\n' "$WARMUP"
  printf 'goblin_cpp_client_conductor_idle_strategy=spin\n'
  printf 'aeron_driver_conductor_idle_strategy=%s\n' \
    "$AERON_CONDUCTOR_IDLE_STRATEGY"
  printf 'aeron_driver_sender_idle_strategy=%s\n' \
    "$AERON_SENDER_IDLE_STRATEGY"
  printf 'aeron_driver_receiver_idle_strategy=%s\n' \
    "$AERON_RECEIVER_IDLE_STRATEGY"
  printf 'aeron_network_publication_max_messages_per_send=%s\n' \
    "$AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND"
  printf 'server_cpu=%s\n' "$SERVER_CPU"
  printf 'client_cpu=%s\n' "$CLIENT_CPU"
  printf 'server_aeron_client_cpu=%s\n' "$SERVER_AERON_CLIENT_CPU"
  printf 'client_aeron_client_cpu=%s\n' "$CLIENT_AERON_CLIENT_CPU"
  printf 'server_driver_cpus=%s,%s,%s\n' \
    "$DRIVER_SERVER_CONDUCTOR_CPU" "$DRIVER_SERVER_RECEIVER_CPU" \
    "$DRIVER_SERVER_SENDER_CPU"
  printf 'client_driver_cpus=%s,%s,%s\n' \
    "$DRIVER_CLIENT_CONDUCTOR_CPU" "$DRIVER_CLIENT_RECEIVER_CPU" \
    "$DRIVER_CLIENT_SENDER_CPU"
  if command -v lscpu >/dev/null 2>&1; then
    lscpu
  fi
} >"$METADATA"

start_driver() {
  local role="$1" directory="$2" conductor_cpu="$3" receiver_cpu="$4"
  local sender_cpu="$5" log_file="$6"
  mkdir -p "$directory"
  env \
    AERON_CONDUCTOR_IDLE_STRATEGY="$AERON_CONDUCTOR_IDLE_STRATEGY" \
    AERON_SENDER_IDLE_STRATEGY="$AERON_SENDER_IDLE_STRATEGY" \
    AERON_RECEIVER_IDLE_STRATEGY="$AERON_RECEIVER_IDLE_STRATEGY" \
    AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND="$AERON_NETWORK_PUBLICATION_MAX_MESSAGES_PER_SEND" \
    "$AERONMD" \
    "-Daeron.dir=$directory" \
    -Daeron.dir.delete.on.start=true \
    -Daeron.dir.delete.on.shutdown=true \
    -Daeron.threading.mode=DEDICATED \
    "-Daeron.conductor.cpu.affinity=$conductor_cpu" \
    "-Daeron.receiver.cpu.affinity=$receiver_cpu" \
    "-Daeron.sender.cpu.affinity=$sender_cpu" \
    >"$log_file" 2>&1 &
  local pid=$!
  if [[ "$role" == "server" ]]; then
    DRIVER_SERVER_PID="$pid"
  else
    DRIVER_CLIENT_PID="$pid"
  fi

  for _ in $(seq 1 200); do
    if [[ -f "$directory/cnc.dat" ]]; then return 0; fi
    if ! kill -0 "$pid" 2>/dev/null; then
      log "$role Media Driver exited; see $log_file"
      return 1
    fi
    sleep 0.05
  done
  log "$role Media Driver did not become ready; see $log_file"
  return 1
}

pin_server_threads() {
  local pid="$1"
  if ! command -v taskset >/dev/null 2>&1; then return 0; fi
  local task tid cpu
  for task in /proc/"$pid"/task/*; do
    tid="${task##*/}"
    cpu="$SERVER_AERON_CLIENT_CPU"
    if [[ "$tid" == "$pid" ]]; then cpu="$SERVER_CPU"; fi
    taskset -pc "$cpu" "$tid" >/dev/null
  done
}

wait_for_server() {
  local kind="$1" resource="$2" log_file="$3"
  for _ in $(seq 1 200); do
    case "$kind" in
      ring) [[ -e "$resource" ]] && return 0 ;;
      uds) [[ -S "$resource" ]] && return 0 ;;
      aeron) grep -q 'Aeron .* ready' "$log_file" 2>/dev/null && return 0 ;;
    esac
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      log "server exited; see $log_file"
      return 1
    fi
    sleep 0.05
  done
  log "server did not become ready; see $log_file"
  return 1
}

run_probe() {
  local case_name="$1"
  shift
  local csv="$OUT_DIR/$case_name.csv"
  local human="$OUT_DIR/$case_name.txt"
  log "$case_name ($SAMPLES measured after $WARMUP warmup)"
  if ! env SAMPLES="$SAMPLES" WARMUP="$WARMUP" \
      CLIENT_CPU="$CLIENT_CPU" \
      AERON_CLIENT_CPU="$CLIENT_AERON_CLIENT_CPU" \
      "$PROBE" "$@" >"$csv" 2> >(tee "$human" >&2); then
    log "$case_name failed; server log: $OUT_DIR/$case_name.server.log"
    tail -n 80 "$OUT_DIR/$case_name.server.log" >&2 || true
    return 1
  fi
  cat "$csv" >>"$RESULTS"
}

run_case() {
  local transport="$1" protocol="$2"
  local protocol_lower
  protocol_lower="$(printf '%s' "$protocol" | tr '[:upper:]' '[:lower:]')"
  local case_name="$transport-$protocol_lower"
  local case_dir="$RUN_DIR/$case_name"
  local server_log="$OUT_DIR/$case_name.server.log"
  local ring_path="$case_dir/goblin.ring"
  local uds_path="$case_dir/goblin.sock"
  local server_aeron_dir="$case_dir/aeron-server"
  local client_aeron_dir="$case_dir/aeron-client"
  local request_stream=1001 response_stream=1002
  local port_base=$((42000 + ($$ % 1000) * 2))
  local tcp_port=$((45000 + ($$ % 10000)))
  local request_endpoint="127.0.0.1:$port_base"
  local response_endpoint="127.0.0.1:$((port_base + 1))"
  local mode args ready_kind ready_resource

  mkdir -p "$case_dir"
  stop_case

  case "$transport" in
    ring)
      "$GOBLIN" --enable-sbe --port "$tcp_port" \
        --ring "$ring_path" 1mb \
        >"$server_log" 2>&1 &
      SERVER_PID=$!
      mode="ring-$protocol_lower"
      args=("$ring_path")
      ready_kind=ring
      ready_resource="$ring_path"
      ;;
    uds)
      "$GOBLIN" --enable-sbe --port "$tcp_port" \
        --uds-listen "$uds_path" \
        >"$server_log" 2>&1 &
      SERVER_PID=$!
      mode="uds-$protocol_lower"
      args=("$uds_path")
      ready_kind=uds
      ready_resource="$uds_path"
      ;;
    aeron-ipc)
      start_driver server "$server_aeron_dir" \
        "$DRIVER_SERVER_CONDUCTOR_CPU" "$DRIVER_SERVER_RECEIVER_CPU" \
        "$DRIVER_SERVER_SENDER_CPU" "$OUT_DIR/$case_name.driver.log"
      "$GOBLIN" --enable-sbe --port "$tcp_port" \
        --aeron-dir "$server_aeron_dir" \
        --aeron-ipc "$request_stream" "$response_stream" \
        >"$server_log" 2>&1 &
      SERVER_PID=$!
      mode="aeron-ipc-$protocol_lower"
      args=("$server_aeron_dir" "$request_stream" "$response_stream")
      ready_kind=aeron
      ready_resource=""
      ;;
    aeron-udp)
      request_stream=2001
      response_stream=2002
      start_driver server "$server_aeron_dir" \
        "$DRIVER_SERVER_CONDUCTOR_CPU" "$DRIVER_SERVER_RECEIVER_CPU" \
        "$DRIVER_SERVER_SENDER_CPU" "$OUT_DIR/$case_name.server-driver.log"
      start_driver client "$client_aeron_dir" \
        "$DRIVER_CLIENT_CONDUCTOR_CPU" "$DRIVER_CLIENT_RECEIVER_CPU" \
        "$DRIVER_CLIENT_SENDER_CPU" "$OUT_DIR/$case_name.client-driver.log"
      "$GOBLIN" --enable-sbe --port "$tcp_port" \
        --aeron-dir "$server_aeron_dir" \
        --aeron-udp "$request_endpoint" "$request_stream" \
        "$response_endpoint" "$response_stream" \
        >"$server_log" 2>&1 &
      SERVER_PID=$!
      mode="aeron-udp-$protocol_lower"
      args=("$client_aeron_dir" "$request_endpoint" "$request_stream"
            "$response_endpoint" "$response_stream")
      ready_kind=aeron
      ready_resource=""
      ;;
    *)
      log "unknown transport: $transport"
      return 2
      ;;
  esac

  wait_for_server "$ready_kind" "$ready_resource" "$server_log"
  pin_server_threads "$SERVER_PID"
  run_probe "$case_name" "$mode" "${args[@]}"
  stop_case
}

log "results: $OUT_DIR"
for transport in ring aeron-ipc aeron-udp uds; do
  run_case "$transport" RESP
  run_case "$transport" SBE
done

awk -F, '
  NR == 1 { next }
  $1 == "LAT" { p50[$2 "/" $3 "/" $4] = $6; p99[$2 "/" $3 "/" $4] = $8 }
  END {
    split("ring aeron-ipc aeron-udp uds", transports, " ")
    split("RESP SBE", protocols, " ")
    split("PING SET GET", operations, " ")
    print ""
    print "median round-trip latency (microseconds)"
    printf "%-22s %9s %9s %9s\n", "protocol/transport", "PING", "SET", "GET"
    for (t = 1; t <= 4; ++t) for (p = 1; p <= 2; ++p) {
      label = protocols[p] "/" transports[t]
      printf "%-22s", label
      for (o = 1; o <= 3; ++o) {
        key = transports[t] "/" protocols[p] "/" operations[o]
        printf " %9.3f", p50[key]
      }
      print ""
    }
    print ""
    print "p99 round-trip latency (microseconds)"
    printf "%-22s %9s %9s %9s\n", "protocol/transport", "PING", "SET", "GET"
    for (t = 1; t <= 4; ++t) for (p = 1; p <= 2; ++p) {
      label = protocols[p] "/" transports[t]
      printf "%-22s", label
      for (o = 1; o <= 3; ++o) {
        key = transports[t] "/" protocols[p] "/" operations[o]
        printf " %9.3f", p99[key]
      }
      print ""
    }
  }
' "$RESULTS" | tee "$OUT_DIR/summary.txt"

log "complete: $RESULTS"
