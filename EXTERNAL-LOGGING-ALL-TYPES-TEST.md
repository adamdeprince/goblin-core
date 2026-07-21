# External Logging: All Persistent Types

The first external-logging test proved that a million-field hash could survive
a native snapshot, a Kafka-journaled suffix, and `SIGKILL`. The next test made
the persistence boundary broad instead of deep: strings, hashes, sets, sorted
sets, lists, and sparse arrays all crossed the same save, crash, load, continued
write, and exhaustive verification cycle.

Pub/Sub is not included because it has no persistent value to save. This test
used the default efficient hash, segmented list, and classic array
implementations.

## Commands under test

Thunder's systemd unit started Goblin Core with this exact argument vector:

```sh
/usr/local/bin/goblin-core \
  --port 6379 \
  --numa eno1 \
  --load /mnt/local/goblin-core/state/goblin.snapshot \
  --kafka kafka://192.168.1.49:9092/goblin-core-replication
```

`--load` restores the native snapshot. `--kafka` first consumes the named
single-partition topic to its high watermark and then journals new primary
writes to the same topic. Goblin Core does not open its client listeners until
startup replay is complete.

The test itself was selected entirely from its command line:

```sh
benchmarks/external_logging_recovery.sh \
  --data-type all \
  --snapshot-at 500000 \
  --count 1000000 \
  --pipeline 512
```

The same runner can isolate one type, for example:

```sh
benchmarks/external_logging_recovery.sh --data-type list
benchmarks/external_logging_recovery.sh --data-type zset --pipeline 256
```

## Workload

Each ordinal was stored in the native shape of its type:

| Type | Write | Verification |
|---|---|---|
| String | `SET external-logging-test:string:N N` | `GET` |
| Hash | `HSET external-logging-test:hash N N` | `HLEN`, `HGET` |
| Set | `SADD external-logging-test:set N` | `SCARD`, `SISMEMBER` |
| Sorted set | `ZADD external-logging-test:zset N N` | `ZCARD`, `ZSCORE` |
| List | `RPUSH external-logging-test:list N` | `LLEN`, `LINDEX` |
| Array | `ARSET external-logging-test:array N N` | `ARLEN`, `ARGET` |

The runner cleared this fixed test namespace before starting; cleanup was
outside the timed phases. Decimal ordinals traveled through the normal client
protocol with Goblin Core's value encoding enabled.

The C++ client used RESP2 over TCP to `127.0.0.1:6379`, pipelined 512 commands
deep. Goblin Core used Kafka over TCP to Redpanda at
`192.168.1.49:9092`. Both services ran on the same Dell PowerEdge R820 described
in [the first test](EXTERNAL-LOGGING-FIRST-TEST.md#test-machine), and Redpanda
remained limited to four cores and 8 GiB in production mode.

The runner wrote 500,000 entries of every type, for three million selected
values at the boundary, then requested an accelerated native snapshot. It
waited for the temporary snapshot to be fsynced and atomically renamed, sent
`SIGKILL` to Goblin Core, and did not construct a restart command. Systemd
restarted the unchanged unit.

Once the listener reopened, the client exhaustively verified all three million
recovered values before it was allowed to continue. It then wrote ordinals
`500000` through `999999` for every type and checked all six million final
values.

## Write throughput while logging

Every write was converted into a canonical RESP2 mutation and submitted to the
idempotent, `acks=all` Redpanda producer. These are pipelined throughput rates,
not single-command latency or synchronous-fsync rates.

| Type | First 500K ops/s | Continued 500K ops/s |
|---|---:|---:|
| String | 188,836 | 173,840 |
| Hash | 99,677 | 190,972 |
| Set | 219,961 | 214,586 |
| Sorted set | 162,370 | 163,722 |
| List | 221,859 | 198,396 |
| Array | 246,508 | 239,744 |
| **Sequential aggregate** | **173,345** | **193,746** |

The first three million writes took 17.31 seconds. The three million continued
writes took 15.48 seconds. The hash phase changed shape substantially across
the boundary; this persistence test does not isolate whether table growth,
promotion, allocator state, or producer backlog caused that difference, so it
should not be treated as a standalone HSET benchmark.

## Crash and recovery

The snapshot was 102,229,100 bytes (97.5 MiB). This was not an empty-server size
measurement: it also contained the million-field `foo` hash retained from the
first recovery test and two small pre-existing keys. The startup report loaded
3.5 million collection members across 500,008 keys.

Systemd observed exit status `9/KILL`, waited its configured one second, and
started a replacement process. Goblin Core loaded the snapshot, resumed Kafka
inclusively at broker offset `5998696` after logical offset `6000009`, filtered
the already represented boundary, and reopened its listener **5.72 seconds**
after the kill.

The systemd unit definition and the raw NUL-delimited process command line had
identical hashes before and after:

```text
86199041dabad28a6388075bd9be9b7fe61d0408f53e574a55ee341c38bd9ee3
```

## Verification

All recovered halves matched before continued writes began:

| Type | Recovered values | Missing | Wrong | Read ops/s |
|---|---:|---:|---:|---:|
| String | 500,000 | 0 | 0 | 595,489 |
| Hash | 500,000 | 0 | 0 | 514,810 |
| Set | 500,000 | 0 | 0 | 766,991 |
| Sorted set | 500,000 | 0 | 0 | 1,009,091 |
| List | 500,000 | 0 | 0 | 888,564 |
| Array | 500,000 | 0 | 0 | 1,249,047 |

After the second half was written, cardinality was one million for every
collection and every final value matched:

| Type | Final values | Missing | Wrong | Read ops/s |
|---|---:|---:|---:|---:|
| String | 1,000,000 | 0 | 0 | 823,982 |
| Hash | 1,000,000 | 0 | 0 | 687,456 |
| Set | 1,000,000 | 0 | 0 | 1,164,571 |
| Sorted set | 1,000,000 | 0 | 0 | 1,215,437 |
| List | 1,000,000 | 0 | 0 | 894,141 |
| Array | 1,000,000 | 0 | 0 | 1,246,103 |
| **Total** | **6,000,000** | **0** | **0** | **956,848 aggregate** |

At completion, Goblin Core reported 188.3 MiB used memory and 283.7 MiB RSS.
Its logical replication offset was `9000009`, its last acknowledged Kafka
record was `9000008`, and Redpanda's high watermark was `9000009`.

## Finding

Every persistent type survived the same hard process death, was available with
the exact saved contents after startup replay, accepted another 500,000 logged
writes, and retained a fully correct million-value result. This is the behavior
the external-logging design promises: Goblin Core owns compact hot state and
native snapshots; Redpanda owns the ordered durable history between them.
