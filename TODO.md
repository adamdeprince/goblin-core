# TODO / Roadmap

Deliberately-deferred work, and a starting point for contributors. The design
stance behind the persistence items lives in the [README](README.md).

Kafka-backed journaling, exact snapshot cursors, and recovery replay shipped in
v0.9.0. Selectable broker-acknowledged replies shipped in v0.10.0. See
[Kafka write log and recovery](docs/kafka.md).

## Deferred

- **Old RDB fixtures.** The RDB reader implements the ziplist zset (type 12) and
  ASCII-double (type 3) encodings for Redis 2.6–6.x, but they are only
  ground-truthed against a real 7.2.4 dump (types 5/17). Build a redis 6.2 (and
  3.2) to generate ziplist/ASCII fixtures and round-trip them.
