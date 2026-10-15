# Njord 0.1.0

Njord is Skadi's optional IMAP proxy, written in C++17. Run it on your home
Linux computer: Skadi uses checksummed **zstd level 22** frames to reach Njord,
and Njord talks conventional IMAP to your provider. You can also use Skadi
with a normal IMAP server directly; no Njord installation is required.

## Install

Build dependencies: a C++17 compiler, Make, pkg-config, libevent 2.1,
OpenSSL 1.1.1 or newer, zstd 1.4 or newer, libcurl and libwebp.
Python is only a test harness dependency, never a runtime dependency.

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libevent-dev libssl-dev libzstd-dev libcurl4-openssl-dev libwebp-dev

# macOS (use the same OpenSSL major version as libevent)
brew install pkgconf libevent openssl@4 zstd webp

make -C njord install PREFIX="$HOME/.local"
"$HOME/.local/bin/njord" --version
```

A configured Skadi `make install` also installs Njord. `DESTDIR`, `BINDIR` and
`MANDIR` are supported; `make -C njord uninstall` removes the installed program.
Binary packages install Skadi and Njord separately and never start a service.

## Connect

```sh
njord --upstream imap.example.com
```

The defaults are a loopback listener at `127.0.0.1:1143` and verified provider
TLS on port 993. Use your provider's normal credentials in Skadi. For example:

```
# Normal IMAP, without Njord:
{imap.example.com:993/ssl/user=you@example.com}INBOX

# Njord on localhost, or forwarded through an SSH tunnel:
{localhost:1143/njord/user=you@example.com}INBOX

# Remote Njord with an implicit TLS listener:
{home.example.com:1993/njord/ssl/user=you@example.com}INBOX
```

Configure a remote TLS listener with:

```sh
njord --upstream imap.example.com --listen 0.0.0.0 --port 1993 \
  --certfile /etc/njord/fullchain.pem --keyfile /etc/njord/privkey.pem
