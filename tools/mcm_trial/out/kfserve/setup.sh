#!/bin/sh
# RAM-only bring-up: nothing is written to flash. A reboot undoes all of it.
H=http://192.168.2.15:8088/out/kfserve
cd /run || exit 1
if ! mountpoint -q /run/ap; then
    wget -q -O /run/ours.sqfs $H/ours.sqfs || exit 1
    mkdir -p /run/ap && mount -t squashfs -o loop,ro /run/ours.sqfs /run/ap || exit 1
fi
mkdir -p /run/apcfg
[ -f /run/apcfg/camera.ini ] || cp /run/ap/ap_cameragimbal/camera.ini.default /run/apcfg/camera.ini
wget -q -O /run/run_ca.sh $H/run_ca.sh && chmod +x /run/run_ca.sh
cat > /run/wdfeed.sh <<"EOF"
#!/bin/sh
# ot_wdt has no write(); the last close feeds it.
while :; do
  if true > /dev/watchdog; then sleep 1; else usleep 100000; fi
done
EOF
chmod +x /run/wdfeed.sh
if pidof xipc >/dev/null; then
    killall -9 xipc device_service
    setsid nohup /run/wdfeed.sh >/run/wd.log 2>&1 </dev/null &
fi
echo setup done
