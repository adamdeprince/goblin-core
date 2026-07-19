# AUTH

Authenticate the current RESP connection.

```text
AUTH password
AUTH username password
```

The one-argument form uses username `default`. Success returns `OK`; failure
returns a generic `WRONGPASS` error. When the server has no `--auth-file`, AUTH
returns an error because there are no configured credentials.

See [Authentication](../authentication.md) for credential-file management,
transport bypasses, and the trust model.
