#!/usr/bin/env bash
#
# Feed a payload to goblin + each incumbent over a UNIX DOMAIN SOCKET, ALL SERVERS
# IN PARALLEL, via redis-cli. For each server it:
#   1. launches the server listening ONLY on a UDS,
#   2. runs `redis-cli -s <sock> < $PAYLOAD` and TIMES just that call,
#   3. filters redis-cli's output to lines matching $FILTER into <name>.output,
#   4. records the server's OS process RSS after the load,
#   5. shuts the server down.
# All of that happens concurrently, one background job per server.
#
# It only starts/stops servers and reads a payload -- nothing destructive.
# Edit the paths/flags below and run it ON naamah:  bash feed_payload_uds.sh
#
# PARALLEL COST: every server reads the FULL payload at the same time and holds
# its own copy in RAM. ~/payload-128.txt is ~187 GB, so 5 servers means ~5x the
# disk reads and RAM at once (fine on a 1 TB / 64-core box; trim SERVERS to run
# fewer). Normal (reply-per-command) mode is required because `redis-cli --pipe`
# is faster but discards replies, so your used_memory_rss:/20..- lines would
# never appear. (Needs GNU date's %N, i.e. Linux -- fine on naamah.)
#
# GOBLIN + used_memory_rss: goblin has no INFO, so an `INFO memory` in the payload
# errors on goblin and goblin.output has NO `used_memory_rss:` line -- use the
# os_rss column. The incumbents' used_memory_rss: comes from your payload's INFO;
# os_rss is the OS-measured cross-check for all of them.

set -u

# ------------------------------- config -------------------------------------
PAYLOAD="${PAYLOAD:-$HOME/payload-128.txt}"
REDIS_CLI="${REDIS_CLI:-$HOME/bench/redis-8.8.0/src/redis-cli}"
OUTDIR="${OUTDIR:-$PWD}"
FILTER='^(used_memory_rss:|20..-)'

# A clobber-safe copy outside the build tree, so a goblin rebuild during the run
# can't replace it mid-benchmark. Refresh it with: cp build-rel/goblin-core \
#   ~/goblin-bench/goblin-core  (or set GOBLIN= to point elsewhere).
GOBLIN="${GOBLIN:-$HOME/goblin-bench/goblin-core}"
REDIS88="${REDIS88:-$HOME/bench/redis-8.8.0/src/redis-server}"
REDIS72="${REDIS72:-$HOME/bench/redis-7.2.4/src/redis-server}"
VALKEY="${VALKEY:-$HOME/bench/valkey-9.1.0/src/valkey-server}"
DRAGONFLY="${DRAGONFLY:-/usr/local/bin/dragonfly}"

# Servers to run, in order (all in parallel). Names become <name>.output.
# Override with e.g. SERVERS="goblin redis-8.8" to run a subset.
read -ra SERVERS <<< "${SERVERS:-goblin redis-8.8 redis-7.2.4 valkey-9.1 dragonfly}"

# Per-engine launch command -> UDS only (edit flags here). A bash array keeps
# quoting exact (e.g. --save '' stays an empty arg, no eval). redis/valkey use
# --port 0 so parallel instances never collide on TCP; only dragonfly needs a
# real (unused) port.
launch_cmd() {  # $1=name  $2=socket  -> fills global CMD[]
  local sock="$2"
  case "$1" in
    goblin)      CMD=("$GOBLIN" --unixsocket "$sock") ;;
    redis-8.8)   CMD=("$REDIS88" --unixsocket "$sock" --port 0 --save '' --appendonly no --dir /tmp) ;;
    redis-7.2.4) CMD=("$REDIS72" --unixsocket "$sock" --port 0 --save '' --appendonly no --dir /tmp) ;;
    valkey-9.1)  CMD=("$VALKEY"  --unixsocket "$sock" --port 0 --save '' --appendonly no --dir /tmp) ;;
    dragonfly)   CMD=("$DRAGONFLY" "--unixsocket=$sock" --port 16399 --proactor_threads=1 --maxmemory=0 --dir /tmp) ;;
    *) return 1 ;;
  esac
}

# Everything for ONE server; writes its formatted result row to $RESULT_DIR/$name.
run_one() {  # $1=name
  local name="$1"
  local sock="/tmp/uds-load-$name.sock"
  local log="/tmp/uds-load-$name.log"
  rm -f "$sock"

  if ! launch_cmd "$name" "$sock"; then
    printf '%-14s %13s %12s   %s\n' "$name" "-" "-" "unknown server" > "$RESULT_DIR/$name"
    return
  fi
  "${CMD[@]}" > "$log" 2>&1 &
  local pid=$!

  local ready=0 _
  for _ in $(seq 1 300); do
    if [ -S "$sock" ] && [ "$("$REDIS_CLI" -s "$sock" ping 2>/dev/null)" = "PONG" ]; then
      ready=1; break
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if [ "$ready" != 1 ]; then
    printf '%-14s %13s %12s   %s\n' "$name" "FAILED" "-" "start failed, see $log" > "$RESULT_DIR/$name"
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$sock"
    return
  fi

  # Feed the payload, streaming redis-cli's replies straight through the filter;
  # time ONLY the redis-cli call.
  local start end elapsed rss_kb rss_mb
  start=$(date +%s.%N)
  "$REDIS_CLI" -s "$sock" < "$PAYLOAD" 2>&1 | grep -E "$FILTER" > "$OUTDIR/$name.output"
  end=$(date +%s.%N)

  rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
  rss_mb=$(awk "BEGIN{printf \"%.1f\", ${rss_kb:-0}/1024}")
  elapsed=$(awk "BEGIN{printf \"%.3f\", $end - $start}")
  printf '%-14s %13s %12s   %s\n' "$name" "$elapsed" "$rss_mb" "$OUTDIR/$name.output" > "$RESULT_DIR/$name"

  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$sock"
}

# --------------------------------- run --------------------------------------
[ -f "$PAYLOAD" ] || { echo "payload not found: $PAYLOAD" >&2; exit 1; }
[ -x "$REDIS_CLI" ] || { echo "redis-cli not found: $REDIS_CLI" >&2; exit 1; }
mkdir -p "$OUTDIR"
RESULT_DIR="$(mktemp -d)"

echo "feeding $PAYLOAD to ${#SERVERS[@]} servers in parallel over UDS ..." >&2
for name in "${SERVERS[@]}"; do
  run_one "$name" &
done
wait

printf '%-14s %13s %12s   %s\n' "server" "redis-cli(s)" "os_rss(MB)" "output"
printf '%-14s %13s %12s   %s\n' "------" "------------" "----------" "------"
for name in "${SERVERS[@]}"; do
  cat "$RESULT_DIR/$name" 2>/dev/null
done
rm -rf "$RESULT_DIR"
