# TODO / Roadmap

Deliberately-deferred work, and a starting point for contributors. The design
stance behind the persistence items lives in the [README](README.md).

## Consume ZADD/ZREM from Kafka (native durable write log)

Goblin Core deliberately has no append-only write log; the README recommends
putting a durable log such as Kafka in front of it. The planned follow-on is to
let Goblin Core **consume `ZADD`/`ZREM` operations directly from a Kafka topic**,
so an operator gets durable, replayable writes without running a separate loader
process — the log lives in Kafka (where durability policy belongs) and Goblin
Core replays it into its in-memory index. This stays opt-in and off by default,
so it does not change the "explicit, no automated `fsync`" stance for users who
do not want it, while giving those who do a clean, log-backed path.

To make this easy to pick up:

- **Reuse the command path.** Apply each record through the existing
  `parse_command` / `execute_command` in `command.hpp`, so semantics match the
  RESP surface exactly — do not write a second implementation of `ZADD`/`ZREM`.
- **Message format.** Start with the same RESP array a client sends
  (`*4\r\n$4\r\nZADD\r\n...`), so any Kafka producer can write to the topic with
  no new schema. A compact binary encoding can come later.
- **Offsets and snapshots.** On startup, `--load` a snapshot, then resume the
  topic from where that snapshot ended. Store the committed Kafka offset(s) in
  the snapshot as a new typed section (the format already supports this — see
  `snapshot.hpp`), so a restart replays exactly the tail: no gaps, no
  double-apply.
- **Consumer.** A background thread/task subscribes, applies each op, and commits
  offsets after apply. Command execution is single-threaded today, so feed
  applied ops through the same loop rather than adding locking.
- **Config, opt-in and off by default.** `--kafka-brokers`, `--kafka-topic`,
  `--kafka-group`, and a start-offset policy.

## Also deferred

- **Old RDB fixtures.** The RDB reader implements the ziplist zset (type 12) and
  ASCII-double (type 3) encodings for Redis 2.6–6.x, but they are only
  ground-truthed against a real 7.2.4 dump (types 5/17). Build a redis 6.2 (and
  3.2) to generate ziplist/ASCII fixtures and round-trip them.
- **LoongArch CRC32C** is written to the builtin docs but untested (no hardware
  here). Verify on real LoongArch, or against a known vector under emulation.
