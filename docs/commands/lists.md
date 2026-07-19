# List commands

Goblin Core's adaptive-PMA and segmented-listpack backends implement
Redis-compatible ordered string lists.
Commands are atomic in the normal single-owner execution path. The storage
design is documented in [Goblin Core Lists](../../LISTS.md).
The measured 100,000-element comparison is in
[List Benchmark](../../LIST-BENCHMARK.md). A local same-server comparison of
the queue commands is in
[List Work-Queue Benchmark](../../LIST-WORK-QUEUE-BENCHMARK.md).

Every operation has implementation-qualified names. Standard Redis names
resolve through `--list-implementation pma|segmented`; segmented lists are the
default. Prefix any command in the table with `GOBLIN.PMA.` or
`GOBLIN.SEGMENTED.` to address one implementation directly.

| Command | Syntax | Reply |
|---|---|---|
| `LPUSH` | `LPUSH key value [value ...]` | list length |
| `RPUSH` | `RPUSH key value [value ...]` | list length |
| `LPUSHX` | `LPUSHX key value [value ...]` | list length, or `0` if absent |
| `RPUSHX` | `RPUSHX key value [value ...]` | list length, or `0` if absent |
| `LPOP` | `LPOP key [count]` | value, values, or nil |
| `RPOP` | `RPOP key [count]` | value, values, or nil |
| `LMOVE` | `LMOVE source destination LEFT/RIGHT LEFT/RIGHT` | moved value or nil |
| `RPOPLPUSH` | `RPOPLPUSH source destination` | moved value or nil |
| `BLPOP` | `BLPOP key [key ...] timeout` | selected key and value, or nil |
| `BRPOP` | `BRPOP key [key ...] timeout` | selected key and value, or nil |
| `BLMOVE` | `BLMOVE source destination LEFT/RIGHT LEFT/RIGHT timeout` | moved value or nil |
| `LMPOP` | `LMPOP numkeys key [key ...] LEFT/RIGHT [COUNT count]` | selected key and values, or nil |
| `BLMPOP` | `BLMPOP timeout numkeys key [key ...] LEFT/RIGHT [COUNT count]` | selected key and values, or nil |
| `LLEN` | `LLEN key` | element count |
| `LINDEX` | `LINDEX key index` | value or nil |
| `LRANGE` | `LRANGE key start stop` | values in the inclusive range |
| `LSET` | `LSET key index value` | `OK`, or an index/key error |
| `LTRIM` | `LTRIM key start stop` | `OK` |
| `LREM` | `LREM key count value` | number removed |
| `LINSERT` | `LINSERT key BEFORE/AFTER pivot value` | new length, `0`, or `-1` |

The segmented backend exposes the identical suffixes under
`GOBLIN.SEGMENTED.*`: for example, `GOBLIN.SEGMENTED.RPUSH`,
`GOBLIN.SEGMENTED.LINDEX`, and `GOBLIN.SEGMENTED.LINSERT`. A qualified push
selects the representation when it creates a key. Later standard or qualified
list commands operate on that existing representation without conversion.

Negative indexes count back from the tail. `LRANGE` and `LTRIM` use inclusive
end indexes. `LREM` scans from the head for positive counts, from the tail for
negative counts, and removes all matches when count is zero.

`LMOVE` atomically removes one endpoint and inserts it at either endpoint of the
destination; using one key rotates that list. `RPOPLPUSH` is the compatible
`RIGHT`-to-`LEFT` form. `LMPOP` selects the first non-empty key in argument order
and removes one value by default or up to the positive `COUNT`.

`BLPOP`, `BRPOP`, `BLMOVE`, and `BLMPOP` first attempt the corresponding
operation immediately. If no source is ready, only that client is parked; the
single-owner server continues processing other clients and every command stays
atomic. Waiters are served FIFO, while keys within one request are checked in
argument order. The timeout is a non-negative number of seconds and may be
fractional; zero waits indefinitely. A blocking command executed by a script or
inside `EXEC` uses its non-blocking form and returns nil when no item is ready.
The server does not consume later pipelined commands from that connection until
the blocked reply has been produced. See the
[Redis list command surface](https://redis.io/docs/latest/develop/data-types/lists/)
for the compatible queue patterns.

Keys are limited to 65,535 bytes. List values use the shared string encoding:
raw values contain up to 65,534 logical bytes, while `--use-lz4` can admit a
larger compressible value when its complete encoding fits 65,535 bytes. A
command with multiple new values validates the entire input before mutating the
list. `--disable-encoding` stores list values verbatim, disables LZ4, and permits
the full 65,535-byte direct value for clients that already use compact binary
data.
