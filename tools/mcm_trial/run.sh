#!/bin/sh
# Runs MCM, or gst-launch-1.0 / gst-inspect-1.0 when named first, from this
# bundle. The binaries' loader path assumes the bundle is on /run/mcm/root.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STATE=${MCM_STATE:-/run/mcm/state}
mkdir -p "$STATE"
export LD_LIBRARY_PATH="$ROOT/lib"
export GST_PLUGIN_SYSTEM_PATH_1_0="$ROOT/lib/gstreamer-1.0"
export GST_PLUGIN_PATH_1_0=
export GST_REGISTRY_1_0="$STATE/gst-registry.bin"
# gst-plugin-scanner would run under the rootfs glibc
export GST_REGISTRY_FORK=no
export GIO_MODULE_DIR="$ROOT/lib/gio-modules"
export HOME="$STATE"
tool=mavlink-camera-manager
case ${1-} in
gst-launch-1.0|gst-inspect-1.0) tool=$1; shift ;;
esac
exec "$ROOT/bin/$tool" "$@"
