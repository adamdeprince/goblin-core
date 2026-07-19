# ZREMRANGEBYSCORE

```
ZREMRANGEBYSCORE key min max
```

**Remove every member of a sorted set whose score falls in `[min, max]`.** Replies
with the number of members removed. If removing them empties the set, the key is
deleted. A missing key removes nothing and replies `0`; a key of another type is a
`WRONGTYPE` error. This is the standard Redis command, with the same score-bound
syntax.

## Score bounds

`min` and `max` use Redis's `ZRANGEBYSCORE` bound syntax:

| Form | Meaning |
|---|---|
| `5` | inclusive — scores `≥ 5` (for `min`) or `≤ 5` (for `max`) |
| `(5` | **exclusive** — scores strictly `> 5` / `< 5` |
| `-inf` | negative infinity — no lower bound |
| `+inf` / `inf` | positive infinity — no upper bound |

`ZREMRANGEBYSCORE key -inf +inf` therefore clears the whole set (and deletes the
key). A bound that is neither a number nor an infinity replies `ERR min or max is
not a float`. As in Redis, an empty range (`min > max`) simply removes nothing.

## Implementation

The removal is a seek on the sorted set's **score index**, not a scan: it locates
the first block at or after `min` and walks forward until it passes `max`, so the
cost is O(log n + removed) rather than O(n). Tiny sets stored in the compact
listpack representation take the same path over the packed entries. After a large
removal the set auto-compacts its arenas and repacks its member index, the same
maintenance [`GOBLIN.OPTIMIZE`](goblin.md) performs on demand.

## Return value

| Situation | Reply |
|---|---|
| members removed | `(integer)` the count |
| range matched nothing, or key absent | `(integer) 0` |
| removal emptied the set | the count, and the key is deleted |
| `key` holds a non-zset (string / hash) | `WRONGTYPE` error |
| `min` or `max` not a valid bound | `ERR min or max is not a float` |

## Examples

```
> ZADD scores 1 a 2 b 3 c 4 d 5 e
(integer) 5
> ZREMRANGEBYSCORE scores 2 4
(integer) 3          # b, c, d removed
> ZRANGE scores 0 -1 WITHSCORES
1) "a"
2) "1"
3) "e"
4) "5"
> ZREMRANGEBYSCORE scores (1 +inf
(integer) 1          # e (score 5 > 1); a stays (score 1 is excluded by "(1")
> ZREMRANGEBYSCORE scores -inf +inf
(integer) 1          # a removed; set now empty, key deleted
> EXISTS scores
(integer) 0
```

## See also

- [`GOBLIN.ZWINDOW`](GOBLIN.ZWINDOW.md) — the sliding-window limiter that uses
  `ZREMRANGEBYSCORE` to evict expired entries before admitting a request.
- [Sorted-set command reference](sorted-sets.md) — `ZADD`, score and rank
  ranges, multi-score reads, endpoint pops, and scans.
