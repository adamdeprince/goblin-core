# SELECT

Goblin Core has one keyspace and accepts database zero for client compatibility.

```text
SELECT 0
```

Database zero returns `OK`. Every other index returns `ERR DB index is out of
range`.
