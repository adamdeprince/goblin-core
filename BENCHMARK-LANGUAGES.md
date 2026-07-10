# Scripting language benchmarks

Goblin Core embeds **six independent scripting interpreters**. Each is reached
through its own command prefix, has its own VM and its own precompiled script
cache, and shares nothing with the others but the key space (one `Store`):

| Interpreter | Language | Commands |
| --- | --- | --- |
| PUC-Lua 5.1 | the dialect real Redis scripts target | [`EVAL`](docs/commands/EVAL.md) · `EVALSHA` · `SCRIPT` |
| Luau | Roblox's typed, sandboxed Lua | [`LUAU.EVAL`](docs/commands/LUAU.EVAL.md) · … |
| Wren | a small class-based language | [`WREN.EVAL`](docs/commands/WREN.EVAL.md) · … |
| Jim Tcl | a small embeddable Tcl | [`TCL.EVAL`](docs/commands/TCL.EVAL.md) · … |
| MicroPython | a lean Python | [`UPYTHON.EVAL`](docs/commands/UPYTHON.EVAL.md) · … |
| QuickJS | JavaScript (quickjs-ng) | [`QUICKJS.EVAL`](docs/commands/QUICKJS.EVAL.md) · … |

Every engine **precompiles**: `SCRIPT LOAD` (or the first `EVAL`) compiles the
script once and caches the compiled artifact — Lua/Luau bytecode, QuickJS
bytecode, a Jim script object, a MicroPython function, a Wren function handle — so
`EVALSHA` executes with no re-parse or re-compile. The full command surface is in
[docs/commands](docs/commands/README.md).

## Methodology

Both benchmarks drive one server with a single Python connection, via the bundled
drivers ([`benchmarks/cad_benchmark.py`](benchmarks/cad_benchmark.py),
[`benchmarks/leaderboard_rescore_benchmark.py`](benchmarks/leaderboard_rescore_benchmark.py)).
A single Python connection is **client-bound**, so these are *relative*
comparisons — read the ratios, not absolute peak throughput (for absolute server
numbers a C load generator is needed, as in [BENCHMARKS.md](BENCHMARKS.md)). Every
script is `SCRIPT LOAD`-compiled before timing and run by `EVALSHA`, and each
driver verifies all six engines return identical output before timing. Numbers
are from a developer machine and are illustrative; reproduce with the commands
shown. **µs/op, lower is better.**

## Workload 1 — compare-and-delete (a trivial script)

