#!/usr/bin/env bash
# Unpipelined single-op round-trip latency shootout: Goblin Core's SBE/ring against
# RESP-over-Unix-socket on Goblin and every Redis-family incumbent. Run it ON naamah:
#
#   bash benchmarks/latency_shootout.sh
#
# It drives one C++ probe (benchmarks/latency_shootout.cpp) against every engine with the
# SAME rdtscp timing, so the only variable is (transport, protocol, server). Low cardinality
# by design -- the zset holds 10 members and the hash 10 fields, so every engine is on its
# compact/listpack small-collection path and there is almost no data-structure work per op;
# what remains is wire-path overhead, which is the combined SBE/ring story.
#
# Binary locations match feed_payload_uds.sh and are env-overridable. Refresh the Goblin
# copy first:  cp build-rel/goblin-core ~/goblin-bench/goblin-core
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # benchmarks/
ROOT="$(cd "$HERE/.." && pwd)"
PARITY="$HERE/redis-parity.conf"                              # activedefrag no, persistence off, io-threads 1

GOBLIN="${GOBLIN:-$HOME/goblin-bench/goblin-core}"
REDIS88="${REDIS88:-$HOME/bench/redis-8.8.0/src/redis-server}"
REDIS72="${REDIS72:-$HOME/bench/redis-7.2.4/src/redis-server}"
VALKEY="${VALKEY:-$HOME/bench/valkey-9.1.0/src/valkey-server}"
DRAGONFLY="${DRAGONFLY:-/usr/local/bin/dragonfly}"
REDIS_CLI="${REDIS_CLI:-$HOME/bench/redis-8.8.0/src/redis-cli}"
PROBE="${PROBE:-$ROOT/latency_shootout}"
SERVER_CORE="${SERVER_CORE:-2}"                              # matches the probe's client core 3 / ring server core 2

# Which engines to include (space-separated labels); override to run a subset.
ENGINES="${ENGINES:-goblin-sbe-ring goblin-resp-uds redis-7.2.4 redis-8.8 valkey-9.1 dragonfly}"

log() { printf '%s\n' "$*" >&2; }

# Compile the header-only probe if it is not already built.
if [ ! -x "$PROBE" ]; then
  log "compiling probe -> $PROBE"
  CXX="${CXX:-g++}"
  "$CXX" -std=c++23 -O3 -march=native -DNDEBUG -I"$ROOT/include" -I"$ROOT/sbe/generated" \
      "$HERE/latency_shootout.cpp" -o "$PROBE" || { log "probe compile failed"; exit 1; }
fi

TASKSET=""
if command -v taskset >/dev/null 2>&1; then TASKSET="taskset -c $SERVER_CORE"; fi

RESULTS="$(mktemp)"
trap 'rm -f "$RESULTS"' EXIT

# Launch <kind> <binary> on a private UDS, wait until it answers PING, probe it, kill it.
probe_uds() {
  local label="$1" kind="$2" bin="$3"
  local sock="/tmp/uds-lat-$label.sock"
  local slog="/tmp/uds-lat-$label.log"
  rm -f "$sock"
  if [ ! -x "$bin" ]; then log "  !! $label: binary not found at $bin -- skipping"; return 1; fi

  case "$kind" in
    goblin)    $TASKSET "$bin" --unixsocket "$sock" >"$slog" 2>&1 & ;;
    redis)     $TASKSET "$bin" "$PARITY" --unixsocket "$sock" --port 0 --dir /tmp >"$slog" 2>&1 & ;;
    dragonfly) "$bin" "--unixsocket=$sock" --port 16399 --proactor_threads=1 --maxmemory=0 --dir /tmp >"$slog" 2>&1 & ;;  # self-pins; ignores taskset
    *) log "  !! unknown kind $kind"; return 1 ;;
  esac
  local pid=$!

  local ok=0 i
  for i in $(seq 1 300); do
    if [ -S "$sock" ] && [ "$("$REDIS_CLI" -s "$sock" ping 2>/dev/null)" = "PONG" ]; then ok=1; break; fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if [ "$ok" != 1 ]; then log "  !! $label failed to become ready (see $slog)"; kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; return 1; fi

  "$PROBE" uds "$sock" "$label" >>"$RESULTS"
  kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; rm -f "$sock"
}

log "== latency shootout =="
if command -v lscpu >/dev/null 2>&1; then log "$(lscpu | grep -m1 'Model name' | sed 's/  */ /g')"; fi
log "cardinality: zset=10 members, hash=10 fields; each op is one synchronous round trip"
log ""

for label in $ENGINES; do
  case "$label" in
    goblin-sbe-ring)
      if [ ! -x "$GOBLIN" ]; then log "  !! goblin-sbe-ring: $GOBLIN not found -- skipping"; continue; fi
      log "[$label] SBE / shared-memory ring"
      "$PROBE" ring "$GOBLIN" "$label" >>"$RESULTS" ;;
    goblin-resp-uds) probe_uds "$label" goblin    "$GOBLIN"  ;;
    redis-7.2.4)     probe_uds "$label" redis      "$REDIS72" ;;
    redis-8.8)       probe_uds "$label" redis      "$REDIS88" ;;
    valkey-9.1)      probe_uds "$label" redis      "$VALKEY"  ;;
    dragonfly)       probe_uds "$label" dragonfly  "$DRAGONFLY" ;;
    *) log "  !! unknown engine label: $label" ;;
  esac
done

# ---- pivot the CSV into median + p99 tables ------------------------------------------------
awk -F, -v engines="$ENGINES" '
  $1=="LAT" { p50[$2","$3]=$4; p99[$2","$3]=$6 }
  END {
    ne=split(engines, E, " ")
    no=split("SET GET HSET HGET ZADD ZSCORE", O, " ")
    printf "\n== median round-trip latency (microseconds), cardinality 10 ==\n"
    printf "%-16s", "engine / op"; for(o=1;o<=no;o++) printf "%9s", O[o]; printf "\n"
    for(e=1;e<=ne;e++){ printf "%-16s", E[e]; for(o=1;o<=no;o++){ k=E[e]","O[o]; printf "%9s", (k in p50)?sprintf("%.3f",p50[k]):"-" } printf "\n" }
    printf "\n== p99 round-trip latency (microseconds), cardinality 10 ==\n"
    printf "%-16s", "engine / op"; for(o=1;o<=no;o++) printf "%9s", O[o]; printf "\n"
    for(e=1;e<=ne;e++){ printf "%-16s", E[e]; for(o=1;o<=no;o++){ k=E[e]","O[o]; printf "%9s", (k in p99)?sprintf("%.3f",p99[k]):"-" } printf "\n" }
  }
' "$RESULTS"

echo ""
echo "raw CSV (LAT,label,op,p50,p90,p99,min,mean,n us):"
grep '^LAT,' "$RESULTS" || true
