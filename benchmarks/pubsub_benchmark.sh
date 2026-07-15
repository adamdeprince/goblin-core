#!/usr/bin/env bash
# Run the native Pub/Sub matrix on naamah. The measured path is entirely C++;
# this script only builds the probe, supplies installed binary paths, and saves
# its CSV output.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-rel}"
PROBE="${PROBE:-$BUILD/goblin_core_pubsub_benchmark}"
GOBLIN="${GOBLIN:-$BUILD/goblin-core}"
REDIS72="${REDIS72:-$HOME/bench/redis-7.2.4/src/redis-server}"
REDIS88="${REDIS88:-$HOME/bench/redis-8.8.0/src/redis-server}"
VALKEY="${VALKEY:-$HOME/bench/valkey-9.1.0/src/valkey-server}"
DRAGONFLY="${DRAGONFLY:-/usr/local/bin/dragonfly}"
MINI_REDIS_GO="${MINI_REDIS_GO:-$HOME/bench/mini-redis-go/bin/mini-redis}"
PARITY="${PARITY:-$HERE/redis-parity.conf}"
RESULTS="${RESULTS:-$ROOT/benchmark-results/pubsub-$(date +%Y%m%d-%H%M%S).csv}"

cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DGOBLIN_CORE_ARCH=avx2 -DGOBLIN_CORE_BUILD_BENCHMARKS=ON >/dev/null
cmake --build "$BUILD" -j "${JOBS:-8}" \
  --target goblin_core_server goblin_core_pubsub_benchmark >/dev/null

for binary in "$GOBLIN" "$REDIS72" "$REDIS88" "$VALKEY" "$DRAGONFLY" \
              "$MINI_REDIS_GO"; do
  if [[ ! -x "$binary" ]]; then
    printf 'missing executable: %s\n' "$binary" >&2
    exit 1
  fi
done

mkdir -p "$(dirname "$RESULTS")"

"$PROBE" \
  --parity-config "$PARITY" \
  --engine "goblin-sbe-ring-4kb:goblin-ring:$GOBLIN" \
  --engine "goblin-resp-uds:goblin-uds:$GOBLIN" \
  --engine "redis-7.2.4:redis:$REDIS72" \
  --engine "redis-8.8:redis:$REDIS88" \
  --engine "valkey-9.1:redis:$VALKEY" \
  --engine "dragonfly:dragonfly:$DRAGONFLY" \
  --engine "mini-redis-go-55178df:mini:$MINI_REDIS_GO" \
  "$@" | tee "$RESULTS"

printf 'wrote %s\n' "$RESULTS" >&2
