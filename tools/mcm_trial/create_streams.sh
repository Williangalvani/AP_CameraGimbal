#!/bin/sh
# From the host: wait until the camera's MCM has discovered camera-app over
# ONVIF, then create its main (4K) and sub (720p) streams. Thumbnails are off
# (the lean bundle has no decoder), and so is MCM's MAVLink camera, since
# camera-app already is one. Usage: create_streams.sh [CAMERA_IP]
set -eu
CAM=${1:-192.168.2.10}
API=http://$CAM:6020
for _ in $(seq 1 60); do
    if curl -fsS "$API/v4l" | grep -q "rtsp://$CAM:8554/stream_0"; then
        break
    fi
    sleep 2
done
curl -fsS "$API/v4l" | grep -q "rtsp://$CAM:8554/stream_0" ||
    { echo "MCM did not discover rtsp://$CAM:8554/stream_0" >&2; exit 1; }

post()
{
    curl -fsS -X POST "$API/streams" -H 'Content-Type: application/json' -d @- <<EOF
{
  "name": "$1",
  "source": "rtsp://$CAM:8554/$2",
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
post "4K Cam main (on camera)" stream_0 cam_main 3840 2160
post "4K Cam sub (on camera)" stream_1 cam_sub 1280 720
curl -fsS "$API/streams"
echo
