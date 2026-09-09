#!/usr/bin/env bash
# Repeat one immutable packed build; never reuse a replay output directory.
set -euo pipefail

STAGE_DIR="${STAGE_DIR:?directory containing bin/goblin-core}"
REFERENCE_DIGEST="${REFERENCE_DIGEST:?known-good packed digest for this prefix}"
TRIALS="${TRIALS:-3}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
test -x "$STAGE_DIR/bin/goblin-core"
test -f "$REFERENCE_DIGEST"
test ! -e "$STAGE_DIR/replay-summary.tsv"
cp "$0" "$STAGE_DIR/run_merge_stage.sh"
sha256sum "$STAGE_DIR/bin/goblin-core" > "$STAGE_DIR/binary.sha256"
printf 'trial\tfeed_seconds\tos_rss_kb\tstatus\n' > "$STAGE_DIR/replay-summary.tsv"

for trial in $(seq 1 "$TRIALS"); do
  replay_dir="$STAGE_DIR/replay-$trial"
  OUTDIR="$replay_dir" GOBLIN="$STAGE_DIR/bin/goblin-core" \
    HEAD_LINES=2000000 HASH_INPUT=0 SAMPLE_SECONDS=10 VERIFY=1 \
    SERVERS=goblin-packed-int32-float32 \
    taskset -c 2-5 bash "$SCRIPT_DIR/run.sh"
  if ! cmp -s "$REFERENCE_DIGEST" \
      "$replay_dir/digests/goblin-packed-int32-float32.json"; then
    echo "final mapping/order differs from known-good prefix: $replay_dir" >&2
    exit 1
  fi
  awk -F '\t' -v trial="$trial" \
    '{printf "%s\t%s\t%s\t%s\n", trial, $3, $5, $2}' \
    "$replay_dir/results/goblin-packed-int32-float32.tsv" \
    >> "$STAGE_DIR/replay-summary.tsv"
done
touch "$STAGE_DIR/REPLAYS-PASS"
