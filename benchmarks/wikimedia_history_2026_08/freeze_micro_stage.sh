#!/usr/bin/env bash
# Freeze candidates for quick screening. This does not certify the server;
# the selected implementation also needs the full build/test/replay workflow.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
STAGE="${STAGE:?new immutable stage name}"
source_dir="$PROJECT_DIR/source"
stage_dir="$PROJECT_DIR/stages/$STAGE"
test ! -e "$stage_dir"
mkdir -p "$stage_dir/bin" "$stage_dir/include/goblin/core"
cp "$0" "$stage_dir/"
for header in packed_zset.hpp packed_bplus_tree.hpp swiss_table.hpp; do
  cp "$source_dir/include/goblin/core/$header" "$stage_dir/include/goblin/core/"
done
for bench in packed_update packed_merge; do
  cp "$source_dir/benchmarks/$bench.cpp" "$stage_dir/"
  c++ -O3 -DNDEBUG -std=c++23 -I"$stage_dir/include" -I"$source_dir/include" \
    "$stage_dir/$bench.cpp" -o "$stage_dir/bin/${bench//_/-}"
done
sha256sum "$stage_dir/include/goblin/core/"*.hpp "$stage_dir/"*.cpp \
  > "$stage_dir/source.sha256"
sha256sum "$stage_dir/bin/"* > "$stage_dir/executables.sha256"
