#!/usr/bin/env bash
#
# Feed the first HEAD_LINES of a payload to goblin + each incumbent over a UNIX
# DOMAIN SOCKET, ALL SERVERS IN PARALLEL, via redis-cli. For each server it:
#   1. launches the server listening ONLY on a UDS,
#   2. feeds `head -n HEAD_LINES $PAYLOAD` through redis-cli, TIMING just that call,
#   3. filters redis-cli's output to lines matching $FILTER into <name>.output,
#   4. records the server's OS process RSS after the load,
#   5. snapshots the $CMP_KEY sorted set, then shuts the server down.
# Afterwards it cross-checks goblin's sorted set against redis-8.8's (ZRANGE
# WITHSCORES) to confirm goblin produced identical data. One job per server.
#
# It only starts/stops servers and reads a payload -- nothing destructive.
# Edit the paths/flags below and run it ON naamah:  bash feed_payload_uds.sh
#
# HEAD_LINES defaults to 10000 (a quick run); the payload (~/payload-128.txt) is
# ~187 GB and head stops after N lines, so it never reads the whole file -- raise
# HEAD_LINES for a bigger load. Normal (reply-per-command) mode is required:
# `redis-cli --pipe` is faster but discards replies, so your used_memory_rss:/
# 20..- lines would never appear. (Needs GNU date's %N, i.e. Linux -- fine on naamah.)
#
# used_memory_rss: comes from each server's INFO (goblin implements INFO now, so
# its line is present); the os_rss column is the OS-measured RSS for all engines.

set -u

# ------------------------------- config -------------------------------------
PAYLOAD="${PAYLOAD:-$HOME/payload-128.txt}"
REDIS_CLI="${REDIS_CLI:-$HOME/bench/redis-8.8.0/src/redis-cli}"
OUTDIR="${OUTDIR:-$PWD}"
FILTER='^(used_memory_rss:|20..-)'
HEAD_LINES="${HEAD_LINES:-10000}"   # feed only the first N payload lines (quick run)
CMP_KEY="${CMP_KEY:-leaderboard}"   # zset cross-checked afterwards: goblin vs redis-8.8

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

  # Feed the first HEAD_LINES of the payload, streaming redis-cli's replies
  # straight through the filter; time ONLY the redis-cli call. (head stops after
  # N lines, so it never reads the whole 187 GB file.)
  local start end elapsed rss_kb rss_mb
  start=$(date +%s.%N)
  head -n "$HEAD_LINES" "$PAYLOAD" | "$REDIS_CLI" -s "$sock" 2>&1 | grep -E "$FILTER" > "$OUTDIR/$name.output"
  end=$(date +%s.%N)

  rss_kb=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
  rss_mb=$(awk "BEGIN{printf \"%.1f\", ${rss_kb:-0}/1024}")
  elapsed=$(awk "BEGIN{printf \"%.3f\", $end - $start}")
  printf '%-14s %13s %12s   %s\n' "$name" "$elapsed" "$rss_mb" "$OUTDIR/$name.output" > "$RESULT_DIR/$name"

  # Snapshot the sorted set (members in sorted order + scores) before shutdown,
  # for the goblin-vs-redis-8.8 correctness check after all servers finish.
  "$REDIS_CLI" -s "$sock" ZRANGE "$CMP_KEY" 0 -1 WITHSCORES > "$RESULT_DIR/$name.zset" 2>/dev/null

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

# Correctness: goblin's sorted set must match redis-8.8's exactly -- same members,
# same scores, same order (redis-cli formats both, so any diff is real data). A
# mismatch means goblin got the leaderboard wrong.
echo
gz="$RESULT_DIR/goblin.zset"; rz="$RESULT_DIR/redis-8.8.zset"
if [ -s "$gz" ] && [ -s "$rz" ]; then
  if diff -q "$gz" "$rz" >/dev/null; then
    echo "zset check: goblin '$CMP_KEY' MATCHES redis-8.8  ($(( $(wc -l < "$gz") / 2 )) members)"
  else
    echo "zset check: MISMATCH -- goblin '$CMP_KEY' differs from redis-8.8"
    echo "  goblin $(( $(wc -l < "$gz") / 2 )) members vs redis-8.8 $(( $(wc -l < "$rz") / 2 )); first diffs:"
    diff "$gz" "$rz" | head -20
  fi
else
  echo "zset check: skipped (need both goblin and redis-8.8 in SERVERS)"
fi
rm -rf "$RESULT_DIR"
