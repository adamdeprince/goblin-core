# TCP listeners and TLS

Goblin Core can serve one store through multiple ordinary TCP listeners at the
same time. `--listen` is repeatable, accepts numeric IPv4 or bracketed numeric
IPv6 endpoints, and keeps the older `--tcp-listen` spelling as an alias.

Every configured TCP port also receives a plaintext `127.0.0.1` listener. This
local endpoint is always available, including when the server has only UDS,
ring, RDMA, or ExaSock endpoints. With no listener options, Goblin Core listens
on plaintext `127.0.0.1:6379`.

## Endpoint forms

```text
--listen :6379
--listen 192.168.1.13:6379
--listen [2001:db8::13]:6379
```

`:PORT` is shorthand for `127.0.0.1:PORT`. IPv6 addresses must use brackets.
Listeners take numeric addresses so startup never depends on name resolution.
Repeat the flag to bind more addresses or ports.

Concrete non-loopback ordinary TCP addresses require TLS. The IPv4 wildcard
`0.0.0.0` is rejected because it would overlap the mandatory plaintext
`127.0.0.1` endpoint; list the intended IPv4 interface addresses instead. The
IPv6 wildcard `[::]` is supported as an IPv6-only TLS listener and can share a
port with the IPv4 localhost listener.

## Certificate identity

Builds enable OpenSSL support by default. Supply one PEM certificate chain and
its matching PEM private key for every non-loopback ordinary TCP listener:

```sh
goblin-core \
  --listen 192.168.1.13:6379 \
  --listen '[2001:db8::13]:6379' \
  --tls-cert-file /etc/goblin/fullchain.pem \
  --tls-key-file /etc/goblin/private-key.pem
```

This opens TLS on both named interface addresses and plaintext
`127.0.0.1:6379`. The server requires TLS 1.2 or newer, disables TLS-level
compression, validates that the private key matches the certificate at startup,
uses one nonblocking OpenSSL context for all TLS listeners, and leaves OpenSSL's
stateful server-session cache off rather than retaining session entries.

`--tls-cert` and `--tls-key` are shorthand aliases. To build without the native
TLS transport or its OpenSSL dependency, configure with
`-DGOBLIN_CORE_ENABLE_TLS=OFF`; that build rejects non-loopback ordinary TCP
listeners.

TLS sits below protocol detection, so RESP2, RESP3, and opt-in SBE can use a TLS
TCP listener. TLS provides encryption and server identity, but Goblin Core does
not request client certificates. RESP authentication remains independently
controlled by `--auth-file`. SBE has no authentication exchange, so
`--enable-sbe` still belongs only inside a network boundary that authenticates
clients.

## Other transports

`--uds-listen`, `--ring`, `--rdma`, and `--exasock` keep their existing trusted
transport behavior and do not use the ordinary TCP TLS context. See
[Authentication and trust boundaries](authentication.md) before exposing any
listener.
