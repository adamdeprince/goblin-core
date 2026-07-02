#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${PYTHON:-python3}"

REDIS_SERVER="${REDIS_SERVER:-}"
if [[ -z "$REDIS_SERVER" ]]; then
  if command -v redis-server >/dev/null 2>&1; then
    REDIS_SERVER="$(command -v redis-server)"
  elif [[ -x /opt/homebrew/bin/redis-server ]]; then
    REDIS_SERVER="/opt/homebrew/bin/redis-server"
  else
    echo "redis-server not found; set REDIS_SERVER=/path/to/redis-server" >&2
    exit 2
  fi
fi

NAME="${BENCHMARK_NAME:-ci-smoke}"
OUT_DIR="${BENCHMARK_OUTPUT_DIR:-$ROOT/benchmark-results}"
REPORT="${BENCHMARK_REPORT:-$OUT_DIR/ci-smoke.md}"
MEMBERS="${BENCHMARK_MEMBERS:-10000}"
OPS="${BENCHMARK_OPS:-10000}"
REMOVE_MEMBERS="${BENCHMARK_REMOVE_MEMBERS:-5000}"
RANGE_SIZE="${BENCHMARK_RANGE_SIZE:-16}"
PIPELINE="${BENCHMARK_PIPELINE:-256}"
ZADD_BATCH="${BENCHMARK_ZADD_BATCH:-128}"
ZREM_BATCH="${BENCHMARK_ZREM_BATCH:-128}"
LATENCY_SAMPLES="${BENCHMARK_LATENCY_SAMPLES:-100}"
LATENCY_WARMUP="${BENCHMARK_LATENCY_WARMUP:-10}"
SKIP_BUILD="${SKIP_BUILD:-0}"
BUILD_HTML="${BUILD_HTML:-1}"

run_args=()
if [[ "$SKIP_BUILD" == "1" || "$SKIP_BUILD" == "true" ]]; then
  run_args+=(--skip-build)
fi

"$PYTHON" "$ROOT/benchmarks/run_benchmarks.py" \
  "${run_args[@]}" \
  --redis-server "$REDIS_SERVER" \
  --name "$NAME" \
  --output-dir "$OUT_DIR" \
  --report "$REPORT" \
  --members "$MEMBERS" \
  --ops "$OPS" \
  --remove-members "$REMOVE_MEMBERS" \
  --range-size "$RANGE_SIZE" \
  --pipeline "$PIPELINE" \
  --zadd-batch "$ZADD_BATCH" \
  --zrem-batch "$ZREM_BATCH" \
  --latency-samples "$LATENCY_SAMPLES" \
  --latency-warmup "$LATENCY_WARMUP" \
  --rank-cache-modes off exact block-hint \
  --skip-supplemental

if [[ "$BUILD_HTML" == "1" || "$BUILD_HTML" == "true" ]]; then
  "$PYTHON" "$ROOT/scripts/build_html_docs.py" \
    --output "$ROOT/html" \
    "$ROOT/README.md" \
    "$ROOT/BENCHMARKS.md" \
    "$ROOT/MICROBENCHMARKS.md" \
    "$ROOT/RELEASE.md"
fi

echo "wrote $REPORT"
