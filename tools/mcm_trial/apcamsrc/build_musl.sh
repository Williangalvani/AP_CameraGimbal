#!/bin/sh
# Builds libgstapcam.so for the musl MCM bundle, against the same Alpine
# GStreamer, in an arm64 Alpine container (binfmt/qemu, as in PLAN.md Task 1):
#   docker run --rm --platform linux/arm64 --network host -v $PWD:/work -w /work \
#       alpine:3.22 sh build_musl.sh
set -eu
apk add -q gcc musl-dev pkgconf gstreamer-dev
mkdir -p build/musl
gcc -O2 -Wall -Wextra -Wno-missing-field-initializers -Werror -std=gnu11 -fPIC -shared \
    -o build/musl/libgstapcam.so gstapcamsrc.c \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-base-1.0)
strip build/musl/libgstapcam.so
ls -l build/musl/libgstapcam.so
