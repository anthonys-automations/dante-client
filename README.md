# Dante client

An Alpine-based image containing a patched Dante `libdsocks.so` client
library at `/usr/lib/libdsocks.so`. It is a base image, not a SOCKS server
or a ready-to-run application. The published image is
`anthonysautomations/dante-client`, for `linux/amd64` and `linux/arm64`.

## Use

Extend the image with an Alpine-compatible application and a Dante client
configuration. The image defaults to UID/GID `65534:65534`; switch to root
when installing packages and return to a non-root user for runtime. Pin a
release tag, preferably with its digest, rather than `latest`.

For example, configure an IPv6 SOCKS5 gateway in a `socks.conf`:

```text
logoutput: stderr
route {
  from: 0.0.0.0/0 to: 0.0.0.0/0 via: 2001:db8::1 port = 1080
  proxyprotocol: socks_v5
  method: none
  command: connect udpassociate
}
```

Replace the example address and authentication settings for your server.
Make the configuration readable by the runtime user, then launch the
application with:

```sh
SOCKS_CONF=/path/to/socks.conf LD_PRELOAD=/usr/lib/libdsocks.so your-application
```

The library intercepts supported dynamically linked socket calls; it is not
a firewall. Applications using static linking, direct syscalls, unsupported
calls, or routes configured as direct can bypass it. Test your application
and enforce network policy separately if proxy use is mandatory.

## Patches and limitations

- The connect-child patch handles non-blocking connection negotiations
  concurrently. `SOCKS_CONNECTCHILD_MAXWORKERS` limits workers (default
  128); excess requests fail immediately. `SOCKS_CONNECTCHILD_TIMEOUT`
  sets the negotiation timeout in seconds (default 30). Values must be
  positive; if a worker cannot be forked, negotiation falls back to inline.
- IPv4 application sockets can reach IPv4 or IPv6 SOCKS gateways, including
  IPv6 literals and names, for TCP CONNECT and UDP ASSOCIATE. IPv6
  application sockets are **not** socksified and may bypass the proxy.
- Reaching an IPv6 gateway can replace the underlying socket while keeping
  its file descriptor. A specific IPv4 local bind is dropped; a wildcard
  bind with a fixed port retains its port. Changed `IP_TOS` and `IP_TTL`
  values are mapped to their IPv6 equivalents; other IP-level options are
  dropped. `SO_DOMAIN` exposes the new family, and IPv4 options set after
  connection do not affect IPv6 traffic. UDP replies whose source cannot
  be represented as IPv4 are dropped.
- The library requests a 2 MiB default thread stack for musl, including
  threads in the preloaded application.

See the [IPv6 patch](dante-ipv6-gateway.patch) and
[worker patch](dante-connectchild-workers.patch) for implementation details.

## Build and test

Build the final image locally, including the same patch compatibility checks
used for CI, Renovate updates and releases:

```sh
docker build -t dante-client:local .
```

The build requires IPv4 and IPv6 loopback, including the `ip6-localhost` name,
in the isolated BuildKit test stage. A blocked connect, unexpected success or
failure count, pending request, or failed TCP/UDP gateway check stops the
build. Test tools stay out of the final image.

See [image builds and versioning](docs/image-versioning.md) for release tags,
publication, and Renovate. Repository code is licensed under
[Apache-2.0](LICENSE).
