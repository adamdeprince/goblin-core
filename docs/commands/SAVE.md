# SAVE

Synchronously write a point-in-time native Goblin Core snapshot.

```text
SAVE [path [ACCEL | NOACCEL]]
```

`SAVE` does not return until the snapshot has been written, fsynced, and
atomically renamed into place. A successful `OK` reply therefore means the final
file is ready to load or copy. The default path is `dump.gcsn`, relative to the
server's working directory.

The server executes commands atomically on one serving thread. While `SAVE`
serializes and installs the snapshot, no other command is interleaved and the
server does not serve other clients. Use [`BGSAVE`](BGSAVE.md) when the process
can safely fork and continuing to serve during a large snapshot matters.

## Snapshot contents

The native GCSN image contains every persistent key type, TTLs, the replication
lineage and logical offset, and the exact acknowledged Kafka offset when one is
known. Restart with `--load path`, or load the image explicitly with
`GOBLIN.LOAD path`.

`ACCEL` is the default. It adds same-build index accelerators so routine restarts
can copy compatible packed structures instead of rebuilding them. `NOACCEL`
emits the smaller canonical image intended for movement between Goblin Core
versions, architectures, or C++ standard-library builds. Both forms contain the
same logical database.

## Why choose SAVE

Use `SAVE` when the caller must know that the file is durable before proceeding,
for example:

- immediately before a planned process replacement;
- before copying a snapshot to another host;
- from a deployment script that must fail if the snapshot cannot be installed;
- when `--arena-hugetlb` is active;
- when an active native transport runtime cannot safely survive `fork()`.

Unlike [`BGSAVE`](BGSAVE.md), `SAVE` does not create a copy-on-write child and
does not temporarily increase memory through fork-time page copies. The tradeoff
is that snapshot serialization blocks the serving thread.

`SAVE` returns an error instead of racing an active `BGSAVE` or
[`GOBLIN.DUMPWORLD`](goblin.md#goblin-dumpworld) child. Retry after the
background snapshot has completed.

## Reply

- `OK` means the final snapshot path has been fsynced and atomically installed.
- `ERR background snapshot already in progress` means another fork-time snapshot
  owns the snapshot slot.
- `ERR snapshot save failed` means the file could not be written, synced, or
  installed.

## Examples

Use the default accelerated format and path:

```sh
redis-cli SAVE
```

Install a known file before replacing the process:

```sh
redis-cli SAVE /var/lib/goblin/state.gcsn ACCEL
systemctl restart goblin-core
```

Create a canonical migration image:

```sh
redis-cli SAVE /var/lib/goblin/portable.gcsn NOACCEL
```

## Compatibility and aliases

The synchronous command name and completion semantics match Redis. Goblin Core
extends the syntax with an optional server-side path and `ACCEL` / `NOACCEL`
format selection.

`GOBLIN.SAVE` is an exact alias retained for existing Goblin Core clients. New
documentation and deployments should use `SAVE`. Typed SBE clients expose the
same operation through `save()`; SBE clients and servers must be exactly the
same version. Use RESP when version compatibility is required.

## See also

- [`BGSAVE`](BGSAVE.md) - forked background snapshots.
- [`GOBLIN.DUMPWORLD`](goblin.md#goblin-dumpworld) - stream the same snapshot
  format to a RESP3 client.
- [Kafka write log and recovery](../kafka.md) - combine compact snapshots with
  an external durable mutation log.
- [SBE protocol](../sbe-protocol.md) - typed same-version client behavior.
