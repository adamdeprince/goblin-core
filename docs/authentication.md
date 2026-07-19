# Authentication

Goblin Core can require username/password authentication for RESP clients. The
server reads a credential file at startup; `goblin-core-auth` is the only tool
that should write that file.

## Create and run

The editor defaults to `goblin-core.auth` in the current directory:

```sh
goblin-core-auth add default 'correct horse battery staple'
goblin-core --auth-file goblin-core.auth
```

Use an explicit production path with `--file`:

```sh
goblin-core-auth --file /etc/goblin/core.auth add api 'long-secret'
goblin-core --auth-file /etc/goblin/core.auth
```

`add` creates a user or rotates an existing user's password. `remove` verifies
the current password before deleting the user and refuses to delete the final
user:

```sh
goblin-core-auth --file /etc/goblin/core.auth remove api 'long-secret'
```

The positional password form requested by the CLI can appear in shell history
and briefly in process listings. Run the editor on a trusted administrative
host and apply the shell's history controls when that exposure matters.

## File contract

Each password is stored as a self-describing libsodium Argon2id password hash,
including its salt and work parameters. The editor uses libsodium's interactive
password-hashing limits and rewrites the file atomically. Plaintext passwords
are never written to disk.

The server refuses to start when the file is missing, malformed, empty, contains
duplicate usernames, is not owned by the server user, or grants any group/other
permissions. Editor-created files use mode `0600`. Credentials are an immutable
startup snapshot; restart Goblin Core after adding, rotating, or removing users.

## RESP commands

Authenticate explicitly with either form:

```text
AUTH password
AUTH username password
```

The one-argument form selects the `default` user. Authentication can be folded
into connection negotiation and naming:

```text
HELLO 3 AUTH username password SETNAME client-name
```

Before authentication, a RESP connection accepts only `AUTH`, `HELLO`, and
`QUIT`. Other commands return `NOAUTH Authentication required.` Failed logins
return the same generic `WRONGPASS` error whether the username or password was
wrong. Re-authentication changes the connection's authenticated username.

This release authenticates identities; it does not implement per-user ACLs.
Every authenticated user has the full command surface.

## Trust boundaries

| Connection | Behavior with `--auth-file` |
|---|---|
| RESP over TCP, UDS, or ExaSock | Authentication required. |
| RESP over a shared-memory ring | Required by default; `--no-auth-ring` marks every ring trusted. |
| RESP over RDMA | Required by default; `--no-auth-rdma` marks every RDMA endpoint trusted. |
| SBE over any transport | Never authenticated; SBE itself requires `--enable-sbe`. |

The two RESP bypasses make authentication optional on that transport; `AUTH`
and `HELLO ... AUTH` still work when a trusted client sends them.

SBE is a lockstep cluster-fabric protocol. Enabling it deliberately places every
SBE-capable listener inside the trusted boundary, including TCP and UDS listeners
that accept the `GOBLINS!` negotiation. Restrict their network reachability.

Authentication is not encryption. RESP carries the submitted password in clear
protocol bytes, so use UDS, a private network, or a TLS proxy for untrusted links.
The password file limits the damage of a file disclosure; it does not protect a
password crossing an observable network.
