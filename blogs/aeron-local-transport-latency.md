# How an 11-byte arena tail became a 157-microsecond Aeron SET

The local-transport benchmark was supposed to answer a straightforward question: what does
the same Goblin Core operation cost over a shared-memory ring, Aeron IPC, Aeron UDP on
localhost, and a Unix-domain socket? The medians gave the expected answer. The p99 for one
case did not.

`PING` and `GET` over Aeron UDP stayed near 20 microseconds at p99, but a 16-byte `SET`
jumped to roughly 157 microseconds. RESP and SBE both did it. Two complete runs reproduced
it. That made the outlier more interesting than the ranking: a protocol-independent store
pause was interacting with Aeron's polling policy.

The short version is that each overwrite retired an 11-byte spilled value tail. Goblin's
keyspace arena compacted after 64 KiB became reclaimable, once every 5,958 overwrites. The
compaction itself was visible over every transport, but the Aeron C Media Driver's default
exponential-backoff agents amplified that brief pause into a cluster of much slower UDP
round trips. Continuously polled driver agents removed the amplification, reducing the same
UDP `SET` p99 from about 157 microseconds to 18–19 microseconds. They did not remove the
underlying store maintenance.

## What we measured

The matrix crosses two independent choices: RESP versus SBE framing, and four local
transports. Each of the eight combinations received a freshly started server and ran
depth-one `PING`, `SET`, and `GET` requests. There was no pipelining to hide a round trip.
Every operation had 10,000 warm-up requests followed by 100,000 measured requests, and the
entire matrix ran twice.

| Transport | Path exercised in this test |
|---|---|
| Shared-memory ring | A 1 MiB Goblin request/reply ring, busy-polled by client and server |
| Aeron IPC | One local C Media Driver shared by client and server |
| Aeron UDP | Separate client and server Media Drivers communicating through Linux UDP loopback |
| UDS | A Unix-domain stream socket handled by the server's socket event loop |

The host, `naamah`, is an AMD Ryzen Threadripper PRO 5995WX with 64 physical cores, 128
logical CPUs, and one NUMA node. The governor was `performance` and boost remained enabled.
The server and measuring client were pinned to CPUs 2 and 3. Aeron client conductors and
Media Driver conductor, receiver, and sender agents occupied distinct physical cores from
CPUs 4 through 11. No CPUs were isolated from the kernel, so these are host-specific
latency measurements rather than a claim about a fully isolated production machine.

