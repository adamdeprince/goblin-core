# BGSAVE

Fork a point-in-time snapshot child and continue serving commands in the parent.

```text
BGSAVE [path [ACCEL | NOACCEL]]
```

The snapshot is the database state at the successful `fork()`. The parent
replies `Background saving started` as soon as the child owns that frozen
copy-on-write view; the reply does **not** mean the file is complete. The child
writes a temporary file, fsyncs it, and atomically renames it to the final path.
Completion or failure is reported in the server log.

The default path is `dump.gcsn`, relative to the server's working directory.
`ACCEL` is the default routine-restart format; `NOACCEL` writes the smaller
canonical migration format. See [`SAVE`](SAVE.md#snapshot-contents) for the
contents and format contract.

## Copy-on-write memory behavior

The child initially shares the parent's pages. Parent writes after the fork are
not included in the image and may cause the kernel to copy the pages they touch.
Goblin Core reduces avoidable amplification while the child is alive:

- keyspace, hash, list, set, array, and sorted-set arena compaction is deferred;
- `GOBLIN.OPTIMIZE` does not rewrite an arena during that interval;
- normal mutations continue;
- a structure may allocate another arena block when its current block fills;
- deferred compaction becomes eligible again after the child is reaped.

This prevents a maintenance pass from rewriting most live bytes solely to
reclaim fragmentation while the old pages are still retained for the snapshot.
It does not make fork copy-on-write free: ordinary mutations can still dirty
shared pages. Size the process with enough headroom for the write rate and
snapshot duration.

Only one fork-time snapshot may run at once. `BGSAVE` and
[`GOBLIN.DUMPWORLD`](goblin.md#goblin-dumpworld) reject one another while either
child is active. A synchronous [`SAVE`](SAVE.md) request is also rejected until
that child exits.

## When BGSAVE is unavailable

Goblin Core rejects `BGSAVE` when `--arena-hugetlb` is active. A post-fork write
could otherwise copy an entire huge page, rapidly consume the reserved HugeTLB
pool, and terminate the process. Use synchronous [`SAVE`](SAVE.md) in that
configuration.

`BGSAVE` is also rejected while a native transport runtime that cannot safely
survive `fork()` is active. Native XLIO Ultra is one such runtime. Synchronous
`SAVE` remains available because it does not fork.

## Reply

- `Background saving started` means the child started successfully.
- `ERR background snapshot already in progress` means another `BGSAVE` or
  `GOBLIN.DUMPWORLD` child owns the snapshot slot.
- `ERR cannot fork for background save` means the operating-system fork failed.
- An error directing the caller to `SAVE` means HugeTLB or an active transport
  makes background fork unsafe.

An error that occurs after the initial success reply is reported in the server
log. Automation that requires a definite installed file before its next step
should use synchronous [`SAVE`](SAVE.md).

## Examples

Start an accelerated background snapshot:

```sh
redis-cli BGSAVE /var/lib/goblin/state.gcsn ACCEL
```

Start a canonical background snapshot:

```sh
redis-cli BGSAVE /var/lib/goblin/portable.gcsn NOACCEL
```

For scheduled snapshots on ordinary pages, invoke `BGSAVE` from the service
scheduler and monitor the Goblin Core log. Goblin Core does not own an internal
periodic-save schedule; `CONFIG GET save` therefore remains empty.

## Compatibility and aliases

The background command name and fork-at-command-boundary behavior match Redis.
Goblin Core extends the syntax with an optional server-side path and `ACCEL` /
`NOACCEL` format selection.

`GOBLIN.BGSAVE` is an exact alias retained for existing Goblin Core clients. New
documentation and deployments should use `BGSAVE`. Typed SBE clients expose the
same operation through `bgsave()`; SBE clients and servers must be exactly the
same version. Use RESP when version compatibility is required.

## See also

- [`SAVE`](SAVE.md) - synchronous completion without fork-time COW.
- [`GOBLIN.DUMPWORLD`](goblin.md#goblin-dumpworld) - stream a fork-time native
  snapshot to a RESP3 client.
- [HugeTLB behavior](../../TLB-BENCHMARK.md) - why background snapshots are
  disabled for HugeTLB-backed arenas.
- [Native XLIO Ultra](../xlio.md) - fork restrictions for XLIO polling groups.
