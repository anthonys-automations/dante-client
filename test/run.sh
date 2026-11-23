#!/bin/sh
# Non-blocking connect test for libdsocks with dante-connectchild-workers.patch.
# Needs cc and python3; all traffic stays on 127.0.0.1.
#   usage: run.sh /path/to/libdsocks.so
# Expected with the patch: no connect() blocks for more than a few ms and
# nothing is left pending. Unpatched, the first case hangs in connect().
set -eu
LIB=$1
D=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
cc -O2 -o "$W/nbconnect" "$D/nbconnect.c"
SRV=
trap '[ -n "$SRV" ] && kill $SRV 2>/dev/null; true' EXIT

srv() { # DELAY FAIL_EVERY
  [ -n "$SRV" ] && kill $SRV 2>/dev/null && sleep 1
  python3 -u "$D/slowsocks.py" 1081 "$1" "$2" > "$W/socks.log" 2>&1 &
  SRV=$!
  sleep 1.5
}

t() { # NAME DELAY FAIL_EVERY N WAIT [ENV...]
  name=$1 delay=$2 fail=$3 n=$4 wait=$5; shift 5
  echo "## $name"
  srv "$delay" "$fail"
  env "$@" SOCKS_CONF="$D/socks.conf" LD_PRELOAD="$LIB" \
    timeout $((wait + 30)) "$W/nbconnect" "$n" "$wait" 2> "$W/client.err" || echo "client rc=$? (124: timed out, connect() blocked)"
}

t "60 connects, proxy delay 5s: all succeed after ~5s" 5 0 60 15
t "worker cap 20: 20 succeed, 40 fail at once" 5 0 60 10 SOCKS_CONNECTCHILD_MAXWORKERS=20
t "worker timeout 3s, proxy delay 60s: all fail after ~3s" 60 0 20 8 SOCKS_CONNECTCHILD_TIMEOUT=3
t "proxy refuses every 3rd: 20 succeed, 10 fail" 2 3 30 8
t "500 connects, default cap 128: 128 succeed, rest fail at once" 2 0 500 15
rm -rf "$W"
