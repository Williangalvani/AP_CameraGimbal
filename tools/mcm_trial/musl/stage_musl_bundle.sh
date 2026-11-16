#!/bin/sh
# Stages the musl MCM build with Alpine aarch64 GStreamer into a bundle that
# mounts on /run/mcm/root, like build_bundle.sh does for the glibc release.
# Runs in x86_64 alpine after build_musl_mcm.sh, from ~/mcm_trial on willpc:
#   docker run --rm --network host -v $PWD:/work -w /work alpine:3.22 sh musl/stage_musl_bundle.sh
set -eu

ALPINE=v3.22
CACHE=/work/musl/cache
RT=$CACHE/runtime
MCM=$CACHE/target-gnu/aarch64-unknown-linux-musl/release/mavlink-camera-manager
OUT=/work/out
STAGE=$OUT/stage-musl
PREFIX=/run/mcm/root
PLUGINS='coreelements app typefindfunctions rtsp rtp rtpmanager udp videoparsersbad webrtc nice dtls srtp sctp proxy video4linux2'

apk add -q llvm patchelf squashfs-tools

if [ ! -f "$RT/.done" ]; then
    rm -rf "$RT"
    mkdir -p "$RT/etc/apk/keys"
    cp /usr/share/apk/keys/aarch64/* "$RT/etc/apk/keys/"
    printf '%s\n' "https://dl-cdn.alpinelinux.org/alpine/$ALPINE/main" \
        "https://dl-cdn.alpinelinux.org/alpine/$ALPINE/community" > "$RT/etc/apk/repositories"
    apk add -q --root "$RT" --arch aarch64 --initdb --no-scripts --update-cache \
        musl gstreamer gstreamer-tools gst-plugins-base gst-plugins-good gst-plugins-bad \
        gst-rtsp-server libnice-gstreamer
    touch "$RT/.done"
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/lib/gstreamer-1.0" "$STAGE/lib/gio-modules"
cp "$MCM" "$STAGE/bin/"
cp "$RT/usr/bin/gst-launch-1.0" "$RT/usr/bin/gst-inspect-1.0" "$STAGE/bin/"
cp "$RT/usr/libexec/gstreamer-1.0/gst-plugin-scanner" "$STAGE/lib/" 2>/dev/null || true
for p in $PLUGINS; do
    cp "$RT/usr/lib/gstreamer-1.0/libgst$p.so" "$STAGE/lib/gstreamer-1.0/"
done
# apcamsrc (apcamsrc/build_musl.sh), when built
APCAM=/work/apcamsrc/build/musl/libgstapcam.so
[ ! -f "$APCAM" ] || cp "$APCAM" "$STAGE/lib/gstreamer-1.0/"
cp -L "$RT/lib/ld-musl-aarch64.so.1" "$STAGE/lib/"
ln -s ld-musl-aarch64.so.1 "$STAGE/lib/libc.musl-aarch64.so.1"

find_lib() {
    for d in "$RT/lib" "$RT/usr/lib"; do
        [ -e "$d/$1" ] && { echo "$d/$1"; return; }
    done
    echo "missing library $1" >&2
    exit 1
}
needed() { llvm-readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

: > "$OUT/deps-musl.txt"
queue=$(find "$STAGE/bin" "$STAGE/lib" -type f)
while [ -n "$queue" ]; do
    next=
    for f in $queue; do
        for n in $(needed "$f"); do
            case $n in libc.musl-aarch64.so.1|ld-musl-aarch64.so.1) continue ;; esac
            [ -e "$STAGE/lib/$n" ] && continue
            src=$(find_lib "$n")
            cp -L "$src" "$STAGE/lib/$n"
            echo "$n <- $(basename "$f")" >> "$OUT/deps-musl.txt"
            next="$next $STAGE/lib/$n"
        done
    done
    queue=$next
done

for f in "$STAGE"/bin/*; do
    patchelf --set-interpreter "$PREFIX/lib/ld-musl-aarch64.so.1" --set-rpath "$PREFIX/lib" "$f"
done
cp /work/run.sh "$STAGE/run.sh"
chmod +x "$STAGE/run.sh"

rm -f "$OUT/mcm-musl.sqfs"
mksquashfs "$STAGE" "$OUT/mcm-musl.sqfs" -noappend -comp xz -b 65536 -all-root -no-xattrs \
    -mkfs-time 0 -all-time 0 -quiet
ls -l "$MCM" "$OUT/mcm-musl.sqfs"
du -sb "$STAGE" "$STAGE/bin/mavlink-camera-manager" "$STAGE/lib"
