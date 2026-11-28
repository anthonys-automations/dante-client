#!/bin/sh
# usage: run.sh /path/to/libdsocks.so (requires IPv4/IPv6 loopback)
set -eu
LIB=$1
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
SRV=
stop_server() {
  if [ -n "$SRV" ]; then
    kill "$SRV" 2>/dev/null || :
    wait "$SRV" 2>/dev/null || :
    SRV=
  fi
}
trap 'stop_server; rm -rf "$W"' EXIT
cc -O2 -o "$W/nbconnect" "$D/nbconnect.c"
cc -O2 -o "$W/v6check" "$D/v6check.c"
cc -O2 -pthread -o "$W/mtstress" "$D/mtstress.c"
cc -O2 -o "$W/readvcheck" "$D/readvcheck.c"
cc -O2 -o "$W/gaicheck" "$D/gaicheck.c"
cc -O2 -o "$W/epollcheck" "$D/epollcheck.c"
cc -O2 -o "$W/bindcheck" "$D/bindcheck.c"
cc -O2 -pthread -o "$W/getsockcheck" "$D/getsockcheck.c"
cc -O2 -o "$W/udpdropcheck" "$D/udpdropcheck.c"
cc -O2 -o "$W/fdreusecheck" "$D/fdreusecheck.c"
cc -O2 -o "$W/stalecheck" "$D/stalecheck.c"
cc -O2 -pthread -o "$W/dnsracecheck" "$D/dnsracecheck.c"

srv() { # GATEWAY DELAY FAIL_EVERY HOST [UDP_HOST]
  stop_server
  gateway=$1; shift
  sed "s/via: 127\.0\.0\.1 /via: $gateway /" "$D/socks.conf" > "$W/socks.conf"
  : > "$W/socks.log"
  python3 -u "$D/slowsocks.py" 1081 "$@" >> "$W/socks.log" 2>&1 &
  SRV=$!
  for _ in $(seq 50); do
    grep -qx 'socks: ready' "$W/socks.log" && return 0
    kill -0 "$SRV" 2>/dev/null || break
    sleep 0.1
  done
  echo "proxy failed to start for $gateway" >&2
  cat "$W/socks.log" >&2
  exit 1
}

client() { # TIME_LIMIT [ENV...] PROGRAM ARGS...
  limit=$1; shift
  timeout -k 5 "$limit" env SOCKS_CONF="$W/socks.conf" LD_PRELOAD="$LIB" \
    "$@" 2> "$W/client.err" || {
      rc=$?
      echo "client exited $rc (124 = timed out)" >&2
      tail -n 20 "$W/socks.log" "$W/client.err" >&2
      exit "$rc"
    }
  # libdsocks' helpers (connect-child, workers) must go when the program
  # does; one left behind is stuck (e.g., on a lock inherited at fork()).
  for _ in $(seq 20); do
    pgrep -f "^$W/" >/dev/null || return 0
    sleep 0.5
  done
  echo "FAIL: libdsocks helper processes left behind:" >&2
  ps -o pid,args | grep "$W/" | grep -v grep >&2
  pkill -9 -f "^$W/"
  exit 1
}

t() { # DELAY FAIL_EVERY N WAIT EXPECTED_SUCCESSES MIN_SECONDS [ENV...]
  delay=$1 fail=$2 n=$3 wait=$4 ok=$5 min=$6; shift 6
  echo "## $n non-blocking connects via $GW: delay=$delay refuse_every=$fail expected_ok=$ok $*"
  srv "$GW" "$delay" "$fail" "$HOST"
  client "$((wait + 5))" "$@" "$W/nbconnect" "$n" "$wait" "$ok" "$min"
}

