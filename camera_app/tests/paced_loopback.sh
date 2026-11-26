#!/bin/sh
# Run a command in a private network namespace whose loopback carries UDP at
# 40 Mbit/s and TCP unlimited, so the RTSP server's UDP sends hit a full link
# while its control connections stay fast. Skips (exit 0) where unprivileged
# network namespaces or tc are unavailable.
set -e
if [ "$1" != --inside ]; then
    if ! command -v tc >/dev/null 2>&1 || ! command -v ip >/dev/null 2>&1 ||
            ! unshare -rn true 2>/dev/null; then
        echo "SKIP paced loopback: needs ip, tc and unprivileged network namespaces"
        exit 0
    fi
    exec unshare -rn "$0" --inside "$@"
fi
shift
ip link set lo up
tc qdisc add dev lo root handle 1: prio
tc qdisc add dev lo parent 1:3 handle 30: tbf rate 40mbit burst 64kb latency 200ms
tc filter add dev lo parent 1: protocol ip prio 1 u32 match ip protocol 17 0xff flowid 1:3
exec "$@"
