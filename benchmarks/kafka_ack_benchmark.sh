#!/usr/bin/env bash
set -euo pipefail

# Reproducible Goblin Kafka acknowledgement benchmark. Intended to run on the
# same host as a durable Redpanda installation, using fresh one-partition topics.

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
GOBLIN_BINARY=${GOBLIN_BINARY:-$ROOT/build/goblin-core}
BENCHMARK_BINARY=${BENCHMARK_BINARY:-$ROOT/build/goblin_core_kafka_ack_benchmark}
BROKERS=${BROKERS:-192.168.1.49:9092}
RPK=${RPK:-$(command -v rpk || true)}
ROUNDS=${ROUNDS:-3}
WARMUP=${WARMUP:-2000}
LATENCY_SAMPLES=${LATENCY_SAMPLES:-100000}
PIPELINE_OPERATIONS=${PIPELINE_OPERATIONS:-200000}
PIPELINE_DEPTHS=${PIPELINE_DEPTHS:-8,32,128,512}
TRANSACTION_OPERATIONS=${TRANSACTION_OPERATIONS:-50000}
TRANSACTION_SIZES=${TRANSACTION_SIZES:-1,8,32,128}
SERVER_CORE=${SERVER_CORE:-0}
CLIENT_CORE=${CLIENT_CORE:-4}
LINGER_MS=${LINGER_MS:-0}
COOLDOWN_SECONDS=${COOLDOWN_SECONDS:-2}
STAMP=${STAMP:-$(date -u +%Y%m%dT%H%M%SZ)}
OUTPUT_DIR=${OUTPUT_DIR:-/mnt/local/goblin-kafka-ack-results/$STAMP}
TOPIC_PREFIX=${TOPIC_PREFIX:-goblin-kafka-ack-$STAMP-$$}

if [[ ! -x "$GOBLIN_BINARY" ]]; then
  echo "missing Goblin binary: $GOBLIN_BINARY" >&2
  exit 2
fi
if [[ ! -x "$BENCHMARK_BINARY" ]]; then
  echo "missing benchmark binary: $BENCHMARK_BINARY" >&2
  exit 2
fi
if [[ -z "$RPK" || ! -x "$RPK" ]]; then
  echo "rpk is required" >&2
  exit 2
fi
if [[ ! "$ROUNDS" =~ ^[1-9][0-9]*$ ]]; then
  echo "ROUNDS must be a positive integer" >&2
  exit 2
fi

mkdir -p "$OUTPUT_DIR"
RESULTS=$OUTPUT_DIR/results.csv
RUN_LOG=$OUTPUT_DIR/run.log
METADATA=$OUTPUT_DIR/metadata.txt
active_topic=

cleanup() {
  if [[ -n "$active_topic" ]]; then
    "$RPK" -X brokers="$BROKERS" topic delete "$active_topic" \
      >>"$RUN_LOG" 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

{
  echo "timestamp_utc=$STAMP"
  echo "hostname=$(hostname)"
  echo "goblin_binary=$GOBLIN_BINARY"
  echo "benchmark_binary=$BENCHMARK_BINARY"
  echo "brokers=$BROKERS"
  echo "rounds=$ROUNDS"
  echo "warmup=$WARMUP"
  echo "latency_samples=$LATENCY_SAMPLES"
  echo "pipeline_operations_per_depth=$PIPELINE_OPERATIONS"
  echo "pipeline_depths=$PIPELINE_DEPTHS"
  echo "transaction_operations_per_size=$TRANSACTION_OPERATIONS"
  echo "transaction_sizes=$TRANSACTION_SIZES"
  echo "server_core=$SERVER_CORE"
  echo "client_core=$CLIENT_CORE"
  echo "linger_ms=$LINGER_MS"
  echo
  uname -a
  echo
  lscpu | grep -E 'Model name|Socket|Core|Thread|NUMA node'
  echo
  numactl --hardware 2>/dev/null || true
  echo
  "$RPK" version
  "$RPK" -X brokers="$BROKERS" cluster health || true
  echo "write_caching_default=$($RPK -X brokers="$BROKERS" cluster config get write_caching_default 2>/dev/null || echo unknown)"
  echo "log_segment_size=$($RPK -X brokers="$BROKERS" cluster config get log_segment_size 2>/dev/null || echo unknown)"
  redpanda_pid=$(systemctl show redpanda --property=MainPID --value 2>/dev/null || true)
  if [[ -n "$redpanda_pid" && "$redpanda_pid" != 0 ]]; then
    ps -p "$redpanda_pid" -o pid,psr,pcpu,pmem,args
    taskset -pc "$redpanda_pid" 2>/dev/null || true
  fi
  echo
  uptime
} >"$METADATA" 2>&1

run_one() {
  local round=$1
  local mode=$2
  local raw=$OUTPUT_DIR/${round}-${mode}.csv
  local server_log=$OUTPUT_DIR/${round}-${mode}-server.log
  local socket=/tmp/goblin-kafka-ack-${STAMP}-${round}-${mode}.sock
  local topic=
  local args=(
    --server "$GOBLIN_BINARY"
    --mode "$mode"
    --socket "$socket"
    --server-log "$server_log"
    --warmup "$WARMUP"
    --latency-samples "$LATENCY_SAMPLES"
    --pipeline-operations "$PIPELINE_OPERATIONS"
    --pipeline-depths "$PIPELINE_DEPTHS"
    --transaction-operations "$TRANSACTION_OPERATIONS"
    --transaction-sizes "$TRANSACTION_SIZES"
    --server-core "$SERVER_CORE"
    --client-core "$CLIENT_CORE"
    --linger-ms "$LINGER_MS"
  )

  if [[ "$mode" != none ]]; then
    topic=${TOPIC_PREFIX}-${round}-${mode}
    active_topic=$topic
    "$RPK" -X brokers="$BROKERS" topic create "$topic" \
      --partitions 1 --replicas 1 >>"$RUN_LOG" 2>&1
    args+=(--brokers "$BROKERS" --topic "$topic")
  fi

  echo "[$(date -Is)] round=$round mode=$mode" | tee -a "$RUN_LOG"
  "$BENCHMARK_BINARY" "${args[@]}" \
    2> >(tee -a "$RUN_LOG" >&2) | tee "$raw"

  if [[ ! -f "$RESULTS" ]]; then
    cp "$raw" "$RESULTS"
  else
    tail -n +2 "$raw" >>"$RESULTS"
  fi

  if [[ -n "$topic" ]]; then
    "$RPK" -X brokers="$BROKERS" topic describe "$topic" -p \
      >"$OUTPUT_DIR/${round}-${mode}-topic.txt"
    "$RPK" -X brokers="$BROKERS" topic delete "$topic" \
      >>"$RUN_LOG" 2>&1
    active_topic=
  fi
  sleep "$COOLDOWN_SECONDS"
}

for ((round = 1; round <= ROUNDS; ++round)); do
  case $(((round - 1) % 3)) in
    0) modes=(none queued broker) ;;
    1) modes=(queued broker none) ;;
    2) modes=(broker none queued) ;;
  esac
  for mode in "${modes[@]}"; do
    run_one "$round" "$mode"
  done
done

trap - EXIT INT TERM
echo "results=$RESULTS" | tee -a "$RUN_LOG"