```

Use listener TLS or an encrypted tunnel across a network. Keep Njord accessible
only to your own clients using a firewall, VPN or SSH forwarding. IMAP login is
handled by the provider; the separate image endpoint does not require an IMAP
login. Njord never logs credentials, message contents or image URLs.

`--upstream-plaintext` selects ordinary TCP to a local test/provider server.
`--raw` disables the Njord protocol for ordinary IMAP clients and opaque tunnels.
STARTTLS is not negotiated inside Njord framing: use `/ssl` for TLS around it.
`--upstream-ca FILE` chooses a provider CA bundle. DNS/connect/handshake and
shutdown deadlines default to 30 seconds and are configurable. Established
IMAP sessions have no idle timeout. See `njord --help` for all options.

## Batches and attachment sizes

In Skadi's message index, **K (sYnc)** requests every header not yet cached,
including subjects. An inventory request and the missing-header request are
pipelined in one flight. The inventory includes UIDs, flags, message sizes and
BODYSTRUCTURE; cached headers are not downloaded again. This also works over
direct IMAP. Servers can still enforce their own command/resource limits;
Skadi reports failures rather than silently splitting into sequential fetches.

Select messages with `:` or `;`, then press **K** to fetch their complete bodies
in one `UID FETCH ... BODY.PEEK[]` command. Downloaded messages do not become
read merely from fetching. Each complete message is saved atomically before
the next message is processed. Completed downloads survive an interrupted
batch and are skipped on retry. RAM need only hold the current message, not
the entire batch. Bodies include attachments; inspect sizes first if needed.

Press **[ (PartSizes)** in the index to see MIME parts, names and their encoded
byte counts without downloading their contents. Sync saves this list for
later offline inspection. IMAP reports transfer-encoded sizes: base64 and
quoted-printable overhead are included. These are not exact decoded file
sizes or predictions of the eventual compressed transfer size.

Cache identity includes the mailbox, UIDVALIDITY and UID. Sequence positions
can change after deletions; UIDs remain meaningful only within one mailbox's
UIDVALIDITY. Message-ID is an email header, not an IMAP fetch identifier.

Explicit body downloads and MIME inventories are individual `.eml` and `.mime`
files in `~/.alpine-offcache`, outside the bounded opportunistic body cache.
They persist across restarts. These private files contain mail; remove that
cache directory if you intentionally want to discard all offline downloads.

## Linked images

HTTPS images are fetched only when you click their handle or Tab to it and
press Enter. With `/njord`, the fetch happens at home through Njord. Without
Njord, Skadi's direct image fetch uses the `curl` command. Formatting, repainting
and cache lookups never initiate an image download.

The default is a smaller preview: Njord decodes JPEG, PNG, GIF or WebP, fits
within 1024 pixels on the longest side, and encodes WebP at quality 65. It sends
the original instead when that is smaller. Previews are still images; a GIF
preview uses its first frame. **O (Original)** in the image viewer requests
the original bytes, without resizing or transcoding. Downloaded previews and
originals are cached separately for repeated viewing during the session.

`--image-width` and `--image-quality` adjust preview size and fidelity. The
width must fit WebP's 14-bit dimension field (1..16383); quality is 0..100.
`--image-timeout` sets each HTTPS transfer's deadline (default 60 seconds).
`--image-ca` selects its CA bundle. TLS certificates are always verified,
redirects must remain HTTPS, redirect cycles fail, and requests carry no mail
credentials, cookies or ambient HTTP proxy configuration. Private destination
addresses are rejected unless you explicitly use `--image-allow-private`.
Each image runs in a separate native worker so fetching and encoding do not
block the relay's event loop; disconnecting the client cleans up its worker.

### Lepton

Lepton preserves the original JPEG exactly, which makes it a candidate for
future lossless original transfers rather than reduced previews. The
[original Dropbox implementation](https://github.com/dropbox/lepton) is archived
and deprecated. [Microsoft's Rust port](https://github.com/microsoft/lepton_jpeg_rust)
is worth evaluating for an optional negotiated codec. It is not included in
this release. WebP handles both JPEG and PNG previews without a Rust runtime
or a client-side Lepton decoder.

## Protocol and operation

An IMAP connection exchanges the eight-byte `NJORD1\r\n` preamble. An image
connection exchanges `NJIMG1\r\n`. Subsequent traffic in either direction is a
four-byte big-endian compressed length followed by one checksummed zstd frame
with its uncompressed length declared. Encoding uses level 22. Senders split
streams at zstd's recommended streaming input size; these are independent
frames, so compression history does not span frame boundaries. This bounds
codec working memory without limiting message or batch size.

An image request is `PREVIEW https://...\r\n` or `ORIGINAL https://...\r\n`.
The response is `OK <source-bytes> <payload-bytes> <mime-type>\r\n` followed by
exactly that many payload bytes, or `ERR <reason>\r\n`. One image per connection.
Both the request and response use the same zstd framing, including originals.

The nonblocking relay uses asynchronous DNS, growing buffers and backpressure
based on OS socket buffers. It imposes no message, batch or connection-count
cap. Actual allocation, API integer and OS resource limits produce errors.
TCP half-closes drain normally; TLS EOF drains traffic and performs TLS close
notification within the shutdown deadline. SIGINT/SIGTERM disconnect sessions.
An interrupted IMAP connection must be reopened by the client; automatic
session replay and measured satellite performance remain future work.

## Verify

```sh
make -C njord check
```

The Python tests create disposable loopback IMAP/HTTPS servers and certificates,
without a live account. They cover TCP/TLS, backpressure, large transfers,
concurrency, repeated sessions, failures, framing, previews and originals.
`NJORDBIN` tests an installed binary. `Makefile.integration` builds the actual
Skadi transport/cache/image integration executable after a normal Skadi build;
set `SKADITEST` to its path to include those end-to-end checks.
