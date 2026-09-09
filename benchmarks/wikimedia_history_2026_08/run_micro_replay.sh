#!/usr/bin/env bash
# Alternate frozen builds on an immutable input; never overwrite prior runs.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
read -ra stages <<< "${STAGES:?space-separated frozen server stages}"
REFERENCE_DIGEST="${REFERENCE_DIGEST:?known-good packed result}"
PAYLOAD="${PAYLOAD:?frozen input file}"
export PAYLOAD
TRIALS="${TRIALS:-3}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
for stage in "${stages[@]}"; do
  stage_dir="$PROJECT_DIR/stages/$stage"
  test -x "$stage_dir/bin/goblin-core"
  test ! -e "$stage_dir/replay-summary.tsv"
  printf 'trial\tfeed_seconds\tos_rss_kb\tstatus\n' > "$stage_dir/replay-summary.tsv"
done
cp "$0" "$PROJECT_DIR/run_micro_replay.sh"
for trial in $(seq 1 "$TRIALS"); do
  for ((offset=0; offset<${#stages[@]}; ++offset)); do
    index=$(((offset + trial - 1) % ${#stages[@]}))
    stage_dir="$PROJECT_DIR/stages/${stages[$index]}"
    replay_dir="$stage_dir/replay-$trial"
    OUTDIR="$replay_dir" GOBLIN="$stage_dir/bin/goblin-core" \
      HEAD_LINES=2000000 HASH_INPUT=1 SAMPLE_SECONDS=10 VERIFY=1 \
      SERVERS=goblin-packed-int32-float32 \
      taskset -c 2-5 bash "$SCRIPT_DIR/run.sh"
    cmp "$REFERENCE_DIGEST" \
      "$replay_dir/digests/goblin-packed-int32-float32.json"
    awk -F '\t' -v trial="$trial" \
      '{printf "%s\t%s\t%s\t%s\n", trial, $3, $5, $2}' \
      "$replay_dir/results/goblin-packed-int32-float32.tsv" \
      >> "$stage_dir/replay-summary.tsv"
  done
done
for stage in "${stages[@]}"; do
  touch "$PROJECT_DIR/stages/$stage/REPLAYS-PASS"
done
