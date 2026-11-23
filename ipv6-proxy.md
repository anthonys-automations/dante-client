# libdsocks: SOCKS servers reachable over IPv6 (design notes)

Status: implemented in [dante-ipv6-gateway.patch](dante-ipv6-gateway.patch),
on top of [dante-connectchild-workers.patch](dante-connectchild-workers.patch).
Tested by [test/run.sh](test/run.sh) with [test/v6check.c](test/v6check.c).
The design notes below are kept as written; see
[Implementation notes](#implementation-notes) for where the patch differs.
Line numbers refer to dante 1.4.4.

## Implementation notes

- **The client rejected IPv6 gateways.** The grammar accepts `via: ::1`, but
  `socks_addroute()` (`lib/config.c:288`) allowed `SOCKS_ADDR_IPV6` gateways
  only in the server (`#if !SOCKS_CLIENT`) and failed with "gateway must be
  an IP-address or qualified domainname". The patch allows them.
- `socks_resocket()` is a new function in `lib/sockopt.c`. It does not change
  `socketoptdup()`. It maps `IP_TOS`/`IP_TTL` only when the value differs
  from a new socket's, so "use the default" stays the default. All other
  `IPPROTO_IP`/`IPPROTO_IPV6` options are dropped. A wildcard bind
  `0.0.0.0:P` becomes `[::]:P` with `IPV6_V6ONLY`, because the old socket
  still holds the port while the new one binds.
- `socks_connecthost()` changes only in the client build (`SOCKS_CLIENT`).
  The client calls it only for gateways, so direct routes are not affected.
  sockd keeps its behaviour.
- **Non-blocking connect:** `socks_nbconnectroute()` bound the `AF_INET` dummy
  fd to the control socket's local IP. When the control socket has become
  IPv6, the dummy is bound to `127.0.0.1:0` instead.
- **UDP:** the data socket is replaced *before* the UDP ASSOCIATE request,
  to the family of the control connection. The address sent in the request
  therefore matches the real source. If the relay address in the reply is of
  the other family, the socket is replaced again; the source then no longer
  matches what was announced (logged at debug level). The socket stays bound
  (same port if possible), because `socks_addrisours()` needs a bound
  address to recognise it on later calls. `udpsetup()` now checks for
  "already set up" before rejecting non-`AF_INET` sockets.
- `Rrecvfrom()` converts IPv4-mapped sources to `AF_INET`. It drops
  datagrams whose source an `AF_INET` socket can't represent, and loops again
  if the socket is blocking.
- `socks_addrisours()` (`lib/address.c:552`) tested
  `sin_addr.s_addr == 0` on IPv6 local addresses, reading
  `sin6_flowinfo`. It now uses `IPADDRISBOUND()`.
- **`setsockopt()` after connect:** Linux accepts `IPPROTO_IP` options such
  as `IP_TOS` on an `AF_INET6` TCP or UDP socket without error. They only
  affect IPv4-mapped traffic, so they have no effect on the IPv6 path to the
  server. libdsocks does not wrap `setsockopt()` and does not translate
  them. `getsockopt(SO_DOMAIN)` on a replaced fd returns `AF_INET6`.

## Goal and scope

**In scope.** The program's sockets stay IPv4 (`AF_INET`) and are socksified
as today. The SOCKS server, given in `via:` as an IPv6 address or as a name
that resolves to IPv6, is reached over IPv6. This covers TCP `connect` and UDP
`udpassociate`. Today this needs a local IPv4 relay in front of the server
(something that listens on `127.0.0.1` and forwards to the IPv6 server).

**Out of scope.** Socksifying the program's own IPv6 sockets (`AF_INET6`
destinations). That is a separate and larger feature, covered in
[Related](#related-socksifying-ipv6-program-sockets).

**Verdict.** Moderate. The pieces already exist: IPv6 `sockshost_t`/sockaddr
conversion, IPv6 addresses in the config grammar, and an in-place
"replace the socket behind this fd" pattern. The real problem is that
libdsocks connects to the SOCKS server **on the program's own socket**, and
an `AF_INET` socket cannot connect to an IPv6 address. The fix is to swap the
socket behind the fd for an `AF_INET6` one at the point where the server's
address is known, then patch up what the program sees. The estimate is
roughly 250-400 changed lines plus tests, one session.

## How it works today

### TCP

- `Rconnect()` (`lib/Rconnect.c:82`) passes anything that is not `AF_INET`
  straight to the system `connect()`. IPv4 sockets continue.
- Blocking connect: `socks_connectroute()` (`lib/config.c:1244`) calls
  `socks_connecthost(s, ..., &route->gw.addr, ...)` (`lib/socket.c:51`) **on
  the program's socket `s`**. Control and data are the same fd.
- Non-blocking connect: `socks_nbconnectroute()` (`lib/connectchild.c`) gives
  the program a dummy fd, `makedummyfd(AF_INET, SOCK_STREAM)` (line 424). It
  keeps the program's original socket as `control` and connects it to the
  server with `socks_connectroute(control, ...)` (line 500). That happens
  before the request is registered (`socks_addaddr`) and sent to the
  connect-child. When the child's reply arrives, `sigio()` does
  `dup2(socksfd.control, s)`, so the program's fd becomes the socket
  connected to the proxy.
- `socks_connecthost()`:
  - **Literal gateway** (`SOCKS_ADDR_IPV4`/`IPV6`): `connect(s, raddr)`
    (line 182). An IPv6 gateway on an `AF_INET` socket fails with
    `EAFNOSUPPORT`.
  - **Name gateway** (`SOCKS_ADDR_DOMAIN`): resolves it, then **skips** every
    result whose family differs from the socket's (line 405, "can not attempt
    connect to address ... from our AF_INET-socket"). An IPv6-only name
    therefore fails.
  - **Retries:** for retries after a failed address it already replaces the
    socket in place: `socketoptdup(s, -1)` + `dup2(new_s, s)` +
    `socks_bind(s, laddr, 1)`. That is the mechanism to reuse.
- `socketoptdup()` (`lib/sockopt.c:62`) creates the new socket with the
  **same family** (from `getsockname()`), then copies an option list from
  `sockopt.m4:281-287`. The list includes IPv4-level options
  (`IP_TOS IP_TTL IP_OPTIONS IP_MULTICAST_*` ...) besides `SOL_SOCKET` and
  `IPPROTO_TCP` ones. Copy failures are only logged at debug level.

### UDP (`udpassociate`)

- `udpsetup()` (`lib/udp.c:659`) rejects non-`AF_INET` program sockets
  (line 702), which is fine for this scope.
- The TCP control connection to the server is a **new** socket:
  `socket(AF_INET, SOCK_STREAM, 0)` (line 847).
- The data path uses the program's own UDP socket, which is connected (or
  `sendto()`s) to the relay address from the UDP ASSOCIATE reply
  (`socksfd.reply`, line 994). An IPv6 server will normally return an IPv6
  relay address, which an `AF_INET` UDP socket cannot reach.
