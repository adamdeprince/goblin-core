#!/usr/bin/env bash
set -euo pipefail
STAGE_DIR="${STAGE_DIR:?directory containing bin/packed-merge}"
test ! -e "$STAGE_DIR/micro"
mkdir "$STAGE_DIR/micro"
cp "$0" "$STAGE_DIR/run_merge_micro.sh"
for shape in local increment random churn rebalance; do
  taskset -c 2 "$STAGE_DIR/bin/packed-merge" 1000000 "$shape" \
    > "$STAGE_DIR/micro/$shape-warmup.tsv"
done
for trial in 1 2 3; do
  for shape in local increment random churn rebalance; do
    taskset -c 2 "$STAGE_DIR/bin/packed-merge" 1000000 "$shape" \
      > "$STAGE_DIR/micro/$shape-$trial.tsv"
  done
done
sha256sum "$STAGE_DIR/bin/packed-merge" > "$STAGE_DIR/micro-binary.sha256"
touch "$STAGE_DIR/MICRO-PASS"
