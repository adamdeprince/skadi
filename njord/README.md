# Njord

Njord is Skadi's companion IMAP proxy. This first version is a transparent,
bidirectional relay. Each client gets its own upstream connection. Authentication,
commands, literals, responses, and IDLE notifications belong to the client and
provider; Njord does not interpret or record their contents.

The low-bandwidth IMAP extension, compression, caching, reconnect logic, and
satellite-specific optimizations are future work. An interrupted connection in
this version disconnects the client; the client must reconnect.

## Install

Requires Python 3.8 or newer, with SSL support. There are no third-party Python
dependencies. From the Skadi repository:

```sh
make -C njord install PREFIX="$HOME/.local"
"$HOME/.local/bin/njord" --version
```

For a system installation, use `sudo make -C njord install PREFIX=/usr/local`.
The normal configured Skadi `make install` also installs Njord. `DESTDIR`,
`BINDIR`, and `MANDIR` are supported for packaging. Run
`make -C njord uninstall` with the same directory variables to remove it.

## Run

```sh
njord --upstream imap.example.com
```

Replace the hostname with the IMAP server supplied by your mail provider.
The defaults are a plain local listener on `127.0.0.1:1143` and verified TLS
to the provider on port 993. Configure the local mail client to use that
listener and its normal provider credentials. Njord does not store credentials.

For clients connecting over a network, configure a TLS listener with a certificate
whose name matches the hostname clients use:

```sh
njord --upstream imap.example.com --listen 0.0.0.0 --port 1993 \
  --certfile /etc/njord/fullchain.pem --keyfile /etc/njord/privkey.pem
```

Alternatively, keep the default loopback listener and reach it through an
encrypted tunnel. Do not send credentials over an unprotected network connection.
Protect the proxy with your normal host firewall/access controls. A public proxy
does not add authentication beyond the provider's own IMAP authentication.

For a local test server or an intentionally opaque TCP tunnel:

```sh
njord --upstream 127.0.0.1 --upstream-port 2143 --upstream-plaintext
```

This mode forwards raw bytes, including an end-to-end TLS conversation if the
client and provider negotiate one. Njord itself does not negotiate STARTTLS.
With the default upstream TLS mode, clients should use plaintext on the local
loopback connection, or implicit TLS when listener certificates are configured.

Other options include `--upstream-ca FILE` for a private CA,
`--connect-timeout SECONDS` for connection establishment,
`--shutdown-timeout SECONDS` for graceful drain and TLS shutdown (default 30), and `--verbose` for
connection diagnostics. Certificate and hostname verification remain enabled.
IPv6 addresses are accepted by `--listen` and `--upstream`; ports are separate
arguments. Run `njord --help` for the complete interface.

## Forwarding and operation

Njord streams the chunks delivered by asyncio transports. It does not allocate
a fixed command/response buffer or impose a maximum message, literal, session,
or connection count. Transport backpressure pauses reading from a fast sender
while a slow receiver catches up. Actual socket or memory failures disconnect
the affected session and are logged. Idle sessions have no application timeout.

Plain TCP half-closes are forwarded, allowing a final response after a client
finishes sending. TLS does not support independent write-half shutdown; an EOF
ends that session. SIGINT/SIGTERM stops the listener and disconnects active
sessions. Run Njord under a service manager for persistent use; no service is
automatically enabled by installation.

## Verify

```sh
make -C njord check
```

The tests use disposable loopback servers and locally generated certificates.
They require OpenSSL's command-line tool, but no live mail account or outbound
mail. They exercise large/binary traffic, simultaneous directions, repeated and
concurrent clients, half-close behavior, backpressure, TLS verification, and
connection failures.
