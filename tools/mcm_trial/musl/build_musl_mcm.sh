#!/bin/sh
# Cross-builds MCM for aarch64-unknown-linux-musl against Alpine's aarch64
# GStreamer. Two steps from ~/mcm_trial on willpc:
#   docker run --rm --network host -v $PWD:/work -w /work alpine:3.22 sh musl/build_musl_mcm.sh sysroot
#   docker run --rm --network host -v $PWD:/work -w /work debian:trixie-slim sh musl/build_musl_mcm.sh build
# Cargo runs on glibc: build scripts from rustup's musl host toolchain are
# static and cannot dlopen libclang for bindgen.
set -eu

ALPINE=v3.22
TARGET=aarch64-unknown-linux-musl
CACHE=/work/musl/cache
SYSROOT=$CACHE/sysroot

sysroot() {
    if [ ! -f "$SYSROOT/.done" ]; then
        rm -rf "$SYSROOT"
        mkdir -p "$SYSROOT/etc/apk/keys"
        cp /usr/share/apk/keys/aarch64/* "$SYSROOT/etc/apk/keys/"
        printf '%s\n' "https://dl-cdn.alpinelinux.org/alpine/$ALPINE/main" \
            "https://dl-cdn.alpinelinux.org/alpine/$ALPINE/community" > "$SYSROOT/etc/apk/repositories"
        apk add -q --root "$SYSROOT" --arch aarch64 --initdb --no-scripts --update-cache \
            musl-dev gcc libgcc glib-dev openssl-dev \
            gstreamer-dev gst-plugins-base-dev gst-plugins-bad-dev gst-rtsp-server-dev
        touch "$SYSROOT/.done"
    fi
    apk add -q --root "$SYSROOT" --arch aarch64 --no-scripts g++ linux-headers
}

build() {
    export CARGO_HOME=$CACHE/cargo-gnu RUSTUP_HOME=$CACHE/rustup-gnu
    export PATH=$CARGO_HOME/bin:$PATH
    apt-get update -q >/dev/null
    apt-get install -y -q --no-install-recommends ca-certificates curl git clang lld llvm \
        libclang-dev build-essential pkgconf cmake perl nodejs npm unzip >/dev/null
    export BUN_INSTALL=$CACHE/bun PATH=$CACHE/bun/bin:$PATH
    command -v bun >/dev/null || curl -fsSL https://bun.sh/install | bash >/dev/null
    if [ ! -x "$CARGO_HOME/bin/cargo" ]; then
        curl -sSf https://sh.rustup.rs | sh -s -- -y -q --no-modify-path --profile minimal
    fi
    rustup target add "$TARGET"

    # cc-rs passes its own --target; clang honours the last one.
    mkdir -p "$CACHE/bin"
    cat > "$CACHE/bin/aarch64-cc" <<EOF
#!/bin/sh
exec clang "\$@" --target=aarch64-alpine-linux-musl --sysroot=$SYSROOT -fuse-ld=lld
EOF
    cat > "$CACHE/bin/aarch64-c++" <<EOF
#!/bin/sh
exec clang++ "\$@" --target=aarch64-alpine-linux-musl --sysroot=$SYSROOT -fuse-ld=lld
EOF
    chmod +x "$CACHE/bin/aarch64-cc" "$CACHE/bin/aarch64-c++"

    export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=$CACHE/bin/aarch64-cc
    export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C target-feature=-crt-static"
    export CC_aarch64_unknown_linux_musl=$CACHE/bin/aarch64-cc
    export CXX_aarch64_unknown_linux_musl=$CACHE/bin/aarch64-c++
    export AR_aarch64_unknown_linux_musl=llvm-ar
    export BINDGEN_EXTRA_CLANG_ARGS_aarch64_unknown_linux_musl="--target=aarch64-alpine-linux-musl --sysroot=$SYSROOT"
    export PKG_CONFIG_ALLOW_CROSS=1
    export PKG_CONFIG_SYSROOT_DIR=$SYSROOT
    export PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig
    export CARGO_TARGET_DIR=$CACHE/target-gnu

    cd /work/mcm-src
    cargo run -q --release --package=bindings
    cargo build --release --locked --target "$TARGET" --bin mavlink-camera-manager
    ls -la "$CARGO_TARGET_DIR/$TARGET/release/mavlink-camera-manager"
}

"$1"
