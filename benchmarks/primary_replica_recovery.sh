#!/usr/bin/env bash
set -euo pipefail

primary_service=${PRIMARY_SERVICE:-goblin-core.service}
replica_service=${REPLICA_SERVICE:-goblin-core-replica-test.service}
worker=${RECOVERY_WORKER:-/mnt/local/goblin-core-recovery-test/goblin_core_external_logging_recovery}
primary_snapshot=${PRIMARY_SNAPSHOT:-/mnt/local/goblin-core/state/goblin.snapshot}
replica_snapshot=${REPLICA_SNAPSHOT:-/mnt/local/goblin-core/state/goblin-replica.snapshot}
result_root=${RECOVERY_RESULT_ROOT:-/mnt/local/goblin-core-recovery-test/results}
primary_host=${PRIMARY_HOST:-127.0.0.1}
primary_port=${PRIMARY_PORT:-6379}
replica_host=${REPLICA_HOST:-127.0.0.1}
replica_port=${REPLICA_PORT:-6380}
baseline_count=${BASELINE_COUNT:-1000000}
delta_count=${DELTA_COUNT:-1000}
pipeline=${RECOVERY_PIPELINE:-512}
timeout_seconds=${RECOVERY_TIMEOUT_SECONDS:-300}
snapshot_timeout=${SNAPSHOT_TIMEOUT_SECONDS:-300}
data_type=${RECOVERY_DATA_TYPE:-all}
key_prefix=${RECOVERY_KEY_PREFIX:-external-logging-test}
kafka_brokers=${KAFKA_BROKERS:-192.168.1.49:9092}
kafka_topic=${KAFKA_TOPIC:-goblin-core-replication}

usage() {
  cat <<EOF
usage: $0 [options]
  --data-type string|hash|set|zset|list|array|all
  --baseline-count N        objects required before the test
  --delta-count N           objects added after primary recovery
  --pipeline N              RESP pipeline depth
  --key-prefix PREFIX       tested key namespace
  --primary-service UNIT    primary systemd unit
  --replica-service UNIT    replica systemd unit
  --primary-snapshot PATH   primary snapshot path
  --replica-snapshot PATH   replica bootstrap snapshot path
  --primary-host HOST --primary-port PORT
  --replica-host HOST --replica-port PORT
  --worker PATH             external_logging_recovery worker
  --timeout N               service/reconnect timeout in seconds
  --snapshot-timeout N      background-save timeout in seconds
  --result-root PATH
  --kafka-brokers HOSTS --kafka-topic TOPIC
EOF
}

while (($# != 0)); do
  case "$1" in
    --data-type) data_type=$2; shift 2 ;;
    --baseline-count) baseline_count=$2; shift 2 ;;
    --delta-count) delta_count=$2; shift 2 ;;
    --pipeline) pipeline=$2; shift 2 ;;
    --key-prefix) key_prefix=$2; shift 2 ;;
    --primary-service) primary_service=$2; shift 2 ;;
    --replica-service) replica_service=$2; shift 2 ;;
    --primary-snapshot) primary_snapshot=$2; shift 2 ;;
    --replica-snapshot) replica_snapshot=$2; shift 2 ;;
    --primary-host) primary_host=$2; shift 2 ;;
    --primary-port) primary_port=$2; shift 2 ;;
    --replica-host) replica_host=$2; shift 2 ;;
    --replica-port) replica_port=$2; shift 2 ;;
    --worker) worker=$2; shift 2 ;;
    --timeout) timeout_seconds=$2; shift 2 ;;
    --snapshot-timeout) snapshot_timeout=$2; shift 2 ;;
    --result-root) result_root=$2; shift 2 ;;
    --kafka-brokers) kafka_brokers=$2; shift 2 ;;
    --kafka-topic) kafka_topic=$2; shift 2 ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$data_type" in
  string|strings|hash|hset|set|zset|sorted-set|list|array|all) ;;
  *) echo "invalid --data-type: $data_type" >&2; exit 2 ;;
esac
for value in "$baseline_count" "$delta_count" "$pipeline" \
             "$timeout_seconds" "$snapshot_timeout"; do
  if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
    echo "counts and timeouts must be positive integers" >&2
    exit 2
  fi
done
final_count=$((baseline_count + delta_count))

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
run_dir="$result_root/${timestamp}-primary-replica-${data_type}"
mkdir -p "$run_dir"
exec > >(tee "$run_dir/run.log") 2>&1

