#!/usr/bin/env bash
set -euo pipefail

service=${GOBLIN_SERVICE:-goblin-core.service}
worker=${RECOVERY_WORKER:-/mnt/local/goblin-core-recovery-test/goblin_core_hset_kafka_recovery}
snapshot=${GOBLIN_SNAPSHOT:-/mnt/local/goblin-core/state/goblin.snapshot}
result_root=${RECOVERY_RESULT_ROOT:-/mnt/local/goblin-core-recovery-test/results}
host=${GOBLIN_HOST:-127.0.0.1}
port=${GOBLIN_PORT:-6379}
count=${RECOVERY_COUNT:-1000000}
snapshot_at=${RECOVERY_SNAPSHOT_AT:-500000}
pipeline=${RECOVERY_PIPELINE:-512}
wait_seconds=${RECOVERY_WAIT_SECONDS:-300}

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
run_dir="$result_root/$timestamp"
mkdir -p "$run_dir"
exec > >(tee "$run_dir/run.log") 2>&1

echo "run_dir=$run_dir"
echo "service=$service"
echo "worker=$worker"
echo "snapshot=$snapshot"
echo "count=$count"
echo "snapshot_at=$snapshot_at"
echo "pipeline=$pipeline"
echo "post_restart_wait_seconds=$wait_seconds"

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
snapshot_mtime_before=missing
if [[ -e "$snapshot" ]]; then
  snapshot_inode_before=$(stat --printf='%i' "$snapshot")
  snapshot_mtime_before=$(stat --printf='%y' "$snapshot")
fi
echo "snapshot_inode_before=$snapshot_inode_before"
echo "snapshot_mtime_before=$snapshot_mtime_before"

"$worker" --action populate --host "$host" --port "$port" --key foo \
  --count "$count" --snapshot-at "$snapshot_at" --pipeline "$pipeline" \
  --snapshot "$snapshot"

# This is deliberately the only grace period between the final write and SIGKILL.
sleep 1

snapshot_inode_at_crash=missing
snapshot_mtime_at_crash=missing
if [[ -e "$snapshot" ]]; then
  snapshot_inode_at_crash=$(stat --printf='%i' "$snapshot")
  snapshot_mtime_at_crash=$(stat --printf='%y' "$snapshot")
fi
echo "snapshot_inode_at_crash=$snapshot_inode_at_crash"
echo "snapshot_mtime_at_crash=$snapshot_mtime_at_crash"
if [[ "$snapshot_inode_before" != missing &&
      "$snapshot_inode_before" == "$snapshot_inode_at_crash" ]]; then
  echo "WARNING: background save had not replaced the prior snapshot before SIGKILL"
fi

echo "sending_sigkill_to_main_pid=$old_pid"
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

echo "restarted_by=systemd"
echo "waiting_after_restart_seconds=$wait_seconds"
sleep "$wait_seconds"

"$worker" --action verify --host "$host" --port "$port" --key foo \
  --count "$count" --pipeline "$pipeline"

if command -v redis-cli >/dev/null 2>&1; then
  redis-cli -h "$host" -p "$port" INFO replication > "$run_dir/info-replication.txt"
  cat "$run_dir/info-replication.txt"
fi
if command -v rpk >/dev/null 2>&1; then
  rpk -X brokers=192.168.1.49:9092 topic describe \
    goblin-core-replication -p > "$run_dir/kafka-partition.txt" || true
  cat "$run_dir/kafka-partition.txt" || true
fi
journalctl -u "$service" --since "@$(( $(date +%s) - wait_seconds - 120 ))" \
  --no-pager > "$run_dir/journal.txt" || true

if [[ "$argument_failure" != 0 ]]; then
  echo "FAIL: systemd recovery changed the Goblin command line" >&2
  exit 1
fi
echo "RESULT=PASS"
