# List commands

Goblin Core's PMA backend implements Redis-compatible ordered string lists.
Commands are atomic in the normal single-owner execution path. The storage
design is documented in [PMA Lists](../../LISTS.md).
The measured 100,000-element comparison is in
[List Benchmark](../../LIST-BENCHMARK.md).

Every PMA operation has an implementation-qualified name. Standard Redis names
resolve through `--list-implementation pma`, which is the default and currently
the selector enum's sole value.

| Command | Syntax | Reply |
|---|---|---|
| `GOBLIN.PMA.LPUSH` | `GOBLIN.PMA.LPUSH key value [value ...]` | list length |
| `GOBLIN.PMA.RPUSH` | `GOBLIN.PMA.RPUSH key value [value ...]` | list length |
| `GOBLIN.PMA.LPUSHX` | `GOBLIN.PMA.LPUSHX key value [value ...]` | list length, or `0` if absent |
| `GOBLIN.PMA.RPUSHX` | `GOBLIN.PMA.RPUSHX key value [value ...]` | list length, or `0` if absent |
| `GOBLIN.PMA.LPOP` | `GOBLIN.PMA.LPOP key [count]` | value, values, or nil |
| `GOBLIN.PMA.RPOP` | `GOBLIN.PMA.RPOP key [count]` | value, values, or nil |
| `GOBLIN.PMA.LLEN` | `GOBLIN.PMA.LLEN key` | element count |
| `GOBLIN.PMA.LINDEX` | `GOBLIN.PMA.LINDEX key index` | value or nil |
| `GOBLIN.PMA.LRANGE` | `GOBLIN.PMA.LRANGE key start stop` | values in the inclusive range |
| `GOBLIN.PMA.LSET` | `GOBLIN.PMA.LSET key index value` | `OK`, or an index/key error |
| `GOBLIN.PMA.LTRIM` | `GOBLIN.PMA.LTRIM key start stop` | `OK` |
| `GOBLIN.PMA.LREM` | `GOBLIN.PMA.LREM key count value` | number removed |
| `GOBLIN.PMA.LINSERT` | `GOBLIN.PMA.LINSERT key BEFORE\|AFTER pivot value` | new length, `0`, or `-1` |

Negative indexes count back from the tail. `LRANGE` and `LTRIM` use inclusive
end indexes. `LREM` scans from the head for positive counts, from the tail for
negative counts, and removes all matches when count is zero.

Keys and list values are limited to 65,535 bytes. A command with multiple new
values validates the entire input before mutating the list.

The waiting-client commands (`BLPOP`, `BRPOP`, and related moves) are not part of
this initial surface. Redis calls them blocking because an empty-list request
waits for a future producer; this is separate from command atomicity.
