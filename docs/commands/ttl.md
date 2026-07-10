# TTL (key expiration) commands

Any key, of any type, can be given a time to live. Expirations live in **one
sparse, keyspace-wide structure** — only keys that have a TTL cost anything —
built as a sorted set with the roles reversed: the *member* is the key (its
48-bit keyspace id) and the *score* is a 48-bit ms-since-epoch expiry. So "what
expires next" is just the minimum, and each entry is 12 bytes.

| Command | Summary |
|---|---|
| [`EXPIRE`](#expire--pexpire) | Set a TTL, in seconds from now. |
| [`PEXPIRE`](#expire--pexpire) | Set a TTL, in milliseconds from now. |
| [`EXPIREAT`](#expireat--pexpireat) | Set an absolute expiry, Unix seconds. |
| [`PEXPIREAT`](#expireat--pexpireat) | Set an absolute expiry, Unix milliseconds. |
| [`TTL`](#ttl--pttl) | Remaining time in seconds. |
| [`PTTL`](#ttl--pttl) | Remaining time in milliseconds. |
| [`EXPIRETIME`](#expiretime--pexpiretime) | Absolute expiry, Unix seconds. |
| [`PEXPIRETIME`](#expiretime--pexpiretime) | Absolute expiry, Unix milliseconds. |
| [`PERSIST`](#persist) | Remove a key's TTL. |

[`SET`](strings.md#set) also takes `EX` / `PX` / `EXAT` / `PXAT` / `KEEPTTL`.

## Expiration model

- **Lazy.** On any access, a key whose expiry has passed is deleted and treated
  as absent — before the command (or the WRONGTYPE check) sees it. A server with
  no TTLs pays nothing for this.
- **Active.** The server sweeps a bounded batch of due keys each event-loop
  iteration, soonest-first, so untouched expired keys are still reclaimed. A
  mass expiry drains across iterations rather than stalling the loop.

## Persistence

TTLs are written to the snapshot in their own `TTL` section as **absolute**
ms-since-epoch, so they survive save/load unchanged (a key reloaded after its
time has passed is reclaimed by the model above).

---

## EXPIRE / PEXPIRE

```
EXPIRE key seconds
PEXPIRE key milliseconds
```

Set the key's TTL relative to now. Reply `1` if set, `0` if the key does not
exist. A non-positive amount deletes the key immediately (and still replies
`1`).

```
> SET session "abc"
OK
> EXPIRE session 60
(integer) 1
> TTL session
(integer) 60
```

## EXPIREAT / PEXPIREAT

```
EXPIREAT key unix-seconds
PEXPIREAT key unix-milliseconds
```

Set an **absolute** expiry. A time already in the past deletes the key (reply
`1`). Reply `0` if the key does not exist.

```
> PEXPIREAT session 1893456000000
(integer) 1
```

## TTL / PTTL

```
TTL key
PTTL key
```

Remaining time to live: in seconds (`TTL`, rounded) or milliseconds (`PTTL`).
Special replies: `-2` if the key does not exist, `-1` if it exists but has no
expiry.

```
> TTL session
(integer) 60
> TTL persistent
(integer) -1
> TTL missing
(integer) -2
```

## EXPIRETIME / PEXPIRETIME

```
EXPIRETIME key
PEXPIRETIME key
```

The **absolute** expiry: Unix seconds (`EXPIRETIME`) or milliseconds
(`PEXPIRETIME`). `-2` if the key does not exist, `-1` if it has no expiry.

## PERSIST

```
PERSIST key
```

Remove the key's TTL so it never expires. Reply `1` if a TTL was removed, `0` if
the key is missing or already had none.

```
> PERSIST session
(integer) 1
> TTL session
(integer) -1
```

## See also

- [strings.md](strings.md) — `SET` and its `EX` / `PX` / `EXAT` / `PXAT` /
  `KEEPTTL` options.
- [keys.md](keys.md) — `DEL`, `EXISTS`, `TYPE`.
