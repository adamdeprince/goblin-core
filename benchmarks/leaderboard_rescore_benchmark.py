#!/usr/bin/env python3
"""Benchmark the real-time leaderboard-rescore idiom across every embedded language.

A leaderboard's stored score is each member's last-activity timestamp. The script
rescores every entry on read by recency -- decay = 1/(1 + age/half_life), no
transcendentals -- and returns the top k most-recent members, keeping the top-k in
a bounded insertion-sorted array (work is O(n*k), not a full sort). It is
read-only, so one board is loaded once and rescored repeatedly.

This exercises the *interpreters* only: the same idiom (verbatim from the
docs/commands/*.EVAL.md pages) in PUC-Lua, Luau, Wren, Jim Tcl, MicroPython, and
QuickJS. There is no native command or hand-rolled baseline -- the point is to
compare the languages on a heavier, branchy, allocation-y script than
compare-and-delete. Every script is SCRIPT LOAD-compiled before timing and run by
EVALSHA, and all six are verified to return identical output first.

One Python connection over a Unix socket; read the ratios, not absolute peak
(client-bound). Example:

    python3 benchmarks/leaderboard_rescore_benchmark.py \
        --goblin-bin build-release/goblin-core --members 1000 --k 10
"""
from __future__ import annotations

import argparse
import os
import statistics
import time
from pathlib import Path
from typing import Sequence

import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import zset_benchmark as zbench  # noqa: E402 - path set above.


LUA = r"""
local now, hl, k = tonumber(ARGV[1]), tonumber(ARGV[2]), tonumber(ARGV[3])
local flat = redis.call('ZRANGE', KEYS[1], 0, -1, 'WITHSCORES')
local best, bestn = {}, 0
for i = 1, #flat, 2 do
  local m = flat[i]
  local ts = tonumber(flat[i + 1])
  local d = 1.0 / (1.0 + (now - ts) / hl)
  if bestn < k or d > best[bestn].s then
    local pos = (bestn < k) and bestn + 1 or bestn
    while pos > 1 and best[pos - 1].s < d do
      best[pos] = best[pos - 1]
      pos = pos - 1
    end
    best[pos] = {m = m, s = d}
    if bestn < k then bestn = bestn + 1 end
  end
end
local result = {}
for i = 1, bestn do
  result[#result + 1] = best[i].m
  result[#result + 1] = math.floor(best[i].s * 1000000 + 0.5)
end
return result
"""

WREN = r"""
var now = Num.fromString(ARGV[0])
var hl = Num.fromString(ARGV[1])
var k = Num.fromString(ARGV[2])
var flat = Redis.call(["zrange", KEYS[0], 0, -1, "WITHSCORES"])
var best = []
var i = 0
while (i < flat.count) {
  var m = flat[i]
  var ts = Num.fromString(flat[i + 1])
  var d = 1 / (1 + (now - ts) / hl)
  if (best.count < k || d > best[best.count - 1][1]) {
    var pos = best.count
    while (pos > 0 && best[pos - 1][1] < d) pos = pos - 1
    best.insert(pos, [m, d])
    if (best.count > k) best.removeAt(best.count - 1)
  }
  i = i + 2
}
var result = []
for (e in best) {
  result.add(e[0])
  result.add((e[1] * 1000000 + 0.5).floor)
}
return result
"""

TCL = r"""
set now [lindex $ARGV 0]
set hl [lindex $ARGV 1]
set k [lindex $ARGV 2]
set flat [redis call zrange [lindex $KEYS 0] 0 -1 WITHSCORES]
set bestM {}
set bestS {}
set n [llength $flat]
for {set i 0} {$i < $n} {incr i 2} {
  set m [lindex $flat $i]
  set ts [lindex $flat [expr {$i + 1}]]
  set d [expr {1.0 / (1.0 + ($now - $ts) / double($hl))}]
  set cnt [llength $bestS]
  set ins 0
  if {$cnt < $k} {
    set ins 1
  } elseif {$d > [lindex $bestS end]} {
    set ins 1
  }
  if {$ins} {
    set pos $cnt
    while {$pos > 0 && [lindex $bestS [expr {$pos - 1}]] < $d} { incr pos -1 }
    set bestM [linsert $bestM $pos $m]
    set bestS [linsert $bestS $pos $d]
    if {[llength $bestS] > $k} {
      set bestM [lreplace $bestM end end]
      set bestS [lreplace $bestS end end]
    }
  }
}
set result {}
foreach m $bestM s $bestS {
  lappend result $m [expr {int($s * 1000000 + 0.5)}]
}
return [redis array $result]
"""

PY = r"""
now = float(ARGV[0]); hl = float(ARGV[1]); k = int(ARGV[2])
flat = redis.call('zrange', KEYS[0], 0, -1, 'WITHSCORES')
best = []
for i in range(0, len(flat), 2):
    m = flat[i]
    ts = float(flat[i + 1])
    d = 1.0 / (1.0 + (now - ts) / hl)
    if len(best) < k or d > best[-1][0]:
        pos = len(best)
        while pos > 0 and best[pos - 1][0] < d:
            pos -= 1
        best.insert(pos, [d, m])
        if len(best) > k:
            best.pop()
reply = []
for d, m in best:
    reply.append(m)
    reply.append(int(d * 1000000 + 0.5))
"""

