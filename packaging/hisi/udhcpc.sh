#!/bin/sh
# udhcpc event script, run by the DHCP client service.sh starts when
# camera.ini sets [network] dhcp = true. The lease goes on $interface:1 and
# the default route follows its router. The address the rootfs updater puts on
# $interface (192.168.2.10 unless updatetype.ini says otherwise) is left alone,
# so the camera stays reachable there with or without a lease; when a lease
# ends, the default route from before the first lease comes back.
set -u
SAVED_GATEWAY=${DHCP_SAVED_GATEWAY:-/run/udhcpc.gateway}
label=$interface:1

default_gateway()
{
    route -n | awk '$1 == "0.0.0.0" { print $2; exit }'
}
set_default()
{
    while route del default 2>/dev/null; do :; done
    [ -z "$1" ] || route add default gw "$1" dev "$interface"
}

case $1 in
bound|renew)
    ifconfig "$label" "$ip" netmask "${subnet:-255.255.255.0}" up || exit 1
    echo "udhcpc: $1 $ip on $label, router ${router:-none}" >&2
    [ -n "${router:-}" ] || exit 0
    set -- $router
    [ -e "$SAVED_GATEWAY" ] || default_gateway >"$SAVED_GATEWAY"
    [ "$(default_gateway)" = "$1" ] || set_default "$1"
    ;;
deconfig)
    ifconfig "$label" down 2>/dev/null
    if [ -e "$SAVED_GATEWAY" ]; then
        set_default "$(cat "$SAVED_GATEWAY")"
        rm -f "$SAVED_GATEWAY"
        echo "udhcpc: lease ended; $interface keeps its own address" >&2
    fi
    ;;
esac
exit 0
