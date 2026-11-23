#!/bin/sh
# Tests for libdsocks with this repository's patches. Needs cc and python3;
# all traffic stays on the loopback (127.0.0.1 and ::1).
#   usage: run.sh /path/to/libdsocks.so
# 1. Non-blocking connects (dante-connectchild-workers.patch). Expected with
#    the patch: no connect() blocks for more than a few ms and nothing is
#    left pending. Unpatched, the first case hangs in connect().
# 2. SOCKS server reached over IPv6 (dante-ipv6-gateway.patch): v6check
#    against an IPv4, an IPv6 and a name gateway, then non-blocking connects
#    through [::1]. The script fails if a v6check check fails. The IPv6 cases
#    are skipped (with a message) if ::1 is not available.
set -eu
LIB=$1
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
cc -O2 -o "$W/nbconnect" "$D/nbconnect.c"
cc -O2 -o "$W/v6check" "$D/v6check.c"
SRV=
trap '[ -n "$SRV" ] && kill $SRV 2>/dev/null; true' EXIT

conf() { # GATEWAY
  GW=$1
  sed "s/via: 127\.0\.0\.1 /via: $GW /" "$D/socks.conf" > "$W/socks.conf"
}

srv() { # DELAY FAIL_EVERY [HOST [UDP_HOST]]
  [ -n "$SRV" ] && kill $SRV 2>/dev/null && sleep 1
  python3 -u "$D/slowsocks.py" 1081 "$@" > "$W/socks.log" 2>&1 &
  SRV=$!
  sleep 1.5
}

t() { # NAME DELAY FAIL_EVERY N WAIT [ENV...]; server on $HOST
  name=$1 delay=$2 fail=$3 n=$4 wait=$5; shift 5
  echo "## $name (via $GW)"
  srv "$delay" "$fail" "$HOST"
  env "$@" SOCKS_CONF="$W/socks.conf" LD_PRELOAD="$LIB" \
    timeout $((wait + 30)) "$W/nbconnect" "$n" "$wait" 2> "$W/client.err" || echo "client rc=$? (124: timed out, connect() blocked)"
}

v6() { # NAME GATEWAY HOST UDP_HOST TCP_FAMILY UDP_FAMILY
  echo "## v6check: $1 (via $2)"
  conf "$2"
  srv 0 0 "$3" "$4"
  SOCKS_CONF="$W/socks.conf" LD_PRELOAD="$LIB" \
    timeout 60 "$W/v6check" "$5" "$6" 2> "$W/client.err" \
    || { echo "v6check rc=$?:"; tail -20 "$W/client.err"; exit 1; }
}

HOST=127.0.0.1; conf 127.0.0.1
t "60 connects, proxy delay 5s: all succeed after ~5s" 5 0 60 15
t "worker cap 20: 20 succeed, 40 fail at once" 5 0 60 10 SOCKS_CONNECTCHILD_MAXWORKERS=20
t "worker timeout 3s, proxy delay 60s: all fail after ~3s" 60 0 20 8 SOCKS_CONNECTCHILD_TIMEOUT=3
t "proxy refuses every 3rd: 20 succeed, 10 fail" 2 3 30 8
t "500 connects, default cap 128: 128 succeed, rest fail at once" 2 0 500 15

v6 "IPv4 server, as before" 127.0.0.1 127.0.0.1 127.0.0.1 4 4
if python3 -c 'import socket; socket.socket(socket.AF_INET6).bind(("::1", 0))' 2>/dev/null; then
  v6 "IPv6 server" ::1 ::1 ::1 6 6
  v6 "IPv6 server with an IPv4 UDP relay" ::1 ::1 127.0.0.1 6 4
  # musl and glibc both read /etc/hosts; Docker maps ip6-localhost to ::1.
  if [ "$(python3 -c 'import socket; print(sorted({a[0].name for a in socket.getaddrinfo("ip6-localhost", 1081, proto=6)}))' 2>/dev/null)" = "['AF_INET6']" ]; then
    NAMEGW=ip6-localhost
    v6 "name that resolves to ::1 only" $NAMEGW ::1 ::1 6 6
  else
    NAMEGW=
    echo "## SKIP name gateway cases: ip6-localhost does not resolve to ::1 only"
  fi
  HOST=::1; conf ::1
  t "60 connects, proxy delay 5s: all succeed after ~5s" 5 0 60 15
  t "proxy refuses every 3rd: 20 succeed, 10 fail" 2 3 30 8
  t "500 connects, default cap 128: 128 succeed, rest fail at once" 2 0 500 15
  if [ -n "$NAMEGW" ]; then
    conf $NAMEGW
    t "60 connects, proxy delay 5s: all succeed after ~5s" 5 0 60 15
  fi
else
  echo "## SKIP IPv6 cases: ::1 is not available"
fi
rm -rf "$W"
