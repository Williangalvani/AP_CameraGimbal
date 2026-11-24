#!/bin/sh
# Decode an H.265 recording of a real encoder's stream with FFmpeg. Optional:
# skipped without ffmpeg and ffprobe, or an ffmpeg without libx265.
set -eu
test_binary=$1
if ! command -v ffmpeg >/dev/null 2>&1 || ! command -v ffprobe >/dev/null 2>&1 ||
   ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx265; then
    echo "SKIP H.265 MP4 decode check: needs ffmpeg with libx265 and ffprobe"
    exit 0
fi
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc=size=320x240:rate=10 -t 3 \
    -c:v libx265 \
    -x265-params log-level=none:aud=1:repeat-headers=1:bframes=0:keyint=10:min-keyint=10:scenecut=0 \
    -pix_fmt yuv420p -f hevc -y "$dir/input.h265"
frames=$("$test_binary" "$dir/input.h265" "$dir/output.mp4" | sed -n 's/^frames=//p')
probe=$(ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=codec_name,codec_tag_string,width,height,nb_read_frames \
    -of default=noprint_wrappers=1 "$dir/output.mp4" 2>"$dir/probe.err")
if [ -s "$dir/probe.err" ]; then cat "$dir/probe.err" >&2; exit 1; fi
for expected in codec_name=hevc codec_tag_string=hvc1 width=320 height=240 "nb_read_frames=$frames"; do
    echo "$probe" | grep -qx "$expected" || { echo "ffprobe: no $expected in: $probe" >&2; exit 1; }
done
ffmpeg -v error -i "$dir/output.mp4" -f null - 2>"$dir/decode.err"
if [ -s "$dir/decode.err" ]; then cat "$dir/decode.err" >&2; exit 1; fi
echo "PASS H.265 MP4 decodes with FFmpeg: hevc in hvc1, $frames frames"