JS = r"""
var now = parseFloat(ARGV[0]), hl = parseFloat(ARGV[1]), k = parseInt(ARGV[2]);
var flat = redis.call('zrange', KEYS[0], 0, -1, 'WITHSCORES');
var best = [];
for (var i = 0; i < flat.length; i += 2) {
  var m = flat[i];
  var ts = parseFloat(flat[i + 1]);
  var d = 1.0 / (1.0 + (now - ts) / hl);
  if (best.length < k || d > best[best.length - 1][0]) {
    var pos = best.length;
    while (pos > 0 && best[pos - 1][0] < d) pos -= 1;
    best.splice(pos, 0, [d, m]);
    if (best.length > k) best.pop();
  }
}
var result = [];
for (var j = 0; j < best.length; j++) {
  result.push(best[j][1]);
  result.push(Math.floor(best[j][0] * 1000000 + 0.5));
}
return result;
"""

# (label, command prefix, source).
ENGINES = [
    ("EVAL (PUC-Lua 5.1)", "", LUA),
    ("LUAU.EVAL (Luau)", "LUAU.", LUA),
    ("WREN.EVAL (Wren)", "WREN.", WREN),
    ("TCL.EVAL (Jim Tcl)", "TCL.", TCL),
    ("UPYTHON.EVAL (MicroPython)", "UPYTHON.", PY),
    ("QUICKJS.EVAL (JavaScript)", "QUICKJS.", JS),
]


def normalize(reply) -> list:
    return [x.decode() if isinstance(x, (bytes, bytearray)) else x for x in reply]


def load_and_compile(client, members: int):
    base = 1_000_000
    cmds = [("ZADD", "board", base + i * 7, f"p{i}") for i in range(members)]
    client.pipeline(cmds, flush_every=512)
    now = base + members * 7 + 50
    shas = {}
    print("Compiling scripts (SCRIPT LOAD) before benchmarking:")
    for label, prefix, src in ENGINES:
        sha = client.command(f"{prefix}SCRIPT", "LOAD", src)
        sha = sha.decode() if isinstance(sha, bytes) else str(sha)
        if len(sha) != 40:
            raise RuntimeError(f"{label}: unexpected SCRIPT LOAD reply {sha!r}")
        shas[label] = (prefix, sha)
        print(f"  {label:30}  {sha}")
    return shas, now


def main(argv: Sequence[str]) -> int:
    default_goblin = Path("build-release/goblin-core")
    if not default_goblin.exists():
        default_goblin = Path("build/goblin-core")

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--members", type=int, default=1000, help="leaderboard size")
    parser.add_argument("--k", type=int, default=10, help="top-k to return")
    parser.add_argument("--half-life", type=float, default=300.0)
    parser.add_argument("--iters", type=int, default=2000, help="rescores per timed round")
    parser.add_argument("--rounds", type=int, default=7, help="rounds (median reported)")
    parser.add_argument("--pipeline", type=int, default=64, help="pipelined in-flight depth")
    parser.add_argument("--unix-socket", type=str, default=None,
                        help="UDS path (default: a short auto path in /tmp)")
    args = parser.parse_args(argv)

    sock = args.unix_socket or f"/tmp/goblin-rescore-{os.getpid()}.sock"
    server = zbench.start_goblin(binary=args.goblin_bin, rank_cache=False, unix_socket=sock)
    try:
        c = zbench.RespClient("127.0.0.1", server.port, unix_socket=sock)
        shas, now = load_and_compile(c, args.members)
        script_args = [str(now), str(int(args.half_life)), str(args.k)]

        # Verify all six return identical output before timing.
        print(f"\nVerifying all six agree on the top {args.k} of {args.members}...")
        ref = None
        for label, (prefix, sha) in shas.items():
            out = normalize(c.command(f"{prefix}EVALSHA", sha, 1, "board", *script_args))
            if ref is None:
                ref = out
            elif out != ref:
                print(f"  [FAIL] {label} disagrees:\n    {out}\n vs {ref}")
                return 1
        print(f"  all agree; top members: {[ref[i] for i in range(0, len(ref), 2)]}")

        print(f"\nBenchmarking {args.iters} rescores/round, median of {args.rounds} rounds, "
              f"over UDS, pipeline depth {args.pipeline}.\n")

        def seq(prefix, sha):
            start = time.perf_counter()
            for _ in range(args.iters):
                c.command(f"{prefix}EVALSHA", sha, 1, "board", *script_args)
            return (time.perf_counter() - start) / args.iters

        def pipe(prefix, sha):
            cmds = [(f"{prefix}EVALSHA", sha, 1, "board", *script_args)
                    for _ in range(args.iters)]
            start = time.perf_counter()
            c.pipeline(cmds, flush_every=args.pipeline)
            return (time.perf_counter() - start) / args.iters

        rows = []
        for label, (prefix, sha) in shas.items():
            s = statistics.median([seq(prefix, sha) for _ in range(args.rounds)]) * 1e6
            p = statistics.median([pipe(prefix, sha) for _ in range(args.rounds)]) * 1e6
            rows.append((label, s, p))

        fastest = min(p for _, _, p in rows)
        head = f"{'language':30}{'seq us/op':>12}{'seq k/s':>10}{'pipe us/op':>12}{'pipe k/s':>10}{'vs best':>9}"
        print(head)
        print("-" * len(head))
        for label, s, p in sorted(rows, key=lambda r: r[2]):
            print(f"{label:30}{s:12.2f}{1000.0/s:10.1f}{p:12.2f}{1000.0/p:10.1f}{p/fastest:8.2f}x")
        print(f"\n  Rescoring a {args.members}-member board, top {args.k}. seq = one at a "
              f"time; pipe = {args.pipeline} in flight.\n  One Python connection over UDS "
              "(client-bound); read the ratios, not absolute peak.")
        c.close()
    finally:
        server.stop()
        if not args.unix_socket:
            try:
                os.unlink(sock)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
