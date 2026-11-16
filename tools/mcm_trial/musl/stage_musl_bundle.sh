#!/bin/sh
# Stages the musl MCM build with Alpine aarch64 GStreamer into a bundle that
# mounts on /run/mcm/root, like build_bundle.sh does for the glibc release.
# Runs in x86_64 alpine after build_musl_mcm.sh, from ~/mcm_trial on willpc:
#   docker run --rm --network host -v $PWD:/work -w /work alpine:3.22 sh musl/stage_musl_bundle.sh
# Pruned bundle (PLAN.md Task 8), keeping only the libraries and plugins listed
# in KEEP (one path under /run/mcm/root or bare basename per line, e.g. the
# union of awk '$6 ~ /\.so/ {print $6}' /proc/PID/maps from working sessions):
#   docker run --rm --network host -v $PWD:/work -w /work -e KEEP=/work/out/keep.txt alpine:3.22 sh musl/stage_musl_bundle.sh
# With KEEP, gst-launch-1.0, gst-inspect-1.0 and gst-plugin-scanner are dropped
# unless KEEP_TOOLS=1. SQFS_BLOCK (default 65536, as appfs) and SQFS_EXTRA
# (extra mksquashfs options, e.g. '-Xbcj arm64') add a suffix to the sqfs name.
set -eu

ALPINE=v3.22
CACHE=/work/musl/cache
RT=$CACHE/runtime
MCM=$CACHE/target-gnu/aarch64-unknown-linux-musl/release/mavlink-camera-manager
OUT=/work/out
PREFIX=/run/mcm/root
PLUGINS='coreelements app typefindfunctions rtsp rtp rtpmanager udp videoparsersbad webrtc nice dtls srtp sctp proxy video4linux2'
KEEP=${KEEP:-}
KEEP_TOOLS=${KEEP_TOOLS:-}
SQFS_BLOCK=${SQFS_BLOCK:-}
SQFS_EXTRA=${SQFS_EXTRA:-}

VARIANT=musl
if [ -n "$KEEP" ]; then
    [ -f "$KEEP" ] || { echo "KEEP file $KEEP not found" >&2; exit 1; }
    VARIANT=musl-pruned
    # basenames of entries that are bare names or paths under $PREFIX
    keep_names=$(awk -v p="$PREFIX/" '
        { sub(/^[ \t\r]+/, ""); sub(/[ \t\r]+$/, "") }
        $0 == "" || /^#/ { next }
        index($0, "/") == 0 { print; next }
        index($0, p) == 1 { n = split($0, a, "/"); print a[n] }' "$KEEP" | sort -u)
    [ -n "$keep_names" ] || { echo "KEEP file $KEEP lists no libraries under $PREFIX" >&2; exit 1; }
fi
STAGE=$OUT/stage-$VARIANT
DEPS=$OUT/deps-$VARIANT.txt
SQFS_SUFFIX=${SQFS_BLOCK:+-b$SQFS_BLOCK}
[ -z "$SQFS_EXTRA" ] || SQFS_SUFFIX=$SQFS_SUFFIX$(printf ' %s' "$SQFS_EXTRA" | tr -cs 'A-Za-z0-9._' '-' | sed 's/-*$//')
SQFS=$OUT/mcm-$VARIANT$SQFS_SUFFIX.sqfs

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

: > "$DEPS"
queue=$(find "$STAGE/bin" "$STAGE/lib" -type f)
while [ -n "$queue" ]; do
    next=
    for f in $queue; do
        for n in $(needed "$f"); do
            case $n in libc.musl-aarch64.so.1|ld-musl-aarch64.so.1) continue ;; esac
            [ -e "$STAGE/lib/$n" ] && continue
            src=$(find_lib "$n")
            cp -L "$src" "$STAGE/lib/$n"
            echo "$n <- $(basename "$f")" >> "$DEPS"
            next="$next $STAGE/lib/$n"
        done
    done
    queue=$next
done

if [ -n "$KEEP" ]; then
    in_keep() { printf '%s\n' "$keep_names" | grep -qxF "$1"; }
    removed=0
    remove() {
        size=$(stat -c %s "$1")
        printf 'removed %10d %s\n' "$size" "${1#"$STAGE"/}"
        removed=$((removed + size))
        rm -f "$1"
    }
    if [ "$KEEP_TOOLS" != 1 ]; then
        for f in "$STAGE/bin/gst-launch-1.0" "$STAGE/bin/gst-inspect-1.0" "$STAGE/lib/gst-plugin-scanner"; do
            [ ! -f "$f" ] || remove "$f"
        done
    fi
    libs=$(find "$STAGE/lib" -type f \( -path "$STAGE/lib/gstreamer-1.0/*" -o -name '*.so' -o -name '*.so.*' \))
    for f in $libs; do
        n=$(basename "$f")
        [ "$n" = ld-musl-aarch64.so.1 ] && continue
        in_keep "$n" || remove "$f"
    done
    echo "removed $removed bytes in total"

    missing=
    libs=$(find "$STAGE/lib" -type f \( -name '*.so' -o -name '*.so.*' -o -name gst-plugin-scanner \))
    for f in "$STAGE"/bin/* $libs; do
        for n in $(needed "$f"); do
            case $n in libc.musl-aarch64.so.1|ld-musl-aarch64.so.1) continue ;; esac
            [ -e "$STAGE/lib/$n" ] && continue
            echo "missing after pruning: $n <- ${f#"$STAGE"/}" >&2
            missing=1
        done
    done
    [ -z "$missing" ] || exit 1
fi

for f in "$STAGE"/bin/*; do
    patchelf --set-interpreter "$PREFIX/lib/ld-musl-aarch64.so.1" --set-rpath "$PREFIX/lib" "$f"
done
cp /work/run.sh "$STAGE/run.sh"
chmod +x "$STAGE/run.sh"

rm -f "$SQFS"
# shellcheck disable=SC2086 # SQFS_EXTRA is a list of options
mksquashfs "$STAGE" "$SQFS" -noappend -comp xz -b "${SQFS_BLOCK:-65536}" -all-root -no-xattrs \
    -mkfs-time 0 -all-time 0 -quiet $SQFS_EXTRA
ls -l "$MCM" "$SQFS"
du -sb "$STAGE" "$STAGE/bin/mavlink-camera-manager" "$STAGE/lib"