- `Rrecvfrom()` recognises datagrams from the relay by comparing the source
  with `socksfd.reply`. It strips the SOCKS UDP header and reports the
  address from the header as the source:
  `fakesockshost2sockaddr(&header.host, &newfrom)` (line 554). Through an
  IPv6-capable server that address can be IPv6.

### What the program sees

- `Rgetsockname()` returns `socksfd.remote`, the BND.ADDR from the server's
  reply (`lib/Rgetsockname.c:74,122`). That can already be IPv6 today
  whenever the server (or a relay in front of it) has IPv6 egress. With an
  `AF_INET` program socket and a 16-byte buffer you get
  `sockaddrcpy(): truncating address ... (af: 10)` warnings and a mangled
  address.

### Already IPv6-capable (no change expected)

- The config grammar: `gwaddress: ipaddress gwport`, and `ipaddress`
  includes `ipv6` (`lib/config_parse.y:3227-3235`).
- `sockshost_t` and the conversions (`int_sockshost2sockaddr2()`,
  `sockaddr2sockshost()` in `lib/util.c`). The SOCKS5 request/reply code uses
  these, so ATYP 4 is handled.
- The fake resolver (`resolveprotocol: fake`) is IPv4-only
  (`lib/Rgethostbyname.c:265`). That doesn't matter here: names still reach
  the server unresolved.

