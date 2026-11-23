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
}

t() { # DELAY FAIL_EVERY N WAIT EXPECTED_SUCCESSES MIN_SECONDS [ENV...]
  delay=$1 fail=$2 n=$3 wait=$4 ok=$5 min=$6; shift 6
  echo "## $n non-blocking connects via $GW: delay=$delay refuse_every=$fail expected_ok=$ok $*"
  srv "$GW" "$delay" "$fail" "$HOST"
  client "$((wait + 5))" "$@" "$W/nbconnect" "$n" "$wait" "$ok" "$min"
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
done

v6 127.0.0.1 127.0.0.1 127.0.0.1 4 4
v6 ::1 ::1 ::1 6 6
v6 ::1 ::1 127.0.0.1 6 4
v6 ip6-localhost ::1 ::1 6 6
HOST=::1 GW=ip6-localhost
t 5 0 60 15 60 0
