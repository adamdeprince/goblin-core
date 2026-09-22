#!/usr/bin/env bash
set -euo pipefail
project=/home/adam/wiki2/wikimedia-rle-matrix-20260922T035401Z
exec > "$project/preflight/build.log" 2>&1
trap 'status=$?; printf "%s\n" "$status" > "$project/preflight/build.status"; date -u +%Y-%m-%dT%H:%M:%SZ > "$project/preflight/build-finished.utc"' EXIT
date -u +%Y-%m-%dT%H:%M:%SZ > "$project/preflight/build-started.utc"
cd "$project"
printf '%s\n' 'ee480f36691b5f7c40bb5db9c19ea649282a91a3ba952a0d012a21a62dec7735  source.tar.gz' | sha256sum -c -
tar -xzf source.tar.gz -C src
(cd src && sha256sum -c ../source-files.sha256) > preflight/source-verification.log
(
  sha256sum input/numeric.cmds > preflight/input.sha256
  actual=$(cut -d' ' -f1 preflight/input.sha256)
  test "$actual" = "$(cat preflight/expected-input-sha256.txt)"
  wc -l < input/numeric.cmds > preflight/actual-commands.txt
  test "$(cat preflight/actual-commands.txt)" = "$(cat preflight/commands.txt)"
) > preflight/input-verification.log 2>&1 &
input_check=$!
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release -DGOBLIN_CORE_ARCH=native -DGOBLIN_CORE_BUILD_HTML_DOCS=OFF -DGOBLIN_CORE_BUILD_BENCHMARKS=OFF -DGOBLIN_CORE_ENABLE_KAFKA=OFF -DGOBLIN_CORE_ENABLE_RDMA=OFF -DGOBLIN_CORE_ENABLE_TLS=OFF
cmake --build build --target goblin_core_server goblin_core_packed_zset_test goblin_core_packed_bplus_tree_test goblin_core_tests goblin_core_replication_test -j 12
ctest --test-dir build -R '^(goblin_core_tests|goblin_core_packed_zset|goblin_core_packed_bplus_tree|goblin_core_replication)$' --output-on-failure > preflight/tests.log
install -m 0555 build/goblin-core bin/goblin-core
sha256sum bin/goblin-core /home/adam/bench/redis-8.8.0/src/redis-cli > preflight/binaries.sha256
wait "$input_check"
{ date -u +%Y-%m-%dT%H:%M:%SZ; uname -a; lscpu; free -b; } > preflight/host.txt
