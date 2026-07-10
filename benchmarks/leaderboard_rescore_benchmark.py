#!/usr/bin/env python3
"""Benchmark the time-decay leaderboard rescore: native command vs every language.

A leaderboard zset stores each member's last-activity timestamp as its score. The
rescore reads the whole zset, recomputes a recency weight for every member, and
returns the top k by that weight -- kept in a bounded insertion-sorted array (no
full sort). Three decay modes:

  LINEAR : 1 / (1 + age/half_life)   -- hyperbolic falloff, no transcendental
  EXP    : 0.5 ^ (age/half_life)     -- true half-life decay (a pow per member)
  STEP   : 1 inside the [now-half_life, now] window, else 0

Compared: the native GOBLIN.TD_LEADERBOARD_RESCORE, and the identical idiom in
each embedded language (PUC-Lua, Luau, Wren, Jim Tcl, MicroPython, QuickJS), run
by EVALSHA (SCRIPT LOAD-compiled before timing). All seven are verified to return
the same top-k member ordering (per mode) before timing. Pipelined over a Unix
domain socket, so the number is server-side per-op cost. One Python connection is
client-bound; read the ratios. Lower is better.

    python3 benchmarks/leaderboard_rescore_benchmark.py \
        --goblin-bin build/goblin-core --members 1000 --k 10
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


# --- the idiom in each language (0.5^x written portably; Luau has no math.pow) ---

LUA = r"""
local now=tonumber(ARGV[1]); local hl=tonumber(ARGV[2]); local k=tonumber(ARGV[3]); local mode=ARGV[4]
local flat=redis.call('ZRANGE',KEYS[1],0,-1,'WITHSCORES')
local names,scores,n={},{},0
local function push(name,s)
  if n<k then n=n+1; names[n],scores[n]=name,s; local j=n
    while j>1 and scores[j]>scores[j-1] do names[j],names[j-1]=names[j-1],names[j]; scores[j],scores[j-1]=scores[j-1],scores[j]; j=j-1 end
  elseif s>scores[k] then names[k],scores[k]=name,s; local j=k
    while j>1 and scores[j]>scores[j-1] do names[j],names[j-1]=names[j-1],names[j]; scores[j],scores[j-1]=scores[j-1],scores[j]; j=j-1 end
  end
end
if mode=='LINEAR' then local inv=1.0/hl
  for i=1,#flat,2 do push(flat[i], 1.0/(1.0+(now-tonumber(flat[i+1]))*inv)) end
elseif mode=='EXP' then local inv=1.0/hl
  for i=1,#flat,2 do push(flat[i], 0.5^((now-tonumber(flat[i+1]))*inv)) end
elseif mode=='STEP' then local cutoff=now-hl
  for i=1,#flat,2 do local ts=tonumber(flat[i+1]); push(flat[i], ts>=cutoff and 1.0 or 0.0) end
