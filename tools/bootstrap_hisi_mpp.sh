#!/bin/sh
set -eu

# Hi3519DV500/Hi3516DV500 MPP sample tree (headers, static libs, sample_comm,
# scene_auto, xop RTSP) for the hisi backend. Only the paths the camera
# application builds against are checked out; prebuilt .so files, OpenCV and
# the OP-TEE sources are not needed for the static firmware link.
mpp_root=${1:-build/deps/hi3516dv500-mpp}
mpp_url=https://gitee.com/e-hi3519dv500/Hi3519DV500_SDK_V2.0.2.0_MPP_Sample.git
mpp_commit=f7ac4db68d139de4006f4c32c91c80a94235ba04
required="src/common/sample_comm.h src/scene_auto/include/ot_scene.h
src/rtspserver/src/xop/RtspServer.h lib/hisilicon/libss_mpi.a
lib/hisilicon/libsns_imx678.a lib/3rdparty/libxoprtsp.a"

for command in git mkdir mktemp mv rm sleep; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing dependency tool: $command" >&2
        exit 1
    }
done

check_tree() {
    for path in $required; do
        [ -e "$1/$path" ] || {
            echo "Incomplete Hi3516DV500 MPP tree in $1: missing $path" >&2
            return 1
        }
    done
}

if [ -d "$mpp_root/.git" ]; then
    actual=$(git -C "$mpp_root" rev-parse HEAD)
    [ "$actual" = "$mpp_commit" ] || {
        echo "Unexpected Hi3516DV500 MPP revision in $mpp_root" >&2
        echo "  expected $mpp_commit" >&2
        echo "  found    $actual" >&2
        exit 1
    }
    check_tree "$mpp_root"
    exit 0
elif [ -e "$mpp_root" ]; then
    # A builder-supplied SDK tree (not a pinned checkout) is used as-is.
    check_tree "$mpp_root"
    exit 0
fi

mkdir -p "$(dirname "$mpp_root")"
temporary=$(mktemp -d "$(dirname "$mpp_root")/.hi3516dv500-mpp.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
trap 'exit 1' HUP INT TERM
attempt=1
while :; do
    # Gitee occasionally resets or stalls connections; retry clone and
    # checkout together so a failed attempt never leaves a usable-looking tree.
    if git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=60 \
        clone --filter=blob:none --no-checkout "$mpp_url" "$temporary/checkout" &&
        git -C "$temporary/checkout" config core.sparseCheckout true &&
        printf '%s\n' /LICENSE /README.md /include/hisilicon/ /include/3rdparty/ \
            '!/include/3rdparty/opencv4/' '/lib/hisilicon/*.a' \
            /lib/3rdparty/libxoprtsp.a /src/Makefile.param /src/common/ \
            /src/scene_auto/ /src/rtspserver/ \
            > "$temporary/checkout/.git/info/sparse-checkout" &&
        git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=60 \
            -C "$temporary/checkout" checkout --detach "$mpp_commit"; then
        break
    fi
    rm -rf "$temporary/checkout"
    if [ "$attempt" -ge 3 ]; then
        echo "Failed to fetch pinned Hi3516DV500 MPP sources after $attempt attempts" >&2
        exit 1
    fi
    echo "Hi3516DV500 MPP fetch failed (attempt $attempt/3); retrying" >&2
    sleep 5
    attempt=$((attempt + 1))
done
check_tree "$temporary/checkout"
mv "$temporary/checkout" "$mpp_root"
rm -rf "$temporary"
trap - EXIT HUP INT TERM
echo "Hi3516DV500 MPP ready in $mpp_root"
