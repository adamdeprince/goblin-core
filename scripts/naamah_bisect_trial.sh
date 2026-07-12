#!/usr/bin/env bash
# One HEAD_LINES=1000 harness trial on naamah. Exit 0 = match, 1 = mismatch.
set -euo pipefail
out="$(ssh naamah 'SERVERS="goblin redis-8.8" HEAD_LINES=1000 ~/feed_payload_uds.sh 2>&1')"
if echo "$out" | grep -q "zset check: goblin .* MATCHES redis-8.8"; then
  exit 0
fi
echo "$out" | grep -E "zset check|members vs" >&2
exit 1