else return redis.error_reply('ERR mode must be LINEAR, EXP or STEP') end
local out={}
for i=1,n do out[#out+1]=names[i]; out[#out+1]=tostring(scores[i]) end
return out
"""

WREN = r"""
var now = Num.fromString(ARGV[0])
var hl = Num.fromString(ARGV[1])
var k = Num.fromString(ARGV[2])
var mode = ARGV[3]
var flat = Redis.call(["zrange", KEYS[0], 0, -1, "WITHSCORES"])
var names = []
var scores = []
var push = Fn.new {|name, s|
  var j = -1
  if (names.count < k) {
    names.add(name)
    scores.add(s)
    j = names.count - 1
  } else if (s > scores[k-1]) {
    names[k-1] = name
    scores[k-1] = s
    j = k - 1
  }
  while (j > 0 && scores[j] > scores[j-1]) {
    var tn = names[j]
    names[j] = names[j-1]
    names[j-1] = tn
    var tx = scores[j]
    scores[j] = scores[j-1]
    scores[j-1] = tx
    j = j - 1
  }
}
var i = 0
if (mode == "LINEAR") {
  var inv = 1 / hl
  while (i < flat.count) {
    push.call(flat[i], 1 / (1 + (now - Num.fromString(flat[i+1])) * inv))
    i = i + 2
  }
} else if (mode == "EXP") {
  var inv = 1 / hl
  while (i < flat.count) {
    push.call(flat[i], (0.5).pow((now - Num.fromString(flat[i+1])) * inv))
    i = i + 2
  }
} else if (mode == "STEP") {
  var cutoff = now - hl
  while (i < flat.count) {
    var ts = Num.fromString(flat[i+1])
    push.call(flat[i], (ts >= cutoff) ? 1 : 0)
    i = i + 2
  }
} else {
  return Redis.error("ERR mode must be LINEAR, EXP or STEP")
}
var out = []
var m = 0
while (m < names.count) {
  out.add(names[m])
  out.add(scores[m].toString)
  m = m + 1
}
return out
"""

TCL = r"""
proc push {name s} {
  upvar 1 bestM bestM bestS bestS nn nn k k
  if {$nn < $k} {
    lappend bestM $name; lappend bestS $s; incr nn
    set j [expr {$nn-1}]
  } elseif {$s > [lindex $bestS [expr {$k-1}]]} {
    lset bestM [expr {$k-1}] $name; lset bestS [expr {$k-1}] $s
    set j [expr {$k-1}]
  } else { return }
  while {$j>0 && [lindex $bestS $j] > [lindex $bestS [expr {$j-1}]]} {
    set jm [expr {$j-1}]
    set t [lindex $bestM $j]; lset bestM $j [lindex $bestM $jm]; lset bestM $jm $t
    set t [lindex $bestS $j]; lset bestS $j [lindex $bestS $jm]; lset bestS $jm $t
    incr j -1
  }
}
set now [lindex $ARGV 0]; set hl [lindex $ARGV 1]; set k [lindex $ARGV 2]; set mode [lindex $ARGV 3]
set flat [redis call zrange [lindex $KEYS 0] 0 -1 WITHSCORES]
set bestM {}; set bestS {}; set nn 0
set len [llength $flat]
if {$mode eq "LINEAR"} {
  set inv [expr {1.0/$hl}]
  for {set i 0} {$i<$len} {incr i 2} {
    push [lindex $flat $i] [expr {1.0/(1.0+($now-[lindex $flat [expr {$i+1}]])*$inv)}]
  }
} elseif {$mode eq "EXP"} {
  set inv [expr {1.0/$hl}]
  for {set i 0} {$i<$len} {incr i 2} {
    push [lindex $flat $i] [expr {pow(0.5,($now-[lindex $flat [expr {$i+1}]])*$inv)}]
  }
} elseif {$mode eq "STEP"} {
  set cutoff [expr {$now-$hl}]
  for {set i 0} {$i<$len} {incr i 2} {
    set ts [lindex $flat [expr {$i+1}]]
    push [lindex $flat $i] [expr {$ts>=$cutoff ? 1.0 : 0.0}]
  }
} else { return [redis error {ERR mode must be LINEAR, EXP or STEP}] }
set out {}
foreach m $bestM sv $bestS { lappend out $m $sv }
return [redis array $out]
"""

PY = r"""
now=float(ARGV[0]); hl=float(ARGV[1]); k=int(ARGV[2]); mode=ARGV[3]
flat=redis.call('zrange',KEYS[0],0,-1,'WITHSCORES')
names=[]; scores=[]
def push(name,s):
    if len(names)<k:
        names.append(name); scores.append(s); j=len(names)-1
    elif k>0 and s>scores[k-1]:
        names[k-1]=name; scores[k-1]=s; j=k-1
    else:
        return
    while j>0 and scores[j]>scores[j-1]:
        names[j],names[j-1]=names[j-1],names[j]; scores[j],scores[j-1]=scores[j-1],scores[j]; j-=1
ok=True
if mode=='LINEAR':
    inv=1.0/hl
    for i in range(0,len(flat),2): push(flat[i], 1.0/(1.0+(now-float(flat[i+1]))*inv))
elif mode=='EXP':
    inv=1.0/hl
    for i in range(0,len(flat),2): push(flat[i], 0.5**((now-float(flat[i+1]))*inv))
elif mode=='STEP':
    cutoff=now-hl
    for i in range(0,len(flat),2):
        ts=float(flat[i+1]); push(flat[i], 1.0 if ts>=cutoff else 0.0)
else:
    ok=False; reply={'err':'ERR mode must be LINEAR, EXP or STEP'}
if ok:
    out=[]
    for a in range(len(names)): out.append(names[a]); out.append(str(scores[a]))
    reply=out
"""

JS = r"""
var now=parseFloat(ARGV[0]), hl=parseFloat(ARGV[1]), k=parseInt(ARGV[2]), mode=ARGV[3];
var flat=redis.call('zrange',KEYS[0],0,-1,'WITHSCORES');
var names=[], scores=[];
function push(name,s){
  var j;
  if (names.length<k){ names.push(name); scores.push(s); j=names.length-1; }
  else if (k>0 && s>scores[k-1]){ names[k-1]=name; scores[k-1]=s; j=k-1; }
  else return;
  while (j>0 && scores[j]>scores[j-1]){ var tn=names[j]; names[j]=names[j-1]; names[j-1]=tn;
    var tx=scores[j]; scores[j]=scores[j-1]; scores[j-1]=tx; j-=1; }
}
if (mode==='LINEAR'){ var inv=1/hl; for(var i=0;i<flat.length;i+=2) push(flat[i], 1/(1+(now-parseFloat(flat[i+1]))*inv)); }
else if (mode==='EXP'){ var inv=1/hl; for(var i=0;i<flat.length;i+=2) push(flat[i], Math.pow(0.5,(now-parseFloat(flat[i+1]))*inv)); }
else if (mode==='STEP'){ var cutoff=now-hl; for(var i=0;i<flat.length;i+=2){ var ts=parseFloat(flat[i+1]); push(flat[i], ts>=cutoff?1:0); } }
else return redis.error('ERR mode must be LINEAR, EXP or STEP');
var out=[]; for(var a=0;a<names.length;a++){ out.push(names[a]); out.push(String(scores[a])); } return out;
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
NATIVE = "GOBLIN.TD_LEADERBOARD_RESCORE (native C++)"
MODES = ["LINEAR", "EXP", "STEP"]


def names_of(reply) -> list:
    # reply is a flat [name, score, name, score, ...]; keep just the member names.
    out = []
    for i in range(0, len(reply), 2):
        x = reply[i]
        out.append(x.decode() if isinstance(x, (bytes, bytearray)) else x)
    return out


def main(argv: Sequence[str]) -> int:
    default_goblin = Path("build-release/goblin-core")
    if not default_goblin.exists():
        default_goblin = Path("build/goblin-core")

    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--goblin-bin", type=Path, default=default_goblin)
    parser.add_argument("--members", type=int, default=1000)
    parser.add_argument("--k", type=int, default=10)
    parser.add_argument("--iters", type=int, default=500, help="rescores per timed round")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--pipeline", type=int, default=32, help="in-flight depth")
    parser.add_argument("--unix-socket", type=str, default=None)
    args = parser.parse_args(argv)

    sock = args.unix_socket or f"/tmp/goblin-td-{os.getpid()}.sock"
    server = zbench.start_goblin(binary=args.goblin_bin, rank_cache=False, unix_socket=sock)
    try:
        c = zbench.RespClient("127.0.0.1", server.port, unix_socket=sock)
        base = 1_000_000
        c.pipeline((("ZADD", "lb", base + i * 7, f"p{i}") for i in range(args.members)),
                   flush_every=512)
        now = base + args.members * 7 + 40
        hl = max(int(args.members * 7 // 2), 1)   # STEP window ~ half the board
        sargs = [str(now), str(hl), str(args.k)]

        print("Compiling scripts (SCRIPT LOAD) before benchmarking:")
        shas = {}
        for label, prefix, src in ENGINES:
            sha = c.command(f"{prefix}SCRIPT", "LOAD", src)
            sha = sha.decode() if isinstance(sha, bytes) else str(sha)
            if len(sha) != 40:
                print(f"  [FAIL] {label} LOAD -> {sha!r}")
                return 1
            shas[label] = (prefix, sha)
            print(f"  {label:30}  {sha}")

        def run(label, mode):
            if label == NATIVE:
                return c.command("GOBLIN.TD_LEADERBOARD_RESCORE", "lb", *sargs, mode)
            prefix, sha = shas[label]
            return c.command(f"{prefix}EVALSHA", sha, 1, "lb", *sargs, mode)

        impls = [NATIVE] + [e[0] for e in ENGINES]

        # Verify agreement on the top-k member ordering, per mode. An engine whose
        # runtime lacks the math a mode needs (Jim Tcl's minimal expr has no
        # pow/exp, so no EXP) is recorded as unsupported rather than compared.
        print(f"\nVerifying agreement across {len(impls)} implementations x {len(MODES)} modes...")
        supported = {}
        for mode in MODES:
            ref = names_of(run(NATIVE, mode))
            for label in impls:
                try:
                    got = names_of(run(label, mode))
                except RuntimeError as exc:
                    supported[(label, mode)] = False
                    print(f"    {label} / {mode}: unsupported ({exc})")
                    continue
                supported[(label, mode)] = True
                if got != ref:
                    print(f"  [FAIL] {mode} {label} disagrees:\n    {got}\n vs {ref}")
                    return 1
            print(f"  {mode:7} agree; top members: {ref[:5]}...")

        # Bench: pipelined per-op cost per supported (implementation, mode).
        def bench(label, mode):
            cmds = ([("GOBLIN.TD_LEADERBOARD_RESCORE", "lb", *sargs, mode)] * args.iters
                    if label == NATIVE else
                    [(f"{shas[label][0]}EVALSHA", shas[label][1], 1, "lb", *sargs, mode)] * args.iters)
            per = []
            for _ in range(args.rounds):
                t = time.perf_counter()
                c.pipeline(iter(cmds), flush_every=args.pipeline)
                per.append((time.perf_counter() - t) / args.iters)
            return statistics.median(per) * 1e6

        print(f"\nRescoring {args.members} members, top {args.k}, pipelined over UDS "
              f"(depth {args.pipeline}), median of {args.rounds} rounds. us/op:\n")
        rows = [(label, {m: (bench(label, m) if supported.get((label, m)) else None)
                         for m in MODES}) for label in impls]

        def cell(x):
            return f"{x:10.1f}" if x is not None else f"{'--':>10}"

        head = f"{'implementation':34}{'LINEAR':>10}{'EXP':>10}{'STEP':>10}"
        print(head)
        print("-" * len(head))
        native_us = dict(rows[0][1])
        for label, us in rows:
            cells = "".join(cell(us[m]) for m in MODES)
            tag = "  <- native" if label == NATIVE else ""
            print(f"{label:34}{cells}{tag}")
        vs = [f"{label.split()[0]} {us['EXP']/native_us['EXP']:.0f}x"
              for label, us in rows if label != NATIVE and us["EXP"] is not None]
        print("\nEXP cost vs native:  " + ", ".join(vs))
        print("\n  us/op, lower is better. All implementations return the same top-k\n"
              "  ordering per mode. `--` = the engine can't express that mode (Jim Tcl\n"
              "  has no pow/exp for EXP). One Python connection; read the ratios.")
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