## Proposed change

### 1. Family-converting socket replacement (core)

Add a helper:

```c
/*
 * Replace the socket behind fd s with a new socket of family af (same type,
 * flags and the options that make sense in af), keeping the fd number.
 * Returns 0 or -1 (errno set, s unchanged).
 */
int socks_resocket(int s, sa_family_t af);
```

- Implement it by giving `socketoptdup()` a family parameter (or adding a
  sibling function). Copy `SOL_SOCKET` and `IPPROTO_TCP` options as today.
  For `IPPROTO_IP` options when going to `AF_INET6`:
  - map `IP_TOS` → `IPPROTO_IPV6/IPV6_TCLASS`;
  - map `IP_TTL` → `IPV6_UNICAST_HOPS`;
  - skip the rest (multicast, `IP_OPTIONS`, `IP_HDRINCL`, ...).
- File status flags (`O_NONBLOCK`) already carry over (`F_GETFL`/`F_SETFL`).
  Also carry `FD_CLOEXEC` (`F_GETFD`/`F_SETFD`), which `socketoptdup()`
  doesn't do today.
- Local address:
  - The old socket may be bound, for example by the program, or through
    `bind` routes to `0.0.0.0`/`127.0.0.1`. An IPv4 bind address can't be
    applied to an IPv6 socket.
  - Default: no bind (the kernel picks the source).
  - `0.0.0.0:P` maps to `[::]:P` only when `P` is nonzero.
  - Log loopback/specific IPv4 binds at debug level and drop them. Binding to
    `::1` would break connects to non-loopback servers.

### 2. Use it where the server address is known

- `socks_connecthost()` (`lib/socket.c`):
  - **Literal path:** when `raddr->ss_family != local family`, call
    `socks_resocket(s, raddr->ss_family)` before `connect()`.
  - **Name path:** instead of skipping results of the other family, try them
    after `socks_resocket()`. Keep the retry logic: the existing "failed,
    need a new socket" branch should recreate with the family of the next
    address.
  - Ordering policy for mixed results: try the socket's own family first,
    then the other.
- Gate it so it only applies to SOCKS gateways, never to `direct` routes:
  direct connects keep the program's semantics.
- This one hook covers:
  - **Blocking connect:** `s` is the program's fd and is swapped in place.
  - **Non-blocking connect:** `control` is swapped before registration and
    before it is sent to the connect-child (line 500 precedes `socks_addaddr`
    at line 627). `sigio()` then `dup2()`s the IPv6 socket onto the program's
    fd, and `socks_addrcontrol()`/`fdisdup()` matching uses the registered
    (new) control socket.
  - **UDP control connection:** create it with the gateway's family instead
    of hard-coded `AF_INET` (`lib/udp.c:847`). For a name gateway, create it
    after resolving, or let `socks_connecthost()` swap it.

### 3. UDP data socket

- After the UDP ASSOCIATE reply, if `socksfd.reply` is IPv6 and the
  program's UDP socket is `AF_INET`, call `socks_resocket(s, AF_INET6)`
  before `connect(s, socksfd.reply)` (`lib/udp.c:994`). Do the same in the
  `sendto()` path.
- Same bind policy as above. The relay expects datagrams from the address
  the client announced in the UDP ASSOCIATE request. Check which address and
  port libdsocks sends there (often `0.0.0.0:0`, meaning any) and make it
  consistent with the new socket.
- `Rrecvfrom()`: when the header address (`header.host`) is IPv6 and the
  program socket was `AF_INET`, the program can't use or reply to it. Drop
  the datagram (log at debug), or make that configurable. Do not hand an
  `AF_INET6` sockaddr to an `AF_INET` caller.
