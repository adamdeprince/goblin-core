# Sorted sets

Goblin Core sorted sets keep one unique member string per score and order entries
by score, then by member bytes when scores tie. They are the primary building
block for leaderboards, priority queues, time windows, and score-indexed work.

## Command surface

| Command | Purpose |
|---|---|
| `ZADD` | Add members, update scores, or conditionally increment one score. |
| `ZINCRBY` | Increment one member's score, creating it when absent. |
| `ZCARD` | Return the number of members. |
| `ZSCORE`, `ZMSCORE` | Read one or several scores without changing the set. |
| `ZRANK`, `ZREVRANK` | Read a member's zero-based ascending or descending rank. |
| `ZRANGE`, `ZRANGEBYSCORE`, `ZREVRANGEBYSCORE` | Read rank or score ranges. |
| `ZCOUNT` | Count members inside a score interval. |
| `ZREM`, `ZREMRANGEBYSCORE` | Remove named members or a score interval. |
| `ZPOPMIN`, `ZPOPMAX` | Remove and return members at one score endpoint. |
| `ZSCAN` | Incrementally visit members and scores. |

Missing keys behave as empty sorted sets. A key holding another type returns
`WRONGTYPE` for every command on this page.

## Scores and bounds

Scores are binary `double` values. `-inf`, `+inf`, and `inf` are accepted;
`nan` is rejected. Equal scores are ordered lexicographically by the member's
binary bytes, making ties deterministic.

Score ranges accept inclusive bounds by default. Prefix a finite number or an
infinity with `(` to make it exclusive:

| Bound | Meaning as a lower bound |
|---|---|
| `10` | score >= 10 |
| `(10` | score > 10 |
| `-inf` | no lower limit |
| `+inf` | positive infinity itself |

The same notation applies to an upper bound with `<=` or `<` respectively.

## ZADD and ZINCRBY

```text
ZADD key [NX | XX] [GT | LT] [CH] [INCR] score member [score member ...]
ZINCRBY key increment member
```

`ZADD` applies pairs from left to right and returns the number of newly added
members. `CH` changes that reply to the number of additions plus actual score
changes. Supplying a member more than once in one command therefore observes
the preceding pair, just as separate atomic updates would.

| Option | Effect |
|---|---|
| `NX` | Add absent members only. |
| `XX` | Update existing members only. |
| `GT` | Update only when the new score is greater. Absent members are still added unless `XX` is also present. |
| `LT` | Update only when the new score is lower. Absent members are still added unless `XX` is also present. |
| `CH` | Count every score change, not only new members. |
| `INCR` | Treat the score as an increment and return the resulting score. Exactly one pair is required. A failed condition returns null. |

`NX` is incompatible with `XX`, `GT`, and `LT`; `GT` and `LT` are mutually
exclusive. `ZINCRBY` is the unconditional single-member increment form. A
resulting NaN, such as adding opposite infinities, is rejected without changing
the member.

## Rank and score ranges

```text
ZRANGE key start stop [REV] [WITHSCORES]
ZRANGE key min max BYSCORE [REV] [LIMIT offset count] [WITHSCORES]
ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count]
ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count]
ZCOUNT key min max
```

Rank ranges use inclusive, zero-based indexes. Negative indexes count from the
end. `REV` reverses the ordering before applying rank indexes.

With `BYSCORE`, `REV` also reverses the argument order: the first argument is
the maximum and the second is the minimum. `LIMIT` skips `offset` matching
members and returns at most `count`; a negative count means no limit. The legacy
`ZRANGEBYSCORE` and `ZREVRANGEBYSCORE` names remain available for existing
clients.

`WITHSCORES` returns flat member/score elements in RESP2 and member/double pairs
in RESP3. Typed SBE returns member/native-double groups without formatting or
parsing scores as text.

Score ranges and `ZCOUNT` seek directly into the large-set score index, then
walk only the selected entries. Their work is proportional to the index seek
plus the returned range, rather than to the full set size.

## Multi-score reads and endpoint pops

```text
ZMSCORE key member [member ...]
ZPOPMIN key [count]
ZPOPMAX key [count]
```

`ZMSCORE` preserves input order and returns null for each missing member.
`ZPOPMIN` and `ZPOPMAX` atomically remove up to `count` entries from the selected
endpoint and return each member with its score. The default count is one. A zero
count returns an empty array.

## Incremental scan

```text
ZSCAN key cursor [MATCH pattern] [COUNT count]
```

Start with cursor `0` and continue with the returned cursor until it becomes
`0` again. The cursor is opaque: do not interpret it, persist it across server
versions, or compare it with another Redis implementation. `COUNT` controls the
amount of index work requested per call and is a hint for pagination; `MATCH`
filters the visited members with Redis glob syntax.

See the [bounded-iteration reference](iteration.md) for the reply contract shared
with `SCAN`, `HSCAN`, and `SSCAN`.

As with Redis scan commands, callers should tolerate duplicate results if the
set is mutated during a traversal. A traversal over an unchanged set visits all
members.

## Storage

Sets with at most 32 entries use one compact listpack blob. When the next entry
arrives, Goblin promotes the complete ordered set to its arena-backed member
storage, fingerprinted member index, and blocked score index. Member strings do
not move through command replay during promotion. Large-set score seeks use the
blocked index; member lookups use the fingerprinted index.

Bulk removals and pops reuse the same removal and compaction policy as `ZREM`.
When the final member is removed, the key itself is deleted. Use
[`GOBLIN.MEMORY`](goblin.md#goblin-memory) to inspect the score width, arena
allocation, member index, score index, and optional rank-cache costs for one
set.

## Example

```text
> ZADD leaderboard NX 1250 alice 980 bob 1410 carol
(integer) 3
> ZINCRBY leaderboard 350 bob
"1330"
> ZRANGE leaderboard +inf 1000 BYSCORE REV LIMIT 0 2 WITHSCORES
1) "carol"
2) "1410"
3) "bob"
4) "1330"
> ZCOUNT leaderboard (1200 +inf
(integer) 3
> ZPOPMAX leaderboard
1) "carol"
2) "1410"
```

See [`ZREMRANGEBYSCORE`](ZREMRANGEBYSCORE.md) for interval-removal details and
[`GOBLIN.ZWINDOW`](GOBLIN.ZWINDOW.md) for the native sliding-window operation.
