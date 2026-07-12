# 14.3 billion Lichess rating updates in one leaderboard

A leaderboard benchmark is easy to make synthetic: invent a few million users,
assign random scores, and time the inserts. This is not that benchmark. This is
nearly thirteen years of people playing chess.

[Lichess publishes its game database](https://database.lichess.org/) as an open,
CC0-licensed archive. The standard-chess records include the date, both players,
and their Glicko2 ratings. We read those records in chronological order, from
December 31, 2012 through September 30, 2025, and turned every valid player rating
into the Redis operation it naturally represents:

```text
ZADD leaderboard <rating> <player>
```

Each rated game contributes up to two observations, one for White and one for
Black. A regular player therefore appears over and over as their rating changes;
a newcomer inserts a new member; an inactive player remains in the set at their
last observed rating. By the end, the trace contains roughly **14.3 billion
rating updates** across **21,598,007 distinct players**, all in one sorted set.

That makes it a real sorted-set workload rather than a large one-time load. The
member population grows organically. Popular players are re-scored repeatedly.
Usernames have real lengths and distributions. Ratings occupy the narrow range
that chess ratings actually use. The ratio of inserts to updates changes over
time as Lichess itself grows.

## Result

All five engines consumed the same chronological trace. The feed used
`redis-cli` over a Unix-domain socket in normal reply-per-command mode, with one
`ZADD` per rating observation. At every date boundary the feed drained, marked
the date, and sampled memory, producing the history in the graph below.

| server | feed time (s) | final OS RSS (MB) |
|---|---:|---:|
| **Goblin Core** | **148,604.246** | **763.8** |
| Redis 8.8 | 160,091.654 | 1,486.7 |
| Redis 7.2.4 | 180,710.011 | 2,058.6 |
| Valkey 9.1 | 158,636.723 | 1,450.7 |
| Dragonfly | 85,545.937 | 1,187.8 |

![Resident memory over nearly thirteen years of Lichess rating updates](blogs/lichess-rss.png)

Goblin Core is the lowest line for the entire history. At the finish it uses:

- **48.6% less memory than Redis 8.8**;
- **47.3% less than Valkey 9.1**;
- **62.9% less than Redis 7.2.4**; and
- **35.7% less than Dragonfly**.

This is the memory result at the scale that matters: not an empty structure or a
carefully selected million-member fixture, but a leaderboard after billions of
real updates. Goblin holds all 21.6 million players in 763.8 MB, about half the
resident memory of current Redis and Valkey.

The shape of the graph matters too. The Redis-family lines rise in allocator and
rehash steps. Goblin's packed representation stays below them as the population
and update history grow. The separation is structural, not a favorable reading
taken at one endpoint.

## Correctness: the same leaderboard

A smaller result is useless if it is the wrong result. After the feed completed,
the harness exported the entire sorted set from Goblin Core and Redis 8.8 using
`ZRANGE leaderboard 0 -1 WITHSCORES` and compared the outputs.

```text
zset check: goblin 'leaderboard' MATCHES redis-8.8  (21598007 members)
```

The member count, ordering, and scores matched across all 21,598,007 players.
The memory comparison is therefore between two representations of the same final
leaderboard, produced by applying the same updates in the same order.

## Speed, including the Dragonfly result

Goblin completed the feed in about **41.3 hours**. It was 7.7% faster than Redis
8.8, 6.8% faster than Valkey 9.1, and 21.6% faster than Redis 7.2.4. The memory
advantage did not require giving up throughput relative to the Redis-family
engines.

Dragonfly was faster: it completed in 85,545.937 seconds, about **1.74x faster
than Goblin**. That result should not be hidden. Dragonfly is designed to spread
work across threads, while Goblin Core's result is the compact, single-engine
path this benchmark set out to measure. If feed completion time is the only
constraint, Dragonfly wins this run. If resident memory is the constraint,
Goblin uses 424 MB less even after the full replay.

There is also a useful human scale for the absolute speed. The trace covers
4,656 elapsed days of play. Goblin replayed those years in 148,604.246 seconds:
**2,707x faster than the games happened in real time**. Even while trailing
Dragonfly, Goblin can rebuild nearly thirteen years of leaderboard history in
less than two days.

## What was measured

The source game records were processed chronologically. For each game, every
valid White or Black rating became a dated `(player, rating)` event. Those events
were then streamed as individual `ZADD` commands. At the end of each date, after
the preceding commands and replies had drained, the stream requested an `INFO
memory` sample. This gave one at-rest memory observation per day rather than a
measurement distorted by queued input or output buffers.

The benchmark harness started each server on its own Unix-domain socket and ran
the five feeds in parallel. Persistence was disabled. Redis and Valkey ran with
active defragmentation enabled and aggressive thresholds so long-lived allocator
fragmentation was reclaimed rather than left as an avoidable handicap. Dragonfly
ran with one proactor thread for the same serving-thread comparison used by the
other Goblin Core benchmark reports.

The reported table uses the operating system's process RSS after the complete
load. During the replay, both self-reported `used_memory_rss` and OS RSS were
collected so their agreement could be checked.

## Why the RSS reporting was patched

The benchmark originally uncovered a measurement problem in the Redis-family
servers. Their `INFO memory` output could report a resident set below the RSS
observed by `ps`, while Goblin and Dragonfly already agreed with the operating
system. Leaving that discrepancy in place would have made Redis and Valkey look
smaller than they really were.

Two reporting issues compounded:

- `used_memory_rss` came from a value cached by the server cron loop, refreshed
  roughly every 100 ms. Reading `INFO` immediately after a burst could therefore
  return a pre-burst value.
- On the benchmark Linux kernel, the `/proc/self/stat` field used by the
  Redis-family RSS reader was systematically below `/proc/self/status` `VmRSS`,
  `/proc/self/statm` resident pages, and `ps -o rss`.

For the isolated benchmark builds only, Redis 7.2.4, Redis 8.8, and Valkey 9.1
were changed to refresh the RSS value when building `INFO memory` and to read
resident pages from `/proc/self/statm`. The corresponding allocated-memory fields
were refreshed at the same instant so fragmentation ratios remained meaningful.

The effect was limited to reporting. No allocator, command implementation,
storage structure, server configuration, or data path was changed. Goblin Core
and Dragonfly were not patched. With the fixes, `used_memory_rss`, `VmRSS`, and
the harness's `ps` reading agreed on the resident-memory metric used in the table.

The direction of the correction is important: it raised the Redis-family numbers
to the operating system's actual resident set. It did not create Goblin's memory
advantage; it removed an artificial advantage from the incumbents.

## Reproducing the feed

The benchmark driver is
[`benchmarks/feed_payload_uds.sh`](benchmarks/feed_payload_uds.sh). It launches
the engines, streams a chronological command payload, records daily memory
samples and final OS RSS, exports the completed leaderboard, and performs the
Goblin-versus-Redis correctness comparison.

The Lichess database is large and continues to grow, so an exact reproduction
must use the same date range: **2012-12-31 through 2025-09-30**. Each input game
must preserve chronological order and emit the White and Black player rating
observations independently. Changing the date range changes the number of games,
players, and updates and is a new benchmark rather than a reproduction of this
one.

For controlled per-operation comparisons and full host configuration, see
[`BENCHMARKS.md`](BENCHMARKS.md). For a shorter explanation of why Goblin's
sorted-set representation stays compact, see
[`blogs/lichess-leaderboard.md`](blogs/lichess-leaderboard.md).