The [Redlock](https://redis.io/docs/latest/develop/use/patterns/distributed-locks/)
unlock idiom — `if GET key == token then DEL key` — two commands, no branching or
allocation, so this measures per-call *overhead*, not compute. Four numbers per
row: sequential (one request at a time) and pipelined (256 in flight), each over
TCP loopback and a Unix domain socket. For context: the native
[`GOBLIN.CAD`](docs/commands/GOBLIN.CAD.md) command and the naive, racy, non-atomic
`GET`+`DEL`.

| implementation | RTT | TCP seq | UDS seq | TCP pipe (256) | UDS pipe (256) |
| --- | ---: | ---: | ---: | ---: | ---: |
| **`GOBLIN.CAD`** (native C++) | 1 | `19.7` | `9.9` | **`1.5`** | **`1.4`** |
| `WREN.EVAL` | 1 | `23.2` | `10.8` | `2.5` | `2.5` |
| `TCL.EVAL` | 1 | `22.9` | `10.6` | `2.6` | `2.6` |
| `UPYTHON.EVAL` | 1 | `23.1` | `10.9` | `2.8` | `2.7` |
| `LUAU.EVAL` | 1 | `22.7` | `10.8` | `2.7` | `2.8` |
| `QUICKJS.EVAL` | 1 | `22.6` | `11.7` | `3.2` | `3.1` |
| `EVAL` (PUC-Lua 5.1) | 1 | `22.6` | `11.7` | `3.2` | `3.2` |
| `GET` + `DEL` (client-side, racy) | 2 | `40.1` | `20.3` | — | — |

On this trivial op every interpreter clusters within ~30% of the native command
once pipelined (`2.5–3.2` µs vs `1.4`) — per-call VM entry, not script execution,
dominates. The Unix socket roughly halves sequential latency vs TCP (~`10` vs
~`20` µs); pipelined, the transport stops mattering. The naive `GET`+`DEL` is ~2×
any atomic form on both transports (its second round trip), and it is *racy* —
another client can change the key between the read and the delete.

```
python3 benchmarks/cad_benchmark.py --goblin-bin build/goblin-core --keys 20000 --rounds 9
```

## Workload 2 — time-decay leaderboard rescore (a heavy script, + a native command)

A read-only real-time rescore: read a 1000-member leaderboard (score = each
member's last-activity timestamp), recompute every entry's recency weight, and
return the top-k, kept in a bounded insertion-sorted array (no full sort). Three
decay modes — `LINEAR` = `1/(1 + age/hl)`, `EXP` = `0.5^(age/hl)`, `STEP` = in/out
of a window. ~1000 iterations of parse + arithmetic + branch + list ops per call,
so this measures the VM's *execution* speed — and here the same idiom also has a
native command, [`GOBLIN.TD_LEADERBOARD_RESCORE`](docs/commands/GOBLIN.TD_LEADERBOARD_RESCORE.md).
All implementations return the same top-k ordering per mode. Pipelined over UDS,
median of 5 rounds, µs/op:

| implementation | LINEAR | EXP | STEP |
| --- | ---: | ---: | ---: |
| **native `GOBLIN.TD_LEADERBOARD_RESCORE`** | **`48`** | **`49`** | **`19`** |
| `LUAU.EVAL` (Luau) | `477` | `479` | `127` |
| `EVAL` (PUC-Lua 5.1) | `770` | `774` | `223` |
| `QUICKJS.EVAL` (JavaScript) | `938` | `952` | `164` |
| `WREN.EVAL` (Wren) | `970` | `971` | `197` |
| `UPYTHON.EVAL` (MicroPython) | `2456` | `2454` | `656` |
| `TCL.EVAL` (Jim Tcl) | `5834` | n/a | `852` |

Three things. **The native command is ~10× faster than the fastest interpreter
(Luau) and up to ~50× the slowest** — it skips the interpreter and the `ZRANGE`
copy and streams the board once. **The interpreters separate by >10×** on this
real workload (Luau leads, Jim Tcl trails). **`EXP`'s `pow` is not the bottleneck**
(`EXP ≈ LINEAR` everywhere) — per-member interpreter overhead is; `STEP` is
cheapest (a compare, no arithmetic). One engine can't even express `EXP`: Jim Tcl's
minimal `expr` has no `pow`/`exp`, so it does `LINEAR` and `STEP` only.

```
python3 benchmarks/leaderboard_rescore_benchmark.py --goblin-bin build/goblin-core --members 1000 --k 10
```

## Takeaway

The ranking **flips** between the two workloads. Jim Tcl is near the top on the
trivial compare-and-delete (per-call overhead is low) but slowest by far — >10× the
fastest interpreter — on the heavy rescore; Luau is middling on the trivial op but
fastest on real compute. So pick the interpreter for the *shape* of your scripts —
light glue between commands vs. heavy in-script computation. And where a hot idiom
has a native command — [`GOBLIN.CAD`](docs/commands/GOBLIN.CAD.md) for lock
release, [`GOBLIN.TD_LEADERBOARD_RESCORE`](docs/commands/GOBLIN.TD_LEADERBOARD_RESCORE.md)
for the rescore — it beats every interpreter by ~10× or more, since it skips the VM
entirely. Either way every script is precompiled, so `EVALSHA` never pays for
compilation regardless of engine (which for MicroPython would otherwise be ~`960` µs
per call).

The same two idioms, translated into each language, are on the command pages:
[`EVAL`](docs/commands/EVAL.md), [`LUAU.EVAL`](docs/commands/LUAU.EVAL.md),
[`WREN.EVAL`](docs/commands/WREN.EVAL.md), [`TCL.EVAL`](docs/commands/TCL.EVAL.md),
[`UPYTHON.EVAL`](docs/commands/UPYTHON.EVAL.md),
[`QUICKJS.EVAL`](docs/commands/QUICKJS.EVAL.md).