echo "run_dir=$run_dir"
echo "primary_service=$primary_service"
echo "replica_service=$replica_service"
echo "data_type=$data_type"
echo "key_prefix=$key_prefix"
echo "baseline_count=$baseline_count"
echo "delta_count=$delta_count"
echo "final_count=$final_count"
echo "pipeline=$pipeline"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

info_field() {
  local host=$1 port=$2 field=$3
  redis-cli --raw -h "$host" -p "$port" INFO replication 2>/dev/null |
    tr -d '\r' | awk -F: -v key="$field" '$1 == key { print substr($0, length(key) + 2); exit }'
}

wait_for_probe() {
  local host=$1 port=$2 label=$3 deadline=$((SECONDS + timeout_seconds))
  while ! "$worker" --action probe --host "$host" --port "$port" \
      --data-type "$data_type" --key-prefix "$key_prefix" \
      --count "$baseline_count" --snapshot-at "$baseline_count" \
      --pipeline "$pipeline" >/dev/null 2>&1; do
    if ((SECONDS >= deadline)); then
      fail "$label did not open its listener within $timeout_seconds seconds"
    fi
    sleep 0.1
  done
}

wait_for_replica_state() {
  local expected=$1 deadline=$((SECONDS + timeout_seconds)) state
  while true; do
    state=$(info_field "$replica_host" "$replica_port" replica_state || true)
    if [[ "$state" == "$expected" ]]; then
      return
    fi
    if ((SECONDS >= deadline)); then
      fail "replica did not reach state $expected; last state was ${state:-unavailable}"
    fi
    sleep 0.1
  done
}

wait_for_replica_offset() {
  local expected=$1 deadline=$((SECONDS + timeout_seconds)) ready offset
  while true; do
    ready=$(info_field "$replica_host" "$replica_port" goblin_ready || true)
    offset=$(info_field "$replica_host" "$replica_port" slave_repl_offset || true)
    if [[ "$ready" == 1 && "$offset" == "$expected" ]]; then
      return
    fi
    if ((SECONDS >= deadline)); then
      fail "replica did not reach offset $expected; ready=${ready:-?} offset=${offset:-?}"
    fi
    sleep 0.1
  done
}

capture_process() {
  local service=$1 label=$2
  local pid
  pid=$(systemctl show "$service" --property=MainPID --value)
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || fail "$service has invalid MainPID $pid"
  cp "/proc/$pid/cmdline" "$run_dir/$label.cmdline"
  systemctl cat "$service" --no-pager > "$run_dir/$label.unit"
  echo "${label}_pid=$pid"
  echo "${label}_cmdline_sha256=$(sha256sum "$run_dir/$label.cmdline" | awk '{print $1}')"
}

kill_and_hold() {
  local service=$1 label=$2
  local pid
  pid=$(systemctl show "$service" --property=MainPID --value)
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || fail "$service has invalid MainPID $pid"
  echo "killing_${label}_pid=$pid"
  sudo kill -KILL "$pid"
  sudo systemctl stop "$service"
  if systemctl is-active --quiet "$service"; then
    fail "$label remained active after SIGKILL and hold"
  fi
  if [[ -e "/proc/$pid" ]]; then
    fail "$label PID $pid survived SIGKILL"
  fi
  echo "${label}_held_down=yes"
}

start_service() {
  local service=$1 label=$2 host=$3 port=$4
  sudo systemctl start "$service"
  wait_for_probe "$host" "$port" "$label"
  capture_process "$service" "$label.after-restart"
  if ! cmp -s "$run_dir/$label.before.cmdline" \
              "$run_dir/$label.after-restart.cmdline"; then
    fail "$label command line changed across restart"
  fi
  if ! cmp -s "$run_dir/$label.before.unit" \
              "$run_dir/$label.after-restart.unit"; then
    fail "$label unit definition changed across restart"
  fi
  echo "${label}_command_line_identical=yes"
  echo "${label}_unit_definition_identical=yes"
}

verify_objects() {
  local host=$1 port=$2 count=$3 label=$4
  echo "verification_begin=$label"
  "$worker" --action verify --host "$host" --port "$port" \
    --data-type "$data_type" --key-prefix "$key_prefix" --count "$count" \
    --snapshot-at "$count" --pipeline "$pipeline"
  echo "verification_pass=$label"
}

test -x "$worker"
command -v redis-cli >/dev/null
test "$(systemctl is-active "$primary_service")" = active
systemctl cat "$replica_service" --no-pager >/dev/null

echo "phase=verify-primary-baseline"
verify_objects "$primary_host" "$primary_port" "$baseline_count" \
  primary-baseline

snapshot_inode_before=missing
if [[ -e "$primary_snapshot" ]]; then
  snapshot_inode_before=$(stat --printf='%i' "$primary_snapshot")
