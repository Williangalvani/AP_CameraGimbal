#!/bin/sh
# RAM-only redeploy: writable copy of our appfs in /run/apw bound over /appfs,
# /date replaced by a tmpfs, the real service.sh supervising. Reboot undoes it.
H=http://192.168.2.15:8088/out/kfserve
A=/run/apw/ap_cameragimbal
stop_all() {
    [ -f /run/hisi-service.pid ] && kill "$(cat /run/hisi-service.pid)" 2>/dev/null
    killall hisi-web 2>/dev/null
    killall camera-app ld-musl-aarch64.so.1 2>/dev/null
    for i in $(seq 1 15); do pidof camera-app ld-musl-aarch64.so.1 >/dev/null || break; sleep 1; done
    killall -9 camera-app ld-musl-aarch64.so.1 2>/dev/null
    rm -f /run/hisi-service.pid
}
stop_all
mountpoint -q /run/ap || { echo "run setup.sh first"; exit 1; }
if [ ! -d /run/apw ]; then
    mkdir -p /run/apw && cp -a /run/ap/. /run/apw/
fi
top_appfs() { awk '$2 == "/appfs" {d=$1} END {print d}' /proc/mounts; }
boot_appfs=$(awk '$2 == "/appfs" {print $1; exit}' /proc/mounts)
while [ "$(top_appfs)" != "$boot_appfs" ]; do umount /appfs || exit 1; done
mount --bind /run/apw /appfs || exit 1
grep -q "^tmpfs /date " /proc/mounts || mount -t tmpfs -o size=4m tmpfs /date
mkdir -p /date/ap_cameragimbal
[ -e /date/ap_cameragimbal/camera.ini ] || cp /run/apcfg/camera.ini /date/ap_cameragimbal/camera.ini
for f in $(wget -q -O - $H/deploy.list); do
    wget -q -O /run/deploy.tmp $H/$f && mv /run/deploy.tmp $A/$f && echo "updated $f"
done
chmod +x $A/bin/* $A/service.sh
sh /run/mpp_reload.sh
setsid nohup /appfs/ap_cameragimbal/service.sh </dev/null >>/run/hisi-service.log 2>&1 &
sleep 12
tail -5 /run/hisi-service.log
grep "media ready\|first encoded\|failed\|error" /run/camera_app.log | tail -8
