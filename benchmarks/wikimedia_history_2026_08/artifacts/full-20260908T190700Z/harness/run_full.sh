#!/usr/bin/env bash
# Run in a detached tmux session after preparing an isolated, read-only input.
# No automatic restart: a new attempt must always use a fresh project directory.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?full benchmark project with bin, harness and preflight}"
export OUTDIR="$PROJECT_DIR/run"
export PAYLOAD="$PROJECT_DIR/redis.cmds"
export GOBLIN="$PROJECT_DIR/bin/goblin-core"
export HEAD_LINES=all SAMPLE_SECONDS=300 VERIFY=1 DIGEST_PAGE_SIZE=65536
export HASH_INPUT=0  # The complete frozen input was already hashed in preflight.
export EXPECTED_FULL_COMMANDS="$(<"$PROJECT_DIR/preflight/commands.txt")"
export SERVERS="goblin-standard goblin-packed-int32-float32 redis-8.8 redis-7.2.4 valkey-9.1 dragonfly"
unset REFERENCE_DIGEST  # The saved short-run digest is not a full-run reference.

test -s "$PROJECT_DIR/preflight/input.sha256"
test -f "$PAYLOAD" && test -x "$GOBLIN"
test ! -e "$OUTDIR" && test ! -e "$PROJECT_DIR/controller.log"
[[ "$EXPECTED_FULL_COMMANDS" =~ ^[1-9][0-9]*$ ]]
set -o noclobber
exec > "$PROJECT_DIR/controller.log" 2>&1
printf '%s\n' "$$" > "$PROJECT_DIR/controller.pid"
date -u +%Y-%m-%dT%H:%M:%SZ > "$PROJECT_DIR/started.utc"
touch "$PROJECT_DIR/JOB-RUNNING"

harness_pid=""
on_exit() {
  local status="$?"
  trap - EXIT
  # On interruption, terminate only this run's recorded harness. Its trap
  # handles its own server/sampler cleanup; tmux SIGINT also reaches the feeds.
  if [ -n "$harness_pid" ] && kill -0 "$harness_pid" 2>/dev/null; then
    kill -TERM "$harness_pid" 2>/dev/null || true
    wait "$harness_pid" 2>/dev/null || true
  fi
  printf '%s\n' "$status" > "$PROJECT_DIR/exit.status"
  date -u +%Y-%m-%dT%H:%M:%SZ > "$PROJECT_DIR/finished.utc"
  if [ "$status" = 0 ]; then
    mv "$PROJECT_DIR/JOB-RUNNING" "$PROJECT_DIR/JOB-PASS"
  else
    mv "$PROJECT_DIR/JOB-RUNNING" "$PROJECT_DIR/JOB-FAILED"
  fi
  exit "$status"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# The original timed feed path is unchanged. Run in the background only so the
# controller can attach the preflight fingerprint as soon as OUTDIR exists.
bash "$PROJECT_DIR/harness/run.sh" &
harness_pid=$!
printf '%s\n' "$harness_pid" > "$PROJECT_DIR/harness.pid"
while [ ! -d "$OUTDIR" ]; do
  kill -0 "$harness_pid" 2>/dev/null || { wait "$harness_pid"; exit 1; }
  sleep 0.1
done
cp "$PROJECT_DIR/preflight/input.sha256" "$OUTDIR/input.sha256"
cp "$PROJECT_DIR/preflight/incumbents.sha256" "$OUTDIR/incumbents.sha256"
set +e
wait "$harness_pid"
run_status=$?
set -e
harness_pid=""

# This extra full-file read occurs after timing and final-state verification.
sha256sum "$PAYLOAD" > "$PROJECT_DIR/input-after.sha256"
cmp "$PROJECT_DIR/preflight/input.sha256" "$PROJECT_DIR/input-after.sha256"
exit "$run_status"
