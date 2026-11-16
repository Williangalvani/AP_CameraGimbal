#!/bin/sh
# On the camera: run MCM from /run/mcm/root beside camera-app. MAVLink and
# RTSP are moved off camera-app's ports (UDP 14550, RTSP 8554); MCM keeps its
# REST 6020, signalling 6021 and STUN 3478. The ONVIF login is the
# [compat] onvif_password default. With apcam sources pass --disable-onvif:
# ONVIF discovery opens RTSP sessions to camera-app every ~26 s.
exec /run/mcm/root/run.sh \
    --mavlink udpin:127.0.0.1:14560 \
    --rtsp-port 8564 \
    --onvif-auth "onvif://admin:${ONVIF_PASSWORD:-blue}@${CAMERA_IP:-192.168.2.10}" \
    --settings-file /run/mcm/state/settings.json \
    --log-path /run/mcm/state/logs \
    --reset "$@"