- Remember the program-visible family per fd. Add an `int origfamily` to
  `socksfd_t`, set when the request is registered. The `R*` wrappers present
  addresses in that family.

### 4. Addresses shown to the program

- `Rgetsockname()`: if `origfamily == AF_INET` and BND.ADDR is IPv6, return
  `0.0.0.0` with the BND port (or `0.0.0.0:0`). Never write more than
  `*namelen`, and set `*namelen` to `sizeof(struct sockaddr_in)`. This also
  fixes the truncation warning that exists today.
- `Rgetpeername()`: returns the destination the program asked for (IPv4 or
  fake IPv4); no change expected, but test it.
- `getsockopt(SO_DOMAIN)` and `getsockopt(IPPROTO_IP, ...)` on the swapped fd
  now reflect IPv6. libdsocks doesn't wrap them. Programs rarely look, but
  note it in the docs.
- `setsockopt(IPPROTO_IP, IP_TOS, ...)` by the program **after** connect now
  hits an `AF_INET6` socket. Whether Linux accepts IPv4-level options on an
  IPv6 TCP/UDP socket needs a test. If it doesn't, wrap `setsockopt` for
  swapped fds and translate `IP_TOS`/`IP_TTL` as in step 1 (libdsocks
  doesn't intercept `setsockopt` today, so that is a new symbol).

## Tests (extend `test/`)

- `slowsocks.py`: add a listen address argument (`::1`), and a minimal UDP
  ASSOCIATE (relay on `::1`, echo with a header that carries an IPv4 and an
  IPv6 source).
- `socks.conf` variants:
  - `via: ::1 port = 1081`
  - a name gateway that resolves to `::1` only (an `/etc/hosts` entry in the
    test container)
- Cases:
  - **TCP:** blocking and non-blocking connects through `::1` succeed (all
    `run.sh` cases, against both config variants).
  - **Program view:** `getsockname()` on a connected fd returns `AF_INET` with
    `namelen` 16; `getpeername()` returns the requested IPv4 destination.
  - **Options:** `IP_TOS` set before connect survives as `IPV6_TCLASS`
    (`getsockopt` on the new socket).
  - **Bind:** a program bound to `127.0.0.1` before `connect()` still
    connects to `[::1]`.
  - **UDP:** datagrams round-trip through an IPv6 relay; a datagram whose
    header source is IPv6 is dropped for an `AF_INET` socket.
  - **Regression:** an IPv4 gateway behaves exactly as before (no
    `socks_resocket()` call, same fd identity).
- Check first, before writing code: that the 1.4.4 lexer accepts an IPv6
  literal after `via:` (the grammar does), and what `getaddrinfo()` hints
  `socks_connecthost()` uses for name gateways on an `AF_INET` socket
  (`hints.ai_family = 0`, which returns both families: good).

## Risks

- **Changing the family behind a program's fd.** Programs that cache the
  family at `socket()` time and later build `sockaddr_in` for `bind`,
  `setsockopt` or `getsockname` keep working through the wrappers. Raw
  syscalls, or libc paths libdsocks doesn't wrap, will see IPv6.
- **Option mapping** is best-effort; unknown `IPPROTO_IP` options are dropped
  silently (at debug level).
- **The dropped local bind** changes the source address the server sees. For
  programs that bind for routing reasons (choosing an interface), add an
  explicit config knob later if it is needed.

## Related: socksifying IPv6 program sockets

This is not part of this change, but once the server is reachable over IPv6
it is the natural next step:
- `Rconnect.c:82`, `Rbind.c:88`, `udp.c:86`, `udp.c:702` pass `AF_INET6`
  program sockets to the kernel today, so they **bypass the proxy**. Deny or
  route them explicitly in the deployment until that changes.
- Supporting them means allowing `AF_INET6` in those checks, IPv6 routes
  (`from: ::/0 to: ::/0`, which the grammar can express), the dummy fd in
  `socks_nbconnectroute()` with the program's family, and the fake resolver
  returning IPv6 for `AF_INET6`/`AF_UNSPEC` lookups (`Rgethostbyname.c`).
