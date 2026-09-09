#!/usr/bin/env bash
# Compare all frozen variants after builds/replays, rotating order per round.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
stages=(00-baseline 01-bitmap 02-run-merge 03-redistribute)
for stage in "${stages[@]}"; do
  stage_dir="$PROJECT_DIR/stages/$stage"
  test -f "$stage_dir/REPLAYS-PASS"
  test ! -e "$stage_dir/micro-confirmation"
  mkdir "$stage_dir/micro-confirmation"
  for shape in local increment random churn rebalance; do
    taskset -c 2 "$stage_dir/bin/packed-merge" 1000000 "$shape" \
      > "$stage_dir/micro-confirmation/$shape-warmup.tsv"
  done
done
for trial in 1 2 3; do
  case "$trial" in
    1) order=(0 1 2 3) ;;
    2) order=(3 2 1 0) ;;
    3) order=(1 3 0 2) ;;
  esac
  for shape in local increment random churn rebalance; do
    for index in "${order[@]}"; do
      stage_dir="$PROJECT_DIR/stages/${stages[$index]}"
      taskset -c 2 "$stage_dir/bin/packed-merge" 1000000 "$shape" \
        > "$stage_dir/micro-confirmation/$shape-$trial.tsv"
    done
  done
done
cp "$0" "$PROJECT_DIR/run_merge_confirmation.sh"
touch "$PROJECT_DIR/MICRO-CONFIRMATION-PASS"