fi
save_reply=$(redis-cli --raw -h "$primary_host" -p "$primary_port" \
  GOBLIN.SAVE "$primary_snapshot" ACCEL | tr -d '\r')
[[ "$save_reply" == "Background saving started" ]] || \
  fail "unexpected GOBLIN.SAVE reply: $save_reply"

snapshot_deadline=$((SECONDS + snapshot_timeout))
while true; do
  snapshot_inode_after=missing
  if [[ -e "$primary_snapshot" ]]; then
    snapshot_inode_after=$(stat --printf='%i' "$primary_snapshot")
  fi
  if [[ "$snapshot_inode_after" != missing &&
        "$snapshot_inode_after" != "$snapshot_inode_before" ]]; then
    break
  fi
  if ((SECONDS >= snapshot_deadline)); then
    fail "primary snapshot did not complete within $snapshot_timeout seconds"
  fi
  sleep 0.1
done
cp --reflink=auto "$primary_snapshot" "$replica_snapshot"
echo "primary_snapshot_bytes=$(stat --printf='%s' "$primary_snapshot")"
echo "replica_snapshot_sha256=$(sha256sum "$replica_snapshot" | awk '{print $1}')"

sudo systemctl stop "$replica_service"
sudo systemctl start "$replica_service"
wait_for_probe "$replica_host" "$replica_port" replica
wait_for_replica_state live
capture_process "$primary_service" primary.before
capture_process "$replica_service" replica.before
verify_objects "$replica_host" "$replica_port" "$baseline_count" \
  replica-initial

echo "phase=kill-primary"
kill_and_hold "$primary_service" primary
wait_for_replica_state reconnecting
replica_ready_while_primary_down=$(
  info_field "$replica_host" "$replica_port" goblin_ready)
echo "$replica_ready_while_primary_down" \
  > "$run_dir/replica-ready-while-primary-down.txt"
[[ "$replica_ready_while_primary_down" == 0 ]] || \
  fail "replica advertised ready while its primary was down"
verify_objects "$replica_host" "$replica_port" "$baseline_count" \
  replica-while-primary-down

echo "phase=restart-primary"
start_service "$primary_service" primary "$primary_host" "$primary_port"
wait_for_replica_state live
verify_objects "$primary_host" "$primary_port" "$baseline_count" \
  primary-after-rebuild

echo "phase=write-post-recovery-suffix"
"$worker" --action continue --host "$primary_host" --port "$primary_port" \
  --data-type "$data_type" --key-prefix "$key_prefix" \
  --count "$final_count" --snapshot-at "$baseline_count" \
  --pipeline "$pipeline"
primary_offset=$(info_field "$primary_host" "$primary_port" master_repl_offset)
wait_for_replica_offset "$primary_offset"
echo "post_suffix_replication_offset=$primary_offset"

echo "phase=kill-replica"
kill_and_hold "$replica_service" replica
verify_objects "$primary_host" "$primary_port" "$final_count" \
  primary-while-replica-down

echo "phase=restart-replica"
start_service "$replica_service" replica "$replica_host" "$replica_port"
wait_for_replica_offset "$primary_offset"
verify_objects "$replica_host" "$replica_port" "$final_count" \
  replica-after-rebuild

echo "phase=final-both-servers"
verify_objects "$primary_host" "$primary_port" "$final_count" primary-final
verify_objects "$replica_host" "$replica_port" "$final_count" replica-final

redis-cli --raw -h "$primary_host" -p "$primary_port" INFO replication \
  > "$run_dir/primary-info-replication.txt"
redis-cli --raw -h "$replica_host" -p "$replica_port" INFO replication \
  > "$run_dir/replica-info-replication.txt"
redis-cli --raw -h "$primary_host" -p "$primary_port" INFO memory \
  > "$run_dir/primary-info-memory.txt"
redis-cli --raw -h "$replica_host" -p "$replica_port" INFO memory \
  > "$run_dir/replica-info-memory.txt"
if command -v rpk >/dev/null 2>&1; then
  rpk -X brokers="$kafka_brokers" topic describe "$kafka_topic" -p \
    > "$run_dir/kafka-partition.txt" || true
fi
journalctl -u "$primary_service" -u "$replica_service" \
  --since "@$(( $(date +%s) - timeout_seconds - 300 ))" --no-pager \
  > "$run_dir/journal.txt" || true

cat "$run_dir/primary-info-replication.txt"
cat "$run_dir/replica-info-replication.txt"
cat "$run_dir/kafka-partition.txt" 2>/dev/null || true
echo "RESULT=PASS"
