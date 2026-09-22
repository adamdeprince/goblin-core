#!/usr/bin/env bash
set -euo pipefail
project=/home/adam/wiki2/wikimedia-rle-matrix-20260922T035401Z
cd "$project"
exec > preflight/prepare-input.log 2>&1
trap 'status=$?; printf "%s\n" "$status" > preflight/prepare-input.status; date -u +%Y-%m-%dT%H:%M:%SZ > preflight/prepare-input-finished.utc' EXIT
date -u +%Y-%m-%dT%H:%M:%SZ > preflight/prepare-input-started.utc
test "$(cat preflight/build.status)" = 0
g++ -O3 -std=c++23 -Wall -Wextra -Wpedantic harness/uuid_page_ids.cpp -o bin/uuid-page-ids
bin/uuid-page-ids input/numeric.cmds input/uuid.cmds.partial > preflight/uuid-conversion.json
python3 - <<'PY'
import json
from pathlib import Path
stats=json.loads(Path('preflight/uuid-conversion.json').read_text())
assert stats['commands']==1483700913, stats
assert stats['input_bytes']==33245740607, stats
assert stats['output_bytes']==Path('input/uuid.cmds.partial').stat().st_size, stats
assert stats['min_page_id']==0 and stats['max_page_id']==84112066, stats
PY
mv input/uuid.cmds.partial input/uuid.cmds
chmod a-w input/uuid.cmds
sha256sum input/uuid.cmds > preflight/uuid-input.sha256
cat preflight/input.sha256 preflight/uuid-input.sha256 > preflight/payloads.sha256
python3 - <<'PY'
from collections import Counter
from pathlib import Path
import subprocess
counts=Counter()
with open('input/numeric.cmds','rb') as stream:
    for _ in range(2000000):
        fields=stream.readline().split()
        assert fields[:3]==[b'ZINCRBY', b'key', b'1'] and len(fields)==4
        counts[int(fields[3])]+=1
with open('preflight/reference-prefix-2000000.json','wb') as out:
    verifier=subprocess.Popen(['python3','harness/zset_digest.py','--tie-order','numeric'],stdin=subprocess.PIPE,stdout=out)
    for member,score in sorted(counts.items(),key=lambda item:(item[1],item[0])):
        verifier.stdin.write(f'{member}\n{score}\n'.encode())
    verifier.stdin.close()
    assert verifier.wait()==0
PY
sha256sum bin/uuid-page-ids harness/uuid_page_ids.cpp > preflight/converter.sha256
