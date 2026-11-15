#!/bin/sh
# Checks a staged bundle in an arm64 alpine container, which has no glibc,
# with the bundle on /run/mcm/root: every element the trial needs loads, and
# MCM starts and answers its REST API. Usage: verify_bundle.sh (in container)
set -u
B=/run/mcm/root/run.sh
LOG=/run/mcm/state/verify-mcm.log
fail=0
mkdir -p /run/mcm/state

for element in capsfilter queue tee valve identity appsrc appsink proxysrc \
    proxysink rtspsrc rtph264depay rtph265depay h264parse h265parse \
    rtph264pay rtph265pay rtpbin rtpjitterbuffer rtpulpfecenc rtpredenc \
    udpsrc multiudpsink webrtcbin nicesrc nicesink dtlssrtpenc dtlssrtpdec \
    srtpenc srtpdec sctpenc sctpdec; do
    if $B gst-inspect-1.0 --exists "$element"; then
        echo "element $element: ok"
    else
        echo "element $element: MISSING"
        fail=1
    fi
done
$B gst-launch-1.0 -q fakesrc num-buffers=10 ! queue ! fakesink || fail=1

$B --version || fail=1
$B --mavlink udpin:127.0.0.1:14560 --rtsp-port 8564 --disable-onvif \
    --settings-file /run/mcm/state/settings.json --log-path /run/mcm/state/logs \
    --reset >"$LOG" 2>&1 &
mcm=$!
up=0
for _ in $(seq 1 120); do
    if wget -q -O /run/mcm/state/streams.json http://127.0.0.1:6020/streams 2>/dev/null; then
        up=1
        break
    fi
    kill -0 "$mcm" 2>/dev/null || break
    sleep 2
done
if [ "$up" = 1 ]; then
    echo "MCM REST /streams: $(cat /run/mcm/state/streams.json)"
    wget -q -O - http://127.0.0.1:6020/v4l | head -c 400
    echo
else
    echo "MCM did not answer on 6020"
    fail=1
fi
kill "$mcm" 2>/dev/null
wait "$mcm" 2>/dev/null
echo "--- MCM plugin warnings and errors"
grep -E 'requirement|ERROR|panicked' "$LOG" | sed 's/\x1b\[[0-9;]*m//g' | head -40
exit $fail
