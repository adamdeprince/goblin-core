#!/usr/bin/env bash
set -euo pipefail

service=${GOBLIN_SERVICE:-goblin-core.service}
worker=${RECOVERY_WORKER:-/mnt/local/goblin-core-recovery-test/goblin_core_external_logging_recovery}
snapshot=${GOBLIN_SNAPSHOT:-/mnt/local/goblin-core/state/goblin.snapshot}
result_root=${RECOVERY_RESULT_ROOT:-/mnt/local/goblin-core-recovery-test/results}
host=${GOBLIN_HOST:-127.0.0.1}
port=${GOBLIN_PORT:-6379}
count=${RECOVERY_COUNT:-1000000}
snapshot_at=${RECOVERY_SNAPSHOT_AT:-500000}
pipeline=${RECOVERY_PIPELINE:-512}
recovery_timeout=${RECOVERY_TIMEOUT_SECONDS:-300}
snapshot_timeout=${SNAPSHOT_TIMEOUT_SECONDS:-300}
data_type=${RECOVERY_DATA_TYPE:-all}
key_prefix=${RECOVERY_KEY_PREFIX:-external-logging-test}
kafka_brokers=${KAFKA_BROKERS:-192.168.1.49:9092}
kafka_topic=${KAFKA_TOPIC:-goblin-core-replication}

usage() {
  cat <<EOF
usage: $0 [options]
  --data-type string|hash|set|zset|list|array|all
  --count N                  final entries per type (default: $count)
  --snapshot-at N            entries per type at snapshot (default: $snapshot_at)
  --pipeline N               RESP pipeline depth (default: $pipeline)
  --key-prefix PREFIX        test key namespace (default: $key_prefix)
  --snapshot PATH            Goblin snapshot path (default: $snapshot)
  --service UNIT             systemd unit (default: $service)
  --worker PATH              native worker executable (default: $worker)
  --host HOST --port PORT    Goblin RESP endpoint
  --recovery-timeout N       listener recovery timeout in seconds
  --snapshot-timeout N       background-save timeout in seconds
  --result-root PATH         result directory parent
  --kafka-brokers HOSTS      rpk evidence endpoint
  --kafka-topic TOPIC        rpk evidence topic
EOF
}

while (($# != 0)); do
  case "$1" in
    --data-type) data_type=$2; shift 2 ;;
    --count) count=$2; shift 2 ;;
    --snapshot-at) snapshot_at=$2; shift 2 ;;
    --pipeline) pipeline=$2; shift 2 ;;
    --key-prefix) key_prefix=$2; shift 2 ;;
    --snapshot) snapshot=$2; shift 2 ;;
    --service) service=$2; shift 2 ;;
    --worker) worker=$2; shift 2 ;;
    --host) host=$2; shift 2 ;;
    --port) port=$2; shift 2 ;;
    --recovery-timeout) recovery_timeout=$2; shift 2 ;;
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
for value in "$count" "$snapshot_at" "$pipeline" "$recovery_timeout" "$snapshot_timeout"; do
  if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
    echo "counts and timeouts must be positive integers" >&2
    exit 2
  fi
done
if ((snapshot_at > count)); then
  echo "--snapshot-at must not exceed --count" >&2
  exit 2
fi

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
run_dir="$result_root/${timestamp}-${data_type}"
mkdir -p "$run_dir"
exec > >(tee "$run_dir/run.log") 2>&1

echo "run_dir=$run_dir"
echo "service=$service"
echo "worker=$worker"
echo "snapshot=$snapshot"
echo "data_type=$data_type"
echo "key_prefix=$key_prefix"
echo "count=$count"
echo "snapshot_at=$snapshot_at"
echo "pipeline=$pipeline"
echo "recovery_timeout_seconds=$recovery_timeout"

test -x "$worker"
test "$(systemctl is-active "$service")" = active
restart_policy=$(systemctl show "$service" --property=Restart --value)
if [[ "$restart_policy" == no ]]; then
  echo "FAIL: $service has no automatic restart policy" >&2
  exit 1
fi
echo "restart_policy=$restart_policy"

systemctl show "$service" --property=ExecStart --value > "$run_dir/execstart.before"
systemctl cat "$service" --no-pager > "$run_dir/unit.before"
old_pid=$(systemctl show "$service" --property=MainPID --value)
if [[ ! "$old_pid" =~ ^[1-9][0-9]*$ ]]; then
  echo "FAIL: invalid MainPID before crash: $old_pid" >&2
  exit 1
fi
cp "/proc/$old_pid/cmdline" "$run_dir/cmdline.before"
echo "old_pid=$old_pid"
echo "execstart=$(<"$run_dir/execstart.before")"
echo "cmdline_sha256_before=$(sha256sum "$run_dir/cmdline.before" | awk '{print $1}')"

