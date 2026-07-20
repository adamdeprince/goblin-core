# Hard memory ceiling

Goblin Core can put a deterministic ceiling on memory committed to the
persistent data store:

```sh
goblin-core --maxmemory 8gb
```

`--maxmemory` accepts an exact byte count or the binary suffixes `k`/`kb`,
`m`/`mb`, and `g`/`gb`. `0`, or omitting the option, means unlimited.

## No eviction

The only policy is `noeviction`. Before a write grows an arena block, index,
directory, object table, TTL table, or compact-blob pool, Goblin reserves that
capacity against the command's memory budget. If the allocation would cross the
configured ceiling, the command is rejected:

```text
-OOM command not allowed when used memory exceeds 'maxmemory'.
```

SBE returns the same condition as an `ErrorReply` with code `OOM`. Goblin does
not evict another key, sample candidates, or perform hidden policy work on this
path. Deletes and writes that fit entirely within capacity already committed to
the store remain available at the ceiling.

The initial store footprint must fit. This matters for `--real-time`, whose RT
hash and keyspace indexes are deliberately allocated and prefaulted before the
server begins listening. Startup fails rather than silently exceeding a limit
smaller than those configured pools.

Snapshot and RDB loads use the same ceiling. A load that cannot fit fails and
leaves the store empty rather than opening listeners with a partial database.

## What is counted

The ceiling uses the same `used_memory` value exposed by `INFO memory`:

- committed key, value, hash, set, sorted-set, list, and array arenas;
- index, directory, object-slot, score, rank, and TTL table capacity;
- fixed prefaulted RT index pools; and
- actual upstream capacity held by the compact-blob pool, including retained
  size-class slack.

This is a data-store ceiling, not an RSS cgroup. Executable mappings, thread
stacks, client input/output pages, transient command replies, scripting VMs,
TLS, Kafka, and operating-system allocator metadata are outside `used_memory`.
Those bounded subsystems retain their own limits; use an OS service or cgroup
limit as the final process-wide guardrail.

## Observability

```text
INFO memory
CONFIG GET maxmemory maxmemory-policy
```

Both report the configured byte value and `noeviction`. `CONFIG GET` is
read-only; change the ceiling by restarting Goblin Core with a different
`--maxmemory` value.
