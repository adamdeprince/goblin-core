#!/usr/bin/env bash
# Run after timed replays/builds; optionally rotate stages for confirmation.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
read -ra stages <<< "${STAGES:-00-baseline 01-hash 02-single-probe 03-shared-path}"
LABEL="${LABEL:-micro}"
TRIALS="${TRIALS:-3}"
for stage in "${stages[@]}"; do
  test ! -e "$PROJECT_DIR/stages/$stage/$LABEL"
  mkdir "$PROJECT_DIR/stages/$stage/$LABEL"
done
for trial in $(seq 0 "$TRIALS"); do
  for bench in packed-update packed-merge; do
    if [ "$bench" = packed-update ]; then
      shapes=(local increment random insert)
    else
      shapes=(local increment random churn rebalance)
    fi
    for shape in "${shapes[@]}"; do
      operations=200000
      [ "$bench" != packed-merge ] || operations=1000000
      [ "$shape" != insert ] || operations=50000
      for ((offset=0; offset<${#stages[@]}; ++offset)); do
        index=$(((offset + trial) % ${#stages[@]}))
        stage_dir="$PROJECT_DIR/stages/${stages[$index]}"
        taskset -c 2 "$stage_dir/bin/$bench" "$operations" "$shape" \
          > "$stage_dir/$LABEL/$bench-$shape-$trial.tsv"
      done
    done
  done
done
cp "$0" "$PROJECT_DIR/run_update_micro-$LABEL.sh"
