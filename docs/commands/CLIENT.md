# CLIENT

Goblin Core implements the connection metadata used by current Redis clients:

```text
CLIENT SETNAME name
CLIENT GETNAME
CLIENT ID
CLIENT SETINFO LIB-NAME library
CLIENT SETINFO LIB-VER version
```

`SETNAME` stores a printable, whitespace-free name on this connection; an empty
name clears it. `GETNAME` returns that name or null. `ID` returns the connection's
server-assigned integer ID. `SETINFO` records client-library metadata and returns
`OK`; it does not alter command behavior.
