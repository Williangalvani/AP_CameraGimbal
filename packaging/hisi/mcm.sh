#!/bin/sh
# MAVLink Camera Manager (MCM), run by service.sh when the appfs carries its
# bundle in mcm/. The bundle's loader and RPATH name /run/mcm/root, so that is
# made a symlink to the bundle; a mount there is an MCM trial from RAM and is
# left alone. MCM reads camera-app's encoded frames from its frame socket
# (apcamsrc) and keeps off camera-app's ports: MAVLink on udpin
# 127.0.0.1:14560, RTSP on 8564, no ONVIF. Its REST API (6020), WebRTC
# signalling (6021) and STUN (3478) are its defaults. MCM rewrites its
# settings file, so the streams in mcm-settings.json are copied fresh at every
# start. Extra arguments are passed on to MCM.
set -eu
APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUNDLE=$APP_ROOT/mcm
ROOT=/run/mcm/root
export MCM_STATE=/run/mcm/state
[ -f "$BUNDLE/run.sh" ] || { echo "No MCM bundle in $BUNDLE" >&2; exit 1; }
mkdir -p /run/mcm
if [ ! -L "$ROOT" ] && [ -d "$ROOT" ]; then
    if mountpoint -q "$ROOT"; then
        echo "$ROOT is a mount (an MCM trial from RAM?); not starting MCM" >&2
        exit 1
    fi
    rmdir "$ROOT" || { echo "$ROOT is a directory that is not empty" >&2; exit 1; }
fi
ln -sfn "$BUNDLE" "$ROOT"
mkdir -p "$MCM_STATE"
cp "$APP_ROOT/mcm-settings.json" "$MCM_STATE/settings.json.new"
mv -f "$MCM_STATE/settings.json.new" "$MCM_STATE/settings.json"
exec "$ROOT/run.sh" \
    --mavlink udpin:127.0.0.1:14560 \
    --rtsp-port 8564 \
    --disable-onvif \
    --settings-file "$MCM_STATE/settings.json" \
    --log-path "$MCM_STATE/logs" \
    "$@"
