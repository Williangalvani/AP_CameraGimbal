#!/bin/sh
# Builds the MCM RAM-trial bundles. Runs inside an arm64 debian:trixie
# container with this directory on /work, and writes to /work/out.
# Each bundle carries its own glibc (the camera's rootfs has 2.34, trixie
# 2.41), so its binaries are patched to the loader under ROOT, where the
# camera must mount the bundle (in appfs, a /run/mcm/root symlink to it).
set -eu
MCM_TAG=${MCM_TAG:-t3.26.3}
LIBNICE_VERSION=0.1.22
LIBSRTP_VERSION=2.6.0
ROOT=/run/mcm/root
WORK=/work
OUT=$WORK/out
SYSLIB=/usr/lib/aarch64-linux-gnu
PLUGINS=$SYSLIB/gstreamer-1.0
# libnice and libsrtp2 rebuilt against OpenSSL: Debian's pull in gnutls,
# gupnp-igd, libsoup, sqlite, Kerberos (libnice) and NSS (libsrtp2).
SMALL=$OUT/small-prefix
export DEBIAN_FRONTEND=noninteractive

apt-get update -q
apt-get install -y -q --no-install-recommends ca-certificates curl patchelf \
    squashfs-tools libgstreamer1.0-0 gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libgstrtspserver-1.0-0 \
    gstreamer1.0-nice gstreamer1.0-tools \
    gcc libc6-dev meson ninja-build pkgconf libglib2.0-dev libssl-dev \
    libgstreamer1.0-dev

mkdir -p "$OUT"
MCM=$OUT/mavlink-camera-manager-$MCM_TAG
if [ ! -s "$MCM" ]; then
    curl -fL -o "$MCM.part" \
        "https://github.com/mavlink/mavlink-camera-manager/releases/download/$MCM_TAG/mavlink-camera-manager-aarch64"
    mv "$MCM.part" "$MCM"
fi
chmod +x "$MCM"

build_small_libs()
{
    [ -s "$SMALL/lib/libnice.so.10" ] && [ -s "$SMALL/lib/libsrtp2.so.1" ] && return 0
    src=$OUT/src
    mkdir -p "$src"
    cd "$src"
    [ -s libnice.tar.gz ] || curl -fL -o libnice.tar.gz \
        "https://libnice.freedesktop.org/releases/libnice-$LIBNICE_VERSION.tar.gz"
    [ -s libsrtp.tar.gz ] || curl -fL -o libsrtp.tar.gz \
        "https://github.com/cisco/libsrtp/archive/refs/tags/v$LIBSRTP_VERSION.tar.gz"
    rm -rf "libnice-$LIBNICE_VERSION" "libsrtp-$LIBSRTP_VERSION"
    tar xzf libnice.tar.gz
    tar xzf libsrtp.tar.gz
    meson setup "libnice-$LIBNICE_VERSION/build" "libnice-$LIBNICE_VERSION" \
        --prefix="$SMALL" --libdir=lib --buildtype=release -Dcrypto-library=openssl \
        -Dgupnp=disabled -Dgstreamer=enabled -Dexamples=disabled -Dtests=disabled \
        -Dintrospection=disabled -Dgtk_doc=disabled
    ninja -C "libnice-$LIBNICE_VERSION/build" install
    meson setup "libsrtp-$LIBSRTP_VERSION/build" "libsrtp-$LIBSRTP_VERSION" \
        --prefix="$SMALL" --libdir=lib --buildtype=release -Ddefault_library=shared \
        -Dcrypto-library=openssl -Dtests=disabled -Ddoc=disabled
    ninja -C "libsrtp-$LIBSRTP_VERSION/build" install
    cd "$WORK"
}

# Same mksquashfs options as tools/AP_CameraGimbal/tools/hisi_firmware.py.
squash()
{
    mksquashfs "$1" "$2" -noappend -comp xz -b 65536 -all-root -no-xattrs \
        -mkfs-time 0 -all-time 0 -quiet
}

