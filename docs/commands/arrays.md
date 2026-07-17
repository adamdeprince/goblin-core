# Arrays (AR*)

Index-addressable arrays. Element bytes live in a page-aligned value arena (not
`std::string`). Classic sparse leaves store packed `(offset, value_id)` entries
(6 bytes each). Dense and realtime leaves use 32-bit value ids with
`0xFFFFFFFF` as empty.

## Implementations

| Family | Behavior |
|--------|----------|
| **Classic** (default) | Redis-8.8-style: sparse leaves; sparse↔dense under load; **auto directory depth** when an index outgrows capacity |
| **Realtime** | Fixed depth and fixed-capacity dense leaves. A touched leaf allocates its final table once; a reserved leaf allocates before serving. Index or reservation exhaustion → **error** (no stop-the-world rebuild). |

### Selecting an implementation

| Command form | Creates / targets |
|--------------|-------------------|
| `AR*` | Process default: **Classic**, unless `--realtime-arrays` |
| `GOBLIN.CLASSIC.AR*` | Always Classic (overrides the flag) |
| `GOBLIN.RT.AR*` | Always Realtime (overrides the flag) |

Both families can be used in the **same session**. The first write that creates a
key pins its representation via that command family (and the store defaults).

**Note:** `GOBLIN.CLASSIC.*` is **array-only**. Hash families remain
`GOBLIN.EFFICENT.H*` / `GOBLIN.RT.H*` (separate product names).

## CLI (shared geometry)

```
--array-slice-slots N       # power of two, leaf width ≤ 65536 (default 4096)
--array-initial-depth D     # Classic: start depth; RT: hard depth (default 1)
--array-chunk-bytes BYTES   # value arena chunk size
--rt-array-arena-growth N   # RT value-arena growth (default 2.0)
--realtime-arrays           # bare AR* → Realtime
```

Capacity (max inclusive index):

```
slice_slots × fanout^depth − 1
```

Fanout defaults to `slice_slots`.

## Commands

```
ARSET key index value [value ...]
ARGET key index
ARMSET key index value [index value ...]
ARMGET key index [index ...]
ARLEN key
ARCOUNT key
ARDEL key index [index ...]
ARINSERT key value [value ...]
ARNEXT key
ARSEEK key index
GOBLIN.RT.ARRESERVE key max-index value-slots encoded-bytes
```

The regular verbs exist under `GOBLIN.RT.` and `GOBLIN.CLASSIC.` prefixes.
`ARRESERVE` is deliberately RT-qualified: Classic arrays retain their normal
memory-oriented growth policy.

## Realtime reservation

`GOBLIN.RT.ARRESERVE` prepares a new or currently unreserved RT key outside the
latency-sensitive path. Existing live values are rebuilt into the reserved
layout. It allocates and touches every fixed leaf through `max-index`, metadata
for `value-slots` simultaneous stored-value IDs, and the `encoded-bytes`
value-arena address budget. The byte budget covers encoded representations,
including the raw-string marker and any chunk-boundary padding.

After reservation, `ARSET`, `ARMSET`, and `ARINSERT` do not grow these
structures. Exceeding the reserved index, value-slot, or byte budget returns an
error. An update retains its stable value ID, so it needs no additional value
slot, but it does append another encoded representation. Provision the byte
budget for mutation volume as well as the initial population.

Deleting every element keeps a reserved key and its prefaulted capacity alive.
Reservation is not serialized in snapshots; it is deployment provisioning and
must be re-established after loading an RT array.

## Errors (Realtime)

- Index beyond fixed capacity →
  `ERR array index exceeds fixed RT capacity (raise --array-slice-slots / --array-initial-depth)`
- Reserved index range, value-slot capacity, or byte arena exhausted → an
  `ERR RT array reserved ... exhausted` reply. The command does not fall back to
  allocation.

## Introspection

- `TYPE key` → `array`
- `INFO` → `array_implementation:classic|realtime` (process default)
- `GOBLIN.MEMORY key` → per-array breakdown (implementation, slices, value arena, depth)

## Persistence

`GOBLIN.SAVE` / `GOBLIN.LOAD` include an **Array** snapshot section (type 7):

- Canonical payload: implementation, geometry, insert cursor, live `(index, value)` pairs
- Leaf tables and the value arena rebuild on load (no accelerator yet)
- Older loaders that do not know the section type skip it by length framing
- Realtime keys restore fixed depth; indexes that would not fit still fail load
- Reservation state is not persisted; snapshots restore live data using the RT
  geometry but not a serving-time capacity promise
