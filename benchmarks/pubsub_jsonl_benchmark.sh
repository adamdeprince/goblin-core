#!/usr/bin/env bash
# End-to-end SBE/ring Pub/Sub replay of a newline-delimited market feed.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-pubsub-jsonl}"
DATASET="${DATASET:-/mnt/local/goblin-core-pubsub.jsonl}"
CHANNELS="${CHANNELS:-/mnt/local/goblin-core-pubsub.channels}"
RESULTS="${RESULTS:-/mnt/local/goblin-core-pubsub-results}"
GOBLIN="${GOBLIN:-$BUILD/goblin-core}"
PUBLISHER="${PUBLISHER:-$BUILD/goblin_core_pubsub_jsonl_publisher}"
CONSUMER="${CONSUMER:-$BUILD/goblin_core_pubsub_jsonl_consumer}"
PYTHON="${PYTHON:-python3}"
CHANNEL_WORKERS="${CHANNEL_WORKERS:-8}"
NUMA_NODE="${NUMA_NODE:-0}"
SERVER_CPU="${SERVER_CPU:-4}"
CONSUMER_CPU="${CONSUMER_CPU:-8}"
PUBLISHER_CPU="${PUBLISHER_CPU:-12}"
PORT="${PORT:-16379}"
RING_SIZE="${RING_SIZE:-1mb}"
EVICT_FILE_CACHE="${EVICT_FILE_CACHE:-1}"
RUN_WILDCARD="${RUN_WILDCARD:-1}"
GENERATE_CHANNELS="${GENERATE_CHANNELS:-1}"
RUN_LITERALS="${RUN_LITERALS:-1}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RUN_RESULTS="$RESULTS/$RUN_ID"
RUNTIME="$(mktemp -d "/dev/shm/goblin-pubsub-jsonl.${RUN_ID}.XXXXXX")"
SERVER_PID=""
CONSUMER_PID=""
PUBLISHER_PID=""

cleanup() {
  if [[ -n "$PUBLISHER_PID" ]]; then
    kill "$PUBLISHER_PID" 2>/dev/null || true
    wait "$PUBLISHER_PID" 2>/dev/null || true
  fi
  if [[ -n "$CONSUMER_PID" ]]; then
    kill "$CONSUMER_PID" 2>/dev/null || true
    wait "$CONSUMER_PID" 2>/dev/null || true
  fi
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$RUNTIME"
}
trap cleanup EXIT

require_executable() {
  if [[ ! -x "$1" ]]; then
    printf 'missing executable: %s\n' "$1" >&2
    exit 1
  fi
}

verify_placement() {
  local pid="$1"
  local expected_cpu="$2"
  local label="$3"
  local require_bound_policy="${4:-1}"
  local actual_cpus
  local actual_policy
  actual_cpus="$(awk '/^Cpus_allowed_list:/ {print $2}' "/proc/$pid/status")"
  actual_policy="$(awk 'NR == 1 {print $2}' "/proc/$pid/numa_maps")"
  if [[ "$actual_cpus" != "$expected_cpu" ]]; then
    printf '%s placement mismatch: CPUs=%s (expected %s), policy=%s (expected bind:%s)\n' \
      "$label" "$actual_cpus" "$expected_cpu" "$actual_policy" "$NUMA_NODE" >&2
    return 1
  fi
  if [[ "$require_bound_policy" == 1 &&
        "$actual_policy" != "bind:$NUMA_NODE" ]]; then
    printf '%s memory-policy mismatch: policy=%s (expected bind:%s)\n' \
      "$label" "$actual_policy" "$NUMA_NODE" >&2
    return 1
  fi
}

wait_ready() {
  local ready_file="$1"
  local deadline=$((SECONDS + 600))
  while [[ ! -s "$ready_file" ]]; do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      printf 'server exited before subscriber became ready\n' >&2
      return 1
    fi
    if ! kill -0 "$CONSUMER_PID" 2>/dev/null; then
      printf 'consumer exited before becoming ready\n' >&2
      return 1
    fi
    if (( SECONDS >= deadline )); then
      printf 'timed out waiting for subscriber readiness\n' >&2
      return 1
    fi
    sleep 0.01
  done
}