snapshot_inode_before=missing
if [[ -e "$snapshot" ]]; then
  snapshot_inode_before=$(stat --printf='%i' "$snapshot")
fi
echo "snapshot_inode_before=$snapshot_inode_before"

common=(--host "$host" --port "$port" --data-type "$data_type"
        --key-prefix "$key_prefix" --count "$count"
        --snapshot-at "$snapshot_at" --pipeline "$pipeline")
"$worker" --action prepare "${common[@]}" --snapshot "$snapshot"

snapshot_deadline=$((SECONDS + snapshot_timeout))
while true; do
  snapshot_inode_at_crash=missing
  if [[ -e "$snapshot" ]]; then
    snapshot_inode_at_crash=$(stat --printf='%i' "$snapshot")
  fi
  if [[ "$snapshot_inode_at_crash" != missing &&
        "$snapshot_inode_at_crash" != "$snapshot_inode_before" ]]; then
    break
  fi
  if ((SECONDS >= snapshot_deadline)); then
    echo "FAIL: background snapshot did not replace $snapshot" >&2
    exit 1
  fi
  sleep 0.1
done
echo "snapshot_inode_at_crash=$snapshot_inode_at_crash"
echo "snapshot_bytes=$(stat --printf='%s' "$snapshot")"
echo "snapshot_mtime=$(stat --printf='%y' "$snapshot")"

echo "sending_sigkill_to_main_pid=$old_pid"
crash_started_ns=$(date +%s%N)
sudo kill -KILL "$old_pid"

new_pid=0
for _ in $(seq 1 600); do
  candidate=$(systemctl show "$service" --property=MainPID --value)
  if [[ "$candidate" =~ ^[1-9][0-9]*$ && "$candidate" != "$old_pid" &&
        -r "/proc/$candidate/cmdline" ]]; then
    new_pid=$candidate
    break
  fi
  sleep 0.1
done
if [[ "$new_pid" == 0 ]]; then
  echo "FAIL: systemd did not recreate $service within 60 seconds" >&2
  systemctl status "$service" --no-pager --full || true
  exit 1
fi
echo "new_pid=$new_pid"

systemctl show "$service" --property=ExecStart --value > "$run_dir/execstart.after"
systemctl cat "$service" --no-pager > "$run_dir/unit.after"
cp "/proc/$new_pid/cmdline" "$run_dir/cmdline.after"
echo "cmdline_sha256_after=$(sha256sum "$run_dir/cmdline.after" | awk '{print $1}')"

argument_failure=0
if cmp -s "$run_dir/unit.before" "$run_dir/unit.after"; then
  echo "unit_definition_identical=yes"
else
  echo "unit_definition_identical=no"
  argument_failure=1
fi
if cmp -s "$run_dir/cmdline.before" "$run_dir/cmdline.after"; then
  echo "process_cmdline_identical=yes"
else
  echo "process_cmdline_identical=no"
  argument_failure=1
fi

ready_deadline=$((SECONDS + recovery_timeout))
while ! "$worker" --action probe "${common[@]}" >/dev/null 2>&1; do
  if ((SECONDS >= ready_deadline)); then
    echo "FAIL: Goblin listener did not become ready within $recovery_timeout seconds" >&2
    journalctl -u "$service" --no-pager -n 100 || true
    exit 1
  fi
  sleep 0.1
done
ready_ns=$(date +%s%N)
recovery_seconds=$(awk -v start="$crash_started_ns" -v end="$ready_ns" \
  'BEGIN { printf "%.6f", (end - start) / 1000000000 }')
echo "restarted_by=systemd"
echo "recovery_to_listener_seconds=$recovery_seconds"

"$worker" --action continue "${common[@]}"
"$worker" --action verify "${common[@]}"

if command -v redis-cli >/dev/null 2>&1; then
  redis-cli -h "$host" -p "$port" INFO replication > "$run_dir/info-replication.txt"
  redis-cli -h "$host" -p "$port" INFO memory > "$run_dir/info-memory.txt"
  cat "$run_dir/info-replication.txt"
  cat "$run_dir/info-memory.txt"
fi
if command -v rpk >/dev/null 2>&1; then
  rpk -X brokers="$kafka_brokers" topic describe "$kafka_topic" -p \
    > "$run_dir/kafka-partition.txt" || true
  cat "$run_dir/kafka-partition.txt" || true
fi
journalctl -u "$service" --since "@$(( $(date +%s) - recovery_timeout - 120 ))" \
  --no-pager > "$run_dir/journal.txt" || true

if [[ "$argument_failure" != 0 ]]; then
  echo "FAIL: systemd recovery changed the Goblin command line" >&2
  exit 1
fi
echo "RESULT=PASS"
