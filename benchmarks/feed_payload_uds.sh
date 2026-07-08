#!/usr/bin/env bash
#
# Feed a payload to goblin + each incumbent over a UNIX DOMAIN SOCKET, one server
# at a time, with redis-cli. For each server it:
#   1. launches the server listening ONLY on a UDS,
#   2. runs `redis-cli -s <sock> < $PAYLOAD` and TIMES just that call,
#   3. filters redis-cli's output to lines matching $FILTER into <name>.output,
#   4. records the server's OS process RSS after the load,
#   5. shuts the server down and moves on.
#
# It only starts/stops servers and reads a payload -- nothing destructive.
# Edit the paths/flags below and run it ON naamah:  bash feed_payload_uds.sh
#
# It feeds the FULL payload to EACH engine, one after another, so total time is
# ~5x a single load. ~/payload-128.txt is ~187 GB, so `redis-cli < payload` in
# normal (reply-per-command) mode will take a long time per engine -- that is the
# number you're timing. To try one engine first, trim the SERVERS list. Normal
# mode is required here because `redis-cli --pipe` is far faster but discards
# replies, so your `used_memory_rss:` / `20..-` lines would never appear.
#
# NOTE ON GOBLIN + used_memory_rss: goblin does not implement INFO, so an
# `INFO memory` in your payload returns an error on goblin and goblin.output will
# have NO `used_memory_rss:` line (only any `20..-` lines). Use the os_rss column
# in the summary for goblin. The incumbents' `used_memory_rss:` still comes from
# your payload's INFO; os_rss is the OS-measured cross-check for all of them.
# (Requires GNU date's %N, i.e. Linux -- fine on naamah.)

set -u

# ------------------------------- config -------------------------------------
PAYLOAD="${PAYLOAD:-$HOME/payload-128.txt}"
REDIS_CLI="${REDIS_CLI:-$HOME/bench/redis-8.8.0/src/redis-cli}"
OUTDIR="${OUTDIR:-$PWD}"
FILTER='^(used_memory_rss:|20..-)'

GOBLIN="${GOBLIN:-$HOME/dev/packrat/build-rel/goblin-core}"
REDIS88="${REDIS88:-$HOME/bench/redis-8.8.0/src/redis-server}"
REDIS72="${REDIS72:-$HOME/bench/redis-7.2.4/src/redis-server}"
VALKEY="${VALKEY:-$HOME/bench/valkey-9.1.0/src/valkey-server}"
DRAGONFLY="${DRAGONFLY:-/usr/local/bin/dragonfly}"

# Servers to run, in order. Names become <name>.output.
SERVERS=(goblin redis-8.8 redis-7.2.4 valkey-9.1 dragonfly)

# Per-engine launch command -> UDS only (edit flags here). Uses a bash array so
# quoting is exact (e.g. --save '' stays an empty arg, no eval).
launch_cmd() {  # $1=name  $2=socket  -> fills global CMD[]
  local sock="$2"
  case "$1" in
    goblin)      CMD=("$GOBLIN" --unixsocket "$sock") ;;
    redis-8.8)   CMD=("$REDIS88" --unixsocket "$sock" --port 0 --save '' --appendonly no --dir /tmp) ;;
    redis-7.2.4) CMD=("$REDIS72" --unixsocket "$sock" --port 0 --save '' --appendonly no --dir /tmp) ;;
    valkey-9.1)  CMD=("$VALKEY"  --unixsocket "$sock" --port 0 --save '' --appendonly no --dir /tmp) ;;
    # dragonfly needs a TCP port even in UDS mode; 16399 is just a parking spot.
    dragonfly)   CMD=("$DRAGONFLY" "--unixsocket=$sock" --port 16399 --proactor_threads=1 --maxmemory=0 --dir /tmp) ;;
    *) echo "unknown server: $1" >&2; return 1 ;;
  esac
}

# --------------------------------- run --------------------------------------
[ -f "$PAYLOAD" ] || { echo "payload not found: $PAYLOAD" >&2; exit 1; }
[ -x "$REDIS_CLI" ] || { echo "redis-cli not found: $REDIS_CLI" >&2; exit 1; }
mkdir -p "$OUTDIR"

printf '%-14s %13s %12s   %s\n' "server" "redis-cli(s)" "os_rss(MB)" "output"
printf '%-14s %13s %12s   %s\n' "------" "------------" "----------" "------"

for name in "${SERVERS[@]}"; do
  sock="/tmp/uds-load-$name.sock"
  log="/tmp/uds-load-$name.log"
  rm -f "$sock"

  if ! launch_cmd "$name" "$sock"; then continue; fi
  "${CMD[@]}" > "$log" 2>&1 &
  pid=$!

  # Wait until the socket answers PING (up to ~10s), else report and skip.
  ready=0
  for _ in $(seq 1 100); do
    if [ -S "$sock" ] && [ "$("$REDIS_CLI" -s "$sock" ping 2>/dev/null)" = "PONG" ]; then
      ready=1; break
    fi
    kill -0 "$pid" 2>/dev/null || break   # server died during startup
    sleep 0.1
  done
  if [ "$ready" != 1 ]; then
    echo "$name: FAILED to start (see $log)" >&2
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$sock"
    continue
  fi

  # Feed the payload, streaming redis-cli's replies straight through the filter
  # so nothing multi-GB is buffered even if the payload emits a reply per command
  # (payload-128.txt is ~187 GB). Timing spans redis-cli + the trivial grep.
  start=$(date +%s.%N)
  "$REDIS_CLI" -s "$sock" < "$PAYLOAD" 2>&1 | grep -E "$FILTER" > "$OUTDIR/$name.output"
  end=$(date +%s.%N)

  rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
  rss_mb=$(awk "BEGIN{printf \"%.1f\", ${rss_kb:-0}/1024}")
  elapsed=$(awk "BEGIN{printf \"%.3f\", $end - $start}")
  printf '%-14s %13s %12s   %s\n' "$name" "$elapsed" "$rss_mb" "$OUTDIR/$name.output"

  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
  rm -f "$sock"
done
