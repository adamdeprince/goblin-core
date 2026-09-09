#!/usr/bin/env bash
# Freeze and test one cumulative update-path optimization before measurement.
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
STAGE="${STAGE:?new immutable stage name}"
source_dir="$PROJECT_DIR/source"
stage_dir="$PROJECT_DIR/stages/$STAGE"
test ! -e "$stage_dir"
mkdir -p "$stage_dir/bin" "$stage_dir/include/goblin/core" "$stage_dir/tests"
cp "$0" "$stage_dir/"
for header in packed_zset.hpp packed_bplus_tree.hpp swiss_table.hpp; do
  cp "$source_dir/include/goblin/core/$header" "$stage_dir/include/goblin/core/"
done
cp "$source_dir/tests/packed_zset_test.cpp" \
   "$source_dir/tests/packed_bplus_tree_test.cpp" \
   "$source_dir/tests/goblin_core_tests.cpp" "$stage_dir/tests/"
cp "$source_dir/benchmarks/packed_update.cpp" \
   "$source_dir/benchmarks/packed_merge.cpp" "$stage_dir/"
sha256sum "$stage_dir/include/goblin/core/"*.hpp "$stage_dir/tests/"*.cpp \
  "$stage_dir/packed_update.cpp" "$stage_dir/packed_merge.cpp" \
  "$source_dir/src/store.cpp" > "$stage_dir/source.sha256"
trap 'tail -n 80 "$stage_dir/build.log"' ERR
cmake -S "$source_dir" -B "$PROJECT_DIR/build" -DCMAKE_BUILD_TYPE=Release \
  > "$stage_dir/build.log" 2>&1
cmake --build "$PROJECT_DIR/build" --target goblin_core_server goblin_core_tests \
  goblin_core_packed_zset_test goblin_core_packed_bplus_tree_test -j 16 \
  >> "$stage_dir/build.log" 2>&1
ctest --test-dir "$PROJECT_DIR/build" --output-on-failure \
  --output-log "$stage_dir/ctest.log" \
  -R '^(goblin_core_tests|goblin_core_packed_zset|goblin_core_packed_bplus_tree)$'
cp "$PROJECT_DIR/build/goblin-core" "$stage_dir/bin/"
for bench in packed_update packed_merge; do
  c++ -O3 -DNDEBUG -std=c++23 -I"$stage_dir/include" -I"$source_dir/include" \
    "$stage_dir/$bench.cpp" -o "$stage_dir/bin/${bench//_/-}"
done
cp "$PROJECT_DIR/build/CMakeCache.txt" "$stage_dir/"
sha256sum "$stage_dir/bin/"* > "$stage_dir/executables.sha256"
touch "$stage_dir/BUILD-TESTS-PASS"
