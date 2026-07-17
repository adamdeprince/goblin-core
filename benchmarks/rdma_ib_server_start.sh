#!/usr/bin/env bash
# Start goblin-core RDMA listener (meant to run ON thunder).
set -euo pipefail

ROOT="${REMOTE_ROOT:-$HOME/dev/packrat}"
BUILD="${BUILD_DIR:-$ROOT/build-rdma}"
GOBLIN="${BUILD}/goblin-core"
IB_IP="${SERVER_IB_IP:-10.88.88.3}"
PORT="${RDMA_PORT:-6380}"
SIZE="${RING_SIZE:-64kb}"
CPU="${SERVER_CPU:-5}"
LOG="${SERVER_LOG:-/tmp/goblin-rdma-server.log}"
PIDFILE="${SERVER_PIDFILE:-/tmp/goblin-rdma-server.pid}"

if [[ ! -x "$GOBLIN" ]]; then
  echo "missing $GOBLIN" >&2
  exit 1
fi

if [[ -f "$PIDFILE" ]]; then
  old=$(cat "$PIDFILE" || true)
  if [[ -n "${old:-}" ]] && kill -0 "$old" 2>/dev/null; then
    kill "$old" 2>/dev/null || true
    sleep 0.5
  fi
  rm -f "$PIDFILE"
fi
pkill -f "${GOBLIN}.*--rdma" 2>/dev/null || true
sleep 0.3

# Detach fully so the SSH session can exit without killing the server.
setsid taskset -c "$CPU" "$GOBLIN" \
  --cpu "$CPU" \
  --rdma "$IB_IP" "$PORT" "$SIZE" \
  </dev/null >"$LOG" 2>&1 &
echo $! >"$PIDFILE"
sleep 1
if ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
  echo "server failed to start; log:" >&2
  cat "$LOG" >&2 || true
  exit 1
fi
echo "started pid=$(cat "$PIDFILE") log=$LOG"
grep -E 'RDMA|listening|error|fail' "$LOG" || true
