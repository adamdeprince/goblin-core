#!/usr/bin/env bash
# Warmup then alternating/rotated trials; build and replay processes must be idle.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
read -ra stages <<< "${STAGES:?space-separated frozen stage names}"
LABEL="${LABEL:?new result-directory name}"
TRIALS="${TRIALS:-7}"
for stage in "${stages[@]}"; do
  test ! -e "$PROJECT_DIR/stages/$stage/$LABEL"
  mkdir "$PROJECT_DIR/stages/$stage/$LABEL"
done
cp "$0" "$PROJECT_DIR/run_micro_pass-$LABEL.sh"
for trial in $(seq 0 "$TRIALS"); do
  for bench in packed-update packed-merge; do
    if [ "$bench" = packed-update ]; then
      shapes=(local increment random insert churn erase noop)
    else
      shapes=(local increment random churn rebalance)
    fi
    for shape in "${shapes[@]}"; do
      operations=1000000
      if [ "$bench" = packed-update ] && \
         { [ "$shape" = insert ] || [ "$shape" = erase ]; }; then
        operations=200000
      fi
      for ((offset=0; offset<${#stages[@]}; ++offset)); do
        index=$(((offset + trial) % ${#stages[@]}))
        stage_dir="$PROJECT_DIR/stages/${stages[$index]}"
        taskset -c 2 "$stage_dir/bin/$bench" "$operations" "$shape" \
          > "$stage_dir/$LABEL/$bench-$shape-$trial.tsv"
      done
    done
  done
done