The archived runs used Goblin commit
[`d122399`](https://github.com/adamdeprince/goblin-core/commit/d122399a3c78be3a4941f7d4767387b7c070bff2)
and Aeron 1.51.0 at commit
[`9773cba`](https://github.com/aeron-io/aeron/commit/9773cba37e4b88b2b7eb9460c4e0050267b71d28).
Their Media Drivers used `DEDICATED`
threading, which puts conductor, receiver, and sender on separate threads, but retained the
C driver's default `backoff` idle strategies. Dedicated threads and continuous polling are
separate choices.

## The baseline results

These are arithmetic means of the two runs' medians, in microseconds:

| Protocol / transport | PING p50 | SET p50 | GET p50 |
|---|---:|---:|---:|
| RESP / ring | 0.316 | 0.721 | 0.401 |
| SBE / ring | 0.195 | 0.461 | 0.256 |
| RESP / Aeron IPC | 0.496 | 0.907 | 0.626 |
| SBE / Aeron IPC | 0.341 | 0.646 | 0.411 |
| RESP / Aeron UDP loopback | 13.967 | 15.485 | 14.713 |
| SBE / Aeron UDP loopback | 13.926 | 14.112 | 13.977 |
| RESP / UDS | 8.236 | 9.979 | 9.613 |
| SBE / UDS | 7.194 | 7.589 | 7.329 |

The median ordering was ring, Aeron IPC, UDS, then Aeron UDP loopback. Aeron IPC added
about 0.15–0.23 microseconds to the corresponding ring median. SBE was faster than RESP in
all 12 transport/operation pairs. UDP loopback was slower than UDS because it deliberately
used two Media Drivers and crossed the kernel's UDP loopback path; it was not an IPC test
with a UDP-shaped channel name.

The two-run mean p99 exposed the anomaly:

| Protocol / transport | PING p99 | SET p99 | GET p99 |
|---|---:|---:|---:|
| RESP / ring | 0.366 | 0.812 | 0.456 |
| SBE / ring | 0.251 | 0.556 | 0.311 |
| RESP / Aeron IPC | 1.623 | 2.229 | 1.788 |
| SBE / Aeron IPC | 1.533 | 2.009 | 1.603 |
| RESP / Aeron UDP loopback | 20.384 | **157.655** | 22.658 |
| SBE / Aeron UDP loopback | 19.727 | **154.909** | 20.359 |
| RESP / UDS | 11.983 | 13.080 | 12.449 |
| SBE / UDS | 8.261 | 9.328 | 8.436 |

This was not a general UDP tail. It was specific to repeated `SET`, and changing the wire
format barely changed its size. That pointed below the protocol parser.

## The 5,958-request fingerprint

Ordered latency traces made the distribution diagnosable. A large spike recurred at the
same request indices in RESP and SBE, with a period of about 5,958 overwrites. Running the
same trace over the shared-memory ring retained those indices, although the individual
spikes were only roughly 15–35 microseconds rather than a 100–220 microsecond UDP
cluster. The transport was magnifying an event that originated in the store.

The benchmark repeatedly writes the logical value `0123456789abcdef`, which is 16 bytes.
Default string encoding adds a one-byte raw-value tag. Goblin's 16-byte `StringValue`
stores up to 14 encoded bytes inline; a larger value keeps a six-byte prefix inline and
places the rest in the keyspace arena:

| Quantity | Bytes |
|---|---:|
| Logical benchmark value | 16 |
| Encoded value, including raw tag | 17 |
| Inline prefix when spilled | 6 |
| Arena tail allocated by every write | 11 |
| Arena compaction dead-byte floor | 65,536 |

The overwrite path first allocates the replacement tail and then marks the old tail dead.
The same key therefore contributes 11 dead bytes per overwrite. The recurrence falls
straight out of the layout:

```text
ceil(65,536 / 11) = 5,958 overwrites
```

At that point the keyspace arena rebuilds its live keys and spilled values into fresh
storage and updates their tail locations. Smaller staircase-shaped spikes between the
compactions aligned with geometric arena growth and copying. Doubling the Goblin ring from
1 MiB to 2 MiB did not move the indices. Neither disabling replication nor switching to
tcmalloc removed them. Changing an Aeron status-message timeout also left the phase intact.
The value layout and arena accounting were the stable explanation.

A useful boundary test was a 13-byte logical value. With its encoding tag it occupies
exactly 14 bytes and stays inline, so there is no arena tail to retire. Ring `SET` p99.9
fell to 1.713 microseconds for RESP and 0.601 microseconds for SBE. With the historical
backoff Media Driver, UDP p99 fell to 20.589 and 21.901 microseconds respectively. Its
p99.9 could still reach roughly 157 microseconds when ordinary system interruptions left
the drivers idle long enough to back off, but the deterministic `SET` recurrence was gone.

## How Aeron amplified the pause

Aeron agents run a duty cycle: do available work, then apply an idle strategy when there
is none. The C Media Driver defaults its dedicated conductor, sender, and receiver agents
to `backoff`. After a short spin and yield phase, that policy parks for progressively
longer intervals up to one millisecond. It is a sensible CPU-saving default, and Aeron's
[best-practices guide](https://github.com/aeron-io/aeron/wiki/Best-Practices-Guide#idle-strategies)
explicitly presents idle strategy as a responsiveness-versus-CPU tradeoff.

The depth-one request/reply loop normally keeps the two UDP paths in lockstep. Arena
maintenance briefly stops the server from consuming the next request. That creates an
idle window elsewhere in the chain: a Media Driver receiver can stop finding packets, a
sender can stop finding frames, and each can advance independently into backoff. When the
store resumes, work now waits for one or more agents to wake. Because the next depth-one
request depends on the previous reply, the delayed wakeups appear as a contiguous latency
cluster rather than one isolated server-side spike.

Separating the driver strategies confirmed the mechanism. Making only the receiver agents
continuous brought p99 down to roughly 19–20 microseconds and p99.9 to 35–37
microseconds. Making only the senders continuous produced a similar p99, but p99.9 remained
near 100 microseconds. Both directions participated; the receiver side contributed more
to the long cluster.

## The latency profile now polls continuously

The controlled follow-up used the C Media Driver's low-latency profile: conductor `spin`,
sender and receiver `noop`, and at most two messages per network send. Goblin's C++ Aeron
client conductor also uses `spin`, while its response path continuously polls its
subscription. In Aeron terminology, `noop` here means immediately begin the next agent iteration;
it does not turn the sender or receiver off.

| Media Driver policy | RESP SET p99 | RESP p99.9 | SBE SET p99 | SBE p99.9 |
|---|---:|---:|---:|---:|
| Historical default backoff | 157.460 | 216.810 | 157.060 | 216.860 |
| Continuous low-latency profile | **18.435** | **32.562** | **19.206** | **31.880** |

Those figures are a controlled follow-up, not replacements silently spliced into the
original two-run table. The archived baseline remains labeled as a backoff run. The current
`benchmarks/local_transport_latency.sh` launcher defaults to the continuous profile and
records the effective strategies in its metadata; callers can override them when the goal
is to measure a lower-CPU policy.

This tuning has an explicit cost. A dedicated C++ client-conductor core plus each dedicated
Media Driver agent core remains occupied even when traffic is quiet. For IPC there is one
shared local driver. For UDP there is a driver on each endpoint, so the reserved-core cost
is larger. Continuous polling is appropriate for this latency benchmark and for deployments
that intentionally buy latency with cores. It should not be presented as free.

It also does not cure the 5,958-overwrite store event. The ring traces still expose arena
growth and compaction. Eliminating that source requires a store-side change, such as
reusing a compatible existing tail during a same-key overwrite or making reclamation more
incremental. Raising the 64 KiB floor would only postpone the pause.

## Reading the result honestly

There are three conclusions, and keeping them separate matters:

- On this host, the steady-state median ranking was ring, Aeron IPC, UDS, then two-driver
  Aeron UDP loopback. IPC stayed within a fraction of a microsecond of Goblin's native ring.
- The original 157-microsecond UDP `SET` p99 began as deterministic keyspace-arena
  maintenance, not RESP parsing, SBE framing, ring capacity, or an Aeron protocol timer.
- Aeron's default backoff converted a short application pause into a longer transport
  cluster. Continuous polling removed that amplification in exchange for occupied cores;
  the underlying arena work remains visible and is a separate optimization target.

The [archived benchmark report and provenance](../benchmarks/local-transports-naamah-2026-08-23/README.md)
contain both full runs, their aggregate table, hashes, host topology, and reproduction
command. The [Aeron transport guide](../docs/aeron.md) documents the current driver profile
and client behavior. Raw CSVs and metadata live in the
[benchmark artifact directory](https://github.com/adamdeprince/goblin-core/tree/main/benchmarks/local-transports-naamah-2026-08-23).
