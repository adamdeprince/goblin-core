# GOBLIN.HCAD

```
GOBLIN.HCAD key field expected
```

**Compare-and-delete a hash field.** Delete `field` and reply `1` when it holds a
string equal to `expected`; otherwise leave it in place and reply `0`. It is the
field-level form of [`GOBLIN.CAD`](GOBLIN.CAD.md): the atomic "delete only if it
still holds what I put there" check, applied to one field of a hash instead of a
whole key.

## The idiom it replaces

Deleting a field only when it still holds an expected token — a per-field lock
release, a conditional retraction — is a read-compare-delete that must be atomic,
so in Redis it is a **Lua** script:

```lua
if redis.call("hget", KEYS[1], ARGV[1]) == ARGV[2] then
  return redis.call("hdel", KEYS[1], ARGV[1])
end
return 0
```

`GOBLIN.HCAD key field expected` does exactly this in one atomic C++ op — an
`HGET`, a byte-wise compare, and a conditional `HDEL`. The `redis.call`s become
**direct calls to the store's primitives**, with no interpreter and no command
re-entry.

## Semantics

- The comparison is an exact **byte-for-byte** string match (like `GOBLIN.CAD`),
  not numeric — `"1"` does not match `"1.0"`.
- A **mismatch, an absent field, or an absent key** all reply `0` and change
  nothing.
- Deleting the **last field** of a hash drops the key (an empty hash does not
  linger), matching `HDEL`.
- On a match the reply is `1` (one field deleted), so it is a drop-in for the
  script's `return redis.call("hdel", ...)`.

## Return value

| Situation | Reply |
|---|---|
| `field` holds a string equal to `expected` | `(integer) 1`; the field is deleted |
| `field` holds a different value | `(integer) 0`; nothing changes |
| `field` is absent, or `key` is absent | `(integer) 0` |
| `key` holds a non-hash (string / zset) | `WRONGTYPE` error |

## Examples

A per-field lock release — a worker deletes its slot only if it still owns it:

```
> HSET locks job:7 worker-a
(integer) 1
> GOBLIN.HCAD locks job:7 worker-b
(integer) 0          # not the owner -> the lock is left alone
> GOBLIN.HCAD locks job:7 worker-a
(integer) 1          # owner matches -> released
> HEXISTS locks job:7
(integer) 0
```

Because it deletes only on an exact match, two workers cannot both release the
slot: the second `HCAD` sees a changed (or absent) value and returns `0`, the same
safety `GOBLIN.CAD` gives a whole-key lock.

## See also

- [`GOBLIN.CAD`](GOBLIN.CAD.md) — compare-and-delete on a whole string key (the
  key-level version of this command).
- [`GOBLIN.HSETGT`](GOBLIN.HSETGT.md) — set-if-greater on a hash field, the
  conditional-write companion.
- [Goblin extension commands](goblin.md) — the rest of the `GOBLIN.*` family.
- The idiom scripted in each embedded language: [`EVAL`](EVAL.md) (Lua) and the
  other interpreters — `GOBLIN.HCAD` replaces the `hget` + `hdel` pair with one
  native op.
