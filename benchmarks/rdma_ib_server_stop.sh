#!/usr/bin/env bash
# Stop goblin-core RDMA listener (meant to run ON thunder).
set -euo pipefail
PIDFILE="${SERVER_PIDFILE:-/tmp/goblin-rdma-server.pid}"
if [[ -f "$PIDFILE" ]]; then
  pid=$(cat "$PIDFILE" || true)
  if [[ -n "${pid:-}" ]]; then
    kill "$pid" 2>/dev/null || true
    sleep 0.3
    kill -9 "$pid" 2>/dev/null || true
  fi
  rm -f "$PIDFILE"
fi
pkill -f "goblin-core.*--rdma" 2>/dev/null || true
echo "stopped"