run_case() {
  local case_name="$1"
  local subscription_option="$2"
  local subscription_value="$3"
  local publisher_ring="$RUNTIME/$case_name-publisher.ring"
  local consumer_ring="$RUNTIME/$case_name-consumer.ring"
  local ready_file="$RUNTIME/$case_name.ready"
  local completion_channel="__GOBLIN_BENCHMARK__:${RUN_ID}:${case_name}:DONE"

  rm -f "$publisher_ring" "$consumer_ring" "$ready_file"
  numactl --physcpubind="$SERVER_CPU" --membind="$NUMA_NODE" \
    taskset -c "$SERVER_CPU" \
    "$GOBLIN" \
      --enable-sbe \
      --cpu "$SERVER_CPU" \
      --numa "$NUMA_NODE" \
      --port "$PORT" \
      --ring "$publisher_ring" "$RING_SIZE" \
      --ring "$consumer_ring" "$RING_SIZE" \
      >"$RUN_RESULTS/$case_name-server.log" 2>&1 &
  SERVER_PID=$!

  numactl --physcpubind="$CONSUMER_CPU" --membind="$NUMA_NODE" \
    taskset -c "$CONSUMER_CPU" \
    "$CONSUMER" \
      --ring "$consumer_ring" \
      "$subscription_option" "$subscription_value" \
      --completion-channel "$completion_channel" \
      --ready-file "$ready_file" \
      >"$RUN_RESULTS/$case_name-consumer.json" \
      2>"$RUN_RESULTS/$case_name-consumer.log" &
  CONSUMER_PID=$!
  wait_ready "$ready_file"
  # Goblin uses --numa while prefaulting the ring mappings, then restores its
  # ambient policy; its exact CPU pin keeps subsequent first-touch memory local.
  verify_placement "$SERVER_PID" "$SERVER_CPU" server 0
  verify_placement "$CONSUMER_PID" "$CONSUMER_CPU" consumer

  local cache_option=()
  if [[ "$EVICT_FILE_CACHE" == 1 ]]; then
    cache_option=(--evict-file-cache)
  fi
  numactl --physcpubind="$PUBLISHER_CPU" --membind="$NUMA_NODE" \
    taskset -c "$PUBLISHER_CPU" \
    "$PUBLISHER" \
      --ring "$publisher_ring" \
      --input "$DATASET" \
      --completion-channel "$completion_channel" \
      --expected-subscribers 1 \
      "${cache_option[@]}" \
      >"$RUN_RESULTS/$case_name-publisher.json" \
      2>"$RUN_RESULTS/$case_name-publisher.log" &
  PUBLISHER_PID=$!
  verify_placement "$PUBLISHER_PID" "$PUBLISHER_CPU" publisher
  wait "$PUBLISHER_PID"
  PUBLISHER_PID=""

  wait "$CONSUMER_PID"
  CONSUMER_PID=""
  kill "$SERVER_PID"
  wait "$SERVER_PID" || true
  SERVER_PID=""
}

mkdir -p "$RUN_RESULTS"
require_executable "$GOBLIN"
require_executable "$PUBLISHER"
require_executable "$CONSUMER"
if [[ ! -r "$DATASET" ]]; then
  printf 'dataset is not readable: %s\n' "$DATASET" >&2
  exit 1
fi
if command -v systemctl >/dev/null &&
   systemctl is-active --quiet numad 2>/dev/null; then
  printf 'numad is active; stop it before running a fixed-NUMA benchmark\n' >&2
  exit 1
fi

cat >"$RUN_RESULTS/environment.txt" <<EOF
dataset=$DATASET
dataset_bytes=$(stat -c %s "$DATASET")
numa_node=$NUMA_NODE
server_cpu=$SERVER_CPU
consumer_cpu=$CONSUMER_CPU
publisher_cpu=$PUBLISHER_CPU
ring_size=$RING_SIZE
file_cache_advisory=$([[ "$EVICT_FILE_CACHE" == 1 ]] && echo evict || echo unchanged)
goblin=$GOBLIN
EOF

if [[ "$RUN_WILDCARD" == 1 ]]; then
  printf 'running wildcard pattern replay\n' >&2
  run_case wildcard --pattern '*'
fi

if [[ "$GENERATE_CHANNELS" == 1 ]]; then
  printf 'extracting literal channel set\n' >&2
  numactl --cpunodebind="$NUMA_NODE" --membind="$NUMA_NODE" \
    "$PYTHON" "$HERE/pubsub_jsonl_channels.py" "$DATASET" "$CHANNELS" \
    --workers "$CHANNEL_WORKERS" \
    >"$RUN_RESULTS/channel-generation.json" \
    2>"$RUN_RESULTS/channel-generation.log"
elif [[ "$RUN_LITERALS" == 1 && ! -r "$CHANNELS" ]]; then
  printf 'channel file is not readable: %s\n' "$CHANNELS" >&2
  exit 1
fi

if [[ "$RUN_LITERALS" == 1 ]]; then
  printf 'running literal subscription-table replay\n' >&2
  run_case literals --channels "$CHANNELS"
fi

printf 'results=%s\n' "$RUN_RESULTS"
for result in "$RUN_RESULTS"/*.json; do
  printf '%s: ' "$(basename "$result")"
  tr -d '\n' <"$result"
  printf '\n'
done
