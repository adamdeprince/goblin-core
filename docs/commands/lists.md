# List commands

Goblin Core's adaptive-PMA and segmented-listpack backends implement
Redis-compatible ordered string lists.
Commands are atomic in the normal single-owner execution path. The storage
design is documented in [Goblin Core Lists](../../LISTS.md).
The measured 100,000-element comparison is in
[List Benchmark](../../LIST-BENCHMARK.md).

Every operation has implementation-qualified names. Standard Redis names
resolve through `--list-implementation pma|segmented`; segmented lists are the
default.

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

The segmented backend exposes the identical suffixes under
`GOBLIN.SEGMENTED.*`: for example, `GOBLIN.SEGMENTED.RPUSH`,
`GOBLIN.SEGMENTED.LINDEX`, and `GOBLIN.SEGMENTED.LINSERT`. A qualified push
selects the representation when it creates a key. Later standard or qualified
list commands operate on that existing representation without conversion.

Negative indexes count back from the tail. `LRANGE` and `LTRIM` use inclusive
end indexes. `LREM` scans from the head for positive counts, from the tail for
negative counts, and removes all matches when count is zero.

Keys are limited to 65,535 bytes. List values use the shared string encoding:
raw values contain up to 65,534 logical bytes, while `--use-lz4` can admit a
larger compressible value when its complete encoding fits 65,535 bytes. A
command with multiple new values validates the entire input before mutating the
list. `--disable-encoding` stores list values verbatim, disables LZ4, and permits
the full 65,535-byte direct value for clients that already use compact binary
data.

The waiting-client commands (`BLPOP`, `BRPOP`, and related moves) are not part of
this initial surface. Redis calls them blocking because an empty-list request
waits for a future producer; this is separate from command atomicity.