# stage NAME LIBRARY_PATH all | stage NAME LIBRARY_PATH PLUGIN...
# PLUGIN as in libgstPLUGIN.so. Libraries and plugins in LIBRARY_PATH (may be
# empty) take precedence over the system's.
stage()
{
    name=$1
    path=$2
    shift 2
    dir=$OUT/stage-$name
    rm -rf "$dir"
    mkdir -p "$dir/bin" "$dir/lib/gstreamer-1.0"
    cp "$MCM" "$dir/bin/mavlink-camera-manager"
    cp /usr/bin/gst-launch-1.0 /usr/bin/gst-inspect-1.0 "$dir/bin/"
    if [ "$1" = all ]; then
        cp "$PLUGINS"/*.so "$dir/lib/gstreamer-1.0/"
    else
        for plugin in "$@"; do
            file=libgst$plugin.so
            if [ -n "$path" ] && [ -e "$path/gstreamer-1.0/$file" ]; then
                cp "$path/gstreamer-1.0/$file" "$dir/lib/gstreamer-1.0/"
            else
                cp "$PLUGINS/$file" "$dir/lib/gstreamer-1.0/"
            fi
        done
    fi
    for file in "$dir"/bin/* "$dir"/lib/gstreamer-1.0/*.so; do
        LD_LIBRARY_PATH=$path ldd "$file" | awk -v f="${file##*/}" '$2 == "=>" {print f, $1}'
    done >"$OUT/deps-$name.txt"
    for file in "$dir"/bin/* "$dir"/lib/gstreamer-1.0/*.so; do
        LD_LIBRARY_PATH=$path ldd "$file"
    done | awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\// {print $1}' |
        sort -u | while read -r lib; do
            cp -L "$lib" "$dir/lib/${lib##*/}"
        done
    for file in "$dir"/bin/*; do
        patchelf --set-interpreter "$ROOT/lib/ld-linux-aarch64.so.1" \
            --force-rpath --set-rpath "$ROOT/lib" "$file"
    done
    cp "$WORK/run.sh" "$dir/run.sh"
    chmod +x "$dir/run.sh"
    squash "$dir" "$OUT/mcm-$name.sqfs"
}

# The elements MCM's ONVIF (RTSP client), RTSP server, UDP and WebRTC paths
# use, without thumbnails (avdec, jpeg, videoconvertscale) or test sources.
# Streams on this bundle need extended_configuration disable_thumbnails, or
# stream creation fails. video4linux2 is there for its device provider: MCM
# panics at startup if its device monitor has none.
LEAN='coreelements app typefindfunctions rtsp rtp rtpmanager udp
      videoparsersbad webrtc nice dtls srtp sctp proxy video4linux2'

build_small_libs
stage full '' all
# shellcheck disable=SC2086
stage lean-debian '' $LEAN
# shellcheck disable=SC2086
stage lean "$SMALL/lib" $LEAN

mcm_only=$OUT/stage-mcm-only
rm -rf "$mcm_only"
mkdir -p "$mcm_only"
cp "$MCM" "$mcm_only/mavlink-camera-manager"
squash "$mcm_only" "$OUT/mcm-mcm-only.sqfs"

{
    echo "MCM $MCM_TAG, GStreamer $(dpkg-query -W -f '${Version}' libgstreamer1.0-0)," \
        "lean: libnice $LIBNICE_VERSION and libsrtp $LIBSRTP_VERSION with OpenSSL"
    for name in mcm-only lean-debian lean full; do
        plugins=$(find "$OUT/stage-$name" -path '*/gstreamer-1.0/*.so' | wc -l)
        libs=$(find "$OUT/stage-$name" -maxdepth 2 -path '*/lib/*.so*' | wc -l)
        printf '%-11s staged %10s B  squashfs %10s B  %3s plugins  %3s libraries\n' \
            "$name" "$(du -sb "$OUT/stage-$name" | cut -f1)" \
            "$(stat -c %s "$OUT/mcm-$name.sqfs")" "$plugins" "$libs"
    done
} | tee "$OUT/sizes.txt"
