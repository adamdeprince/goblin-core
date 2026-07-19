# Transactions

Goblin Core implements Redis-compatible optimistic transactions over RESP with
`MULTI`, `EXEC`, `DISCARD`, `WATCH`, and `UNWATCH`. A transaction queues complete
commands on one connection, then executes the queue as one atomic block: the
single-threaded server does not service another client between two commands in
the same `EXEC`.

| Command | Summary |
|---|---|
| [`MULTI`](#multi-and-exec) | Begin queuing commands on this connection. |
| [`EXEC`](#multi-and-exec) | Execute the queue atomically and return its ordered results. |
| [`DISCARD`](#discard) | Throw away the queue and all watches. |
| [`WATCH`](#watch-and-unwatch) | Abort the next `EXEC` if any named key changes. |
| [`UNWATCH`](#watch-and-unwatch) | Remove every watch owned by this connection. |

## MULTI and EXEC

```
MULTI
command [argument ...]
...
EXEC
```

`MULTI` replies `OK`. Each valid command that follows is copied into the
connection's fixed transaction mapping and replies `QUEUED`; it does not touch
the store yet. `EXEC` runs the commands in order and returns one array element
per queued command.

```
> MULTI
OK
> SET counter 1
QUEUED
> INCR counter
QUEUED
> GET counter
QUEUED
> EXEC
1) OK
2) (integer) 2
3) "2"
```

An unknown command or wrong arity is a queue-time error. The bad command replies
with its error immediately, later valid commands may still reply `QUEUED`, and
`EXEC` discards the complete transaction with `EXECABORT`.

An error discovered while executing a valid queued command is instead one result
inside the `EXEC` array. Execution continues with the next command. Transactions
do not roll back earlier results.

Nested `MULTI` is rejected without poisoning the existing transaction. `EXEC`
outside `MULTI` is an error.

## DISCARD

```
DISCARD
```

`DISCARD` clears the queued commands, exits transaction mode, removes every key
watched by the connection, and replies `OK`. `DISCARD` outside `MULTI` is an
error. Disconnecting has the same cleanup effect without a reply.

## WATCH and UNWATCH

```
WATCH key [key ...]
UNWATCH
```

`WATCH` calls accumulate. If any watched key is modified before `EXEC`, `EXEC`
does not run the queue and returns a null array in RESP2 or null in RESP3. This
includes writes by the watching connection itself and keys removed by expiration.
Assigning an existing string or hash field counts as a modification even when the
new bytes are equal to the old bytes. A conditional command that does not apply,
or deleting a key that does not exist, does not dirty a watch.

`UNWATCH` removes all watches and replies `OK`. Inside `MULTI`, `UNWATCH` is
queued like an ordinary command. `WATCH` inside `MULTI` is rejected immediately
and does not poison the transaction. Successful `EXEC`, aborted `EXEC`,
`DISCARD`, `UNWATCH`, and disconnect all release watched state.

Watch metadata exists only for actively watched names. Goblin Core does not add
a revision counter to every key: a sparse key-to-connection registry marks
the interested transactions when the store reports a real mutation.

## Fixed transaction memory

Every client owns one anonymous, prefaulted, locked `mmap` allocation for queued
commands. The default is one native page. Set another requested size with:

```sh
goblin-core --transaction-buffer-bytes 65536
```

The requested number is rounded up to a whole number of native pages. The limit
counts the compact queue records, including their command and argument length
fields. The mapping never grows.

When a command cannot fit, Goblin Core marks the transaction failed, keeps
receiving and discarding its remaining commands, and returns a buffer-limit error
for each discarded valid command. `EXEC` returns an `EXECABORT` that names the
rounded byte limit; `DISCARD` can also reset the state. The connection remains
usable after either reset.

## Protocol and transport

These Redis transaction commands use the RESP2 or RESP3 command wire. RESP is
available over TCP, Unix-domain sockets, shared-memory rings, ExaSock, and RDMA
rings. SBE remains a typed, schema-locked protocol and does not define a generic
transaction envelope; use RESP when framework transaction compatibility is the
priority.

## See also

- [Redis transaction contract](https://redis.io/docs/latest/develop/using-commands/transactions/).
- [Native atomic helpers](goblin.md) for common workflows implemented as one C++ command.
- [Scripting commands](README.md) for larger server-side atomic programs.
- [Authentication](../authentication.md) for connection setup before `MULTI`.
