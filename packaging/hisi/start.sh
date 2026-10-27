#!/bin/sh
# Installed as /appfs/sh/start.sh in place of the vendor launcher. rcS starts
# it in the background after the rootfs recovery updater (/usr/hiface/update,
# TCP 9091, which also applies the saved eth0 address) and before telnetd.
# xipc is not started; service.sh takes over its MPP and SD card duties.

# xipc mounted these; rootfs /tmp and /var are not writable.
mountpoint -q /tmp || mount -t tmpfs -o size=16M tmpfs /tmp
if ! mountpoint -q /var; then
    mount -t tmpfs -o size=8M tmpfs /var
    mkdir -p /var/run /var/lock
fi
ifconfig lo 127.0.0.1 up

exec /appfs/ap_cameragimbal/service.sh </dev/null >>/run/hisi-service.log 2>&1
