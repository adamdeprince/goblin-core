# COMMAND

Discover the command surface and metadata used by Redis client libraries.

```text
COMMAND
COMMAND INFO [command-name ...]
```

`COMMAND` returns descriptors for every keyword in Goblin Core's perfect-hash
dispatch table. `COMMAND INFO` returns one descriptor per requested name and a
null entry for an unknown command. Descriptors use the Redis 7+ ten-field shape:
name, arity, flags, legacy key positions, ACL categories, tips, key specs, and
subcommands.
