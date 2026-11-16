#!/bin/sh
# From the host: create the camera's MCM main (4K) and sub (720p) streams.
# The source is camera-app's RTSP as MCM discovers it over ONVIF (onvif, the
# default) or camera-app's frame socket through apcamsrc (apcam). Thumbnails
# are off (the lean bundle has no decoder), and so is MCM's MAVLink camera,
# since camera-app already is one.
# Usage: create_streams.sh [CAMERA_IP] [onvif|apcam]
set -eu
CAM=${1:-192.168.2.10}
MODE=${2:-onvif}
API=http://$CAM:6020
case $MODE in
onvif) MAIN=rtsp://$CAM:8554/stream_0 SUB=rtsp://$CAM:8554/stream_1 ;;
apcam) MAIN=apcam:main SUB=apcam:sub ;;
*) echo "unknown source $MODE" >&2; exit 2 ;;
esac
for _ in $(seq 1 60); do
    if curl -fsS "$API/v4l" | grep -q "\"$MAIN\""; then
        break
    fi
    sleep 2
done
curl -fsS "$API/v4l" | grep -q "\"$MAIN\"" ||
    { echo "MCM does not offer $MAIN" >&2; exit 1; }

post()
{
    curl -fsS -X POST "$API/streams" -H 'Content-Type: application/json' -d @- <<EOF
{
  "name": "$1",
  "source": "$2",
  "stream_information": {
    "endpoints": ["rtsp://0.0.0.0:8564/$3"],
    "configuration": {
      "type": "video", "encode": "H264", "width": $4, "height": $5,
      "frame_interval": {"numerator": 1, "denominator": 30}
    },
    "extended_configuration": {
      "disable_mavlink": true, "disable_thumbnails": true,
      "disable_zenoh": true, "disable_recording": true
    }
  }
}
EOF
    echo
}
post "4K Cam main (on camera)" "$MAIN" cam_main 3840 2160
post "4K Cam sub (on camera)" "$SUB" cam_sub 1280 720
curl -fsS "$API/streams"
echo
