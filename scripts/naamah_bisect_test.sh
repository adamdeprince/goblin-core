#!/usr/bin/env bash
# Build current commit on naamah and run ~/feed_payload_uds.sh (HEAD_LINES=1000).
# Exit 0 = good (zset matches redis-8.8), 1 = bad, 125 = skip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

commit="$(git rev-parse --short HEAD)"
echo "bisect testing $commit ..." >&2

# main.cpp still references removed IoBackend APIs; patch so the server builds.
git checkout -- src/main.cpp
patch -p1 < "$ROOT/scripts/main-no-io-backend.patch" || {
  echo "failed to apply main.cpp build patch" >&2
  exit 125
}

tar czf /tmp/goblin-bisect.tgz --exclude='build*' --exclude='.git' --exclude='memory' .
scp -q /tmp/goblin-bisect.tgz naamah:/tmp/goblin-bisect.tgz

ssh naamah "set -euo pipefail
  ps -eo pid,cmd | awk '/goblin-bench\\/goblin-core --unixsocket/ {print \$1}' | xargs -r kill -9 2>/dev/null || true
  sleep 0.2
  rm -rf ~/lfsweep
  mkdir -p ~/lfsweep
  tar xzf /tmp/goblin-bisect.tgz -C ~/lfsweep 2>/dev/null
  cd ~/lfsweep
  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build build-rel --target goblin_core_server -j\$(nproc) >/dev/null
  install -m 755 build-rel/goblin-core ~/goblin-bench/goblin-core
"

ssh naamah "ps -eo pid,cmd | awk '/goblin-bench\\/goblin-core --unixsocket/ {print \$1}' | xargs -r kill -9 2>/dev/null || true"
out="$(ssh naamah 'SERVERS="goblin redis-8.8" HEAD_LINES=1000 ~/feed_payload_uds.sh 2>&1')"
ssh naamah "ps -eo pid,cmd | awk '/goblin-bench\\/goblin-core --unixsocket/ {print \$1}' | xargs -r kill -9 2>/dev/null || true"
echo "$out" | tail -8 >&2

if echo "$out" | grep -q "zset check: goblin .* MATCHES redis-8.8"; then
  echo "bisect: $commit GOOD" >&2
  exit 0
fi
if echo "$out" | grep -q "zset check: MISMATCH"; then
  echo "$out" | grep -E "zset check|members vs" >&2
  echo "bisect: $commit BAD" >&2
  exit 1
fi

echo "bisect: $commit inconclusive" >&2
exit 125