bp() { # IPv4 wildcard bind with a port that is taken in IPv6
  echo "## connect via ::1 from 0.0.0.0:P with [::]:P taken"
  srv ::1 0 0 ::1
  # binds go to the kernel, as in a program's config.
  printf 'route {\n  from: 0.0.0.0/0 to: 0.0.0.0/0 via: direct\n  command: bind\n}\n' >> "$W/socks.conf"
  client 20 "$W/bindcheck"
  # it must not need Rconnect()'s EADDRINUSE workaround to get there.
  if grep -q 'Address in use' "$W/client.err"; then
    echo "FAIL: replacing the socket hit EADDRINUSE:" >&2
    grep 'Address in use' "$W/client.err" | head -3 >&2
    exit 1
  fi
}
gai() { # numeric IPv6 addresses with the fake resolver
  echo "## getaddrinfo() with resolveprotocol: fake"
  { echo "resolveprotocol: fake"; cat "$D/socks.conf"; } > "$W/socks.conf"
  timeout 20 env SOCKS_CONF="$W/socks.conf" LD_PRELOAD="$LIB" "$W/gaicheck" \
    2> "$W/client.err" || { cat "$W/client.err" >&2; exit 1; }
}
ep() { # epoll registrations survive libdsocks replacing the socket
  echo "## 20 non-blocking connects watched with epoll via $GW"
  srv "$GW" 1 0 "$HOST"
  client 20 "$W/epollcheck" 20
}
dr() { # resolver calls in other threads must not switch proxying off
  echo "## connects proxied while other threads resolve, via $GW"
  srv "$GW" 0 0 "$HOST"
  client 60 "$W/dnsracecheck" 5
}
sc() { # numbers of finished, closed connects work for new sockets
  echo "## connects on reused descriptor numbers after refused connects, via $GW"
  srv "$GW" 0 1 "$HOST"
  client 30 "$W/stalecheck" 20
}
fr() { # connects given up on while negotiating; their numbers reused for files
  echo "## abandoned non-blocking connects, descriptors reused for files, via $GW"
  srv "$GW" 1 0 "$HOST"
  client 30 "$W/fdreusecheck" 30
}
ud() { # the server ends a UDP association; the next datagram needs a new one
  echo "## UDP association ended by the server via $GW"
  export SLOWSOCKS_UDP_TTL=2
  srv "$GW" 0 0 "$HOST"
  unset SLOWSOCKS_UDP_TTL
  client 30 "$W/udpdropcheck" 4
}
gs() { # getsockname(2) right after a non-blocking connect, threaded
  echo "## getsockname() during non-blocking connects from 4 threads via $GW"
  srv "$GW" 1 0 "$HOST"
  client 30 "$W/getsockcheck" 4 5
}
rv() { # readv(2) with several iovecs on a proxied socket
  echo "## readv() with 4 iovecs via $GW"
  srv "$GW" 0 0 "$HOST"
  client 20 "$W/readvcheck"
}
mt() { # threads racing for the connect-child, short waits, fd reuse
  echo "## 8 threads churning non-blocking connects via $GW (one connect-child)"
  srv "$GW" 2 0 "$HOST"
  client 40 SOCKS_CONNECTCHILD_MAXWORKERS=20 "$W/mtstress" 8 10 50
}
v6() { # GATEWAY HOST UDP_HOST TCP_FAMILY UDP_FAMILY
  echo "## TCP and UDP via $1 (relay $3)"
  srv "$1" 0 0 "$2" "$3"
  client 60 "$W/v6check" "$4" "$5"
}

for HOST in 127.0.0.1 ::1; do
  GW=$HOST
  t 5 0 60 15 60 0
  t 5 0 60 15 20 0 SOCKS_CONNECTCHILD_MAXWORKERS=20
  t 60 0 20 8 0 2 SOCKS_CONNECTCHILD_TIMEOUT=3
  t 2 3 30 10 20 0
  t 6 0 500 20 128 0
  mt
  rv
  ep
  gs
  ud
  fr
  dr
  sc
done

gai
bp
v6 127.0.0.1 127.0.0.1 127.0.0.1 4 4
v6 ::1 ::1 ::1 6 6
v6 ::1 ::1 127.0.0.1 6 4
v6 ip6-localhost ::1 ::1 6 6
HOST=::1 GW=ip6-localhost
t 5 0 60 15 60 0
