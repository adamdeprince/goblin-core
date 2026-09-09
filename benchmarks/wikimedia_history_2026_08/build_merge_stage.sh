#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="${PROJECT_DIR:?isolated benchmark project}"
STAGE="${STAGE:?new immutable stage name}"
SOURCE_DIR="$PROJECT_DIR/source"
STAGE_DIR="$PROJECT_DIR/stages/$STAGE"
test ! -e "$STAGE_DIR"
mkdir -p "$STAGE_DIR/bin" "$STAGE_DIR/include/goblin/core"
cp "$0" "$STAGE_DIR/build_merge_stage.sh"
cp "$SOURCE_DIR/include/goblin/core/packed_bplus_tree.hpp" \
   "$STAGE_DIR/include/goblin/core/"
cp "$SOURCE_DIR/tests/packed_bplus_tree_test.cpp" "$STAGE_DIR/"
cp "$SOURCE_DIR/benchmarks/packed_merge.cpp" "$STAGE_DIR/"
sha256sum "$SOURCE_DIR/include/goblin/core/packed_bplus_tree.hpp" \
  "$SOURCE_DIR/include/goblin/core/packed_zset.hpp" "$SOURCE_DIR/src/store.cpp" \
  "$SOURCE_DIR/tests/packed_bplus_tree_test.cpp" \
  "$SOURCE_DIR/benchmarks/packed_merge.cpp" > "$STAGE_DIR/source.sha256"
trap 'tail -n 80 "$STAGE_DIR/build.log"' ERR
cmake -S "$SOURCE_DIR" -B "$PROJECT_DIR/build" -DCMAKE_BUILD_TYPE=Release \
  > "$STAGE_DIR/build.log" 2>&1
cmake --build "$PROJECT_DIR/build" --target goblin_core_server \
  goblin_core_tests goblin_core_packed_zset_test goblin_core_packed_bplus_tree_test \
  -j 16 >> "$STAGE_DIR/build.log" 2>&1
ctest --test-dir "$PROJECT_DIR/build" --output-on-failure \
  --output-log "$STAGE_DIR/ctest.log" \
  -R '^(goblin_core_tests|goblin_core_packed_zset|goblin_core_packed_bplus_tree)$'
cp "$PROJECT_DIR/build/goblin-core" "$STAGE_DIR/bin/"
c++ -O3 -DNDEBUG -std=c++23 -I"$STAGE_DIR/include" -I"$SOURCE_DIR/include" \
  "$SOURCE_DIR/benchmarks/packed_merge.cpp" -o "$STAGE_DIR/bin/packed-merge"
cp "$PROJECT_DIR/build/CMakeCache.txt" "$STAGE_DIR/"
touch "$STAGE_DIR/BUILD-TESTS-PASS"
