#!/bin/sh
# On the camera: run a test camera-app from RAM in place of the flashed one,
# through service.sh's restart protocol so its supervisor stops the old one.
# Nothing is written to flash; a reboot or `swap_camera_app.sh restore`
# undoes it.
# Usage: swap_camera_app.sh http://HOST:PORT/path/camera-app | restore
set -eu
BIN=/appfs/ap_cameragimbal/bin/camera-app
TEST=/run/ca-test/camera-app

mounted() { awk -v path="$BIN" '$2 == path {found=1} END {exit !found}' /proc/mounts; }

restart()
{
    old=$(pidof camera-app || true)
    (
        flock -x 8
        n=$(cat /run/camera-app.request 2>/dev/null || echo 0)
        echo $((n + 1)) >/run/camera-app.request
    ) 8>/run/camera-app.request.lock
    for _ in $(seq 1 90); do
        new=$(pidof camera-app || true)
        if [ -n "$new" ] && [ "$new" != "$old" ] && [ -f /run/camera-app.ready ]; then
            echo "camera-app $new ready ($(md5sum "/proc/$new/exe" | cut -c1-12))"
            return 0
        fi
        sleep 1
    done
    echo "camera-app did not become ready; see /run/camera_app.log" >&2
    return 1
}

# The running camera-app keeps the bound binary busy, so a plain umount
# fails; a lazy one detaches it and the restarted camera-app execs the
# flashed binary.
if [ "${1-}" = restore ]; then
    if mounted; then umount -l "$BIN"; fi
    restart
    exit
fi
mkdir -p /run/ca-test
wget -q -O "$TEST.part" "$1"
chmod +x "$TEST.part"
mv "$TEST.part" "$TEST"
if mounted; then umount -l "$BIN"; fi
mount --bind "$TEST" "$BIN"
restart
