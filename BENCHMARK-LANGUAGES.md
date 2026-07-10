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

## Workload 2 — leaderboard rescore (a heavy script)

A read-only real-time rescore: read a 1000-member leaderboard (score = each
member's last-activity timestamp), recompute every entry's recency
`1/(1 + age/half_life)` (no transcendentals), keep the top-k in a bounded
insertion-sorted array, and return it. Roughly 1000 iterations of parse +
arithmetic + branch + list ops per call — so this measures the VM's *execution*
speed, not per-call overhead.

| language | seq µs/op | pipe µs/op | vs fastest |
| --- | ---: | ---: | ---: |
| **Luau** | `403` | **`386`** | **`1.00×`** |
| PUC-Lua 5.1 | `452` | `440` | `1.14×` |
| QuickJS (JavaScript) | `548` | `529` | `1.37×` |
| Wren | `680` | `672` | `1.74×` |
| MicroPython | `1582` | `1566` | `4.06×` |
| Jim Tcl | `2271` | `2267` | `5.87×` |

Now the interpreters separate by nearly **6×**: Luau and PUC-Lua lead, QuickJS and
Wren follow, and MicroPython and Tcl trail. Because the script cost (`0.4–2.3` ms)
dwarfs the ~`10` µs round trip, **sequential ≈ pipelined** — pipelining buys almost
nothing when the server is doing real work per call.

```
python3 benchmarks/leaderboard_rescore_benchmark.py --goblin-bin build/goblin-core --members 1000 --k 10
```

## Takeaway

The ranking **flips** between the two workloads. Jim Tcl is near the top on the
trivial compare-and-delete (per-call overhead is low) but slowest by far — ~6× —
on the heavy rescore; Luau is middling on the trivial op but fastest on real
compute. So pick the interpreter for the *shape* of your scripts — light glue
between commands vs. heavy in-script computation — and for the one idiom that has
a native command, prefer [`GOBLIN.CAD`](docs/commands/GOBLIN.CAD.md). Either way
every script is precompiled, so `EVALSHA` never pays for compilation regardless of
engine (which for MicroPython would otherwise be ~`960` µs per call).

The same two idioms, translated into each language, are on the command pages:
[`EVAL`](docs/commands/EVAL.md), [`LUAU.EVAL`](docs/commands/LUAU.EVAL.md),
[`WREN.EVAL`](docs/commands/WREN.EVAL.md), [`TCL.EVAL`](docs/commands/TCL.EVAL.md),
[`UPYTHON.EVAL`](docs/commands/UPYTHON.EVAL.md),
[`QUICKJS.EVAL`](docs/commands/QUICKJS.EVAL.md).
