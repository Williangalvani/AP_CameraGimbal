#!/bin/sh
set -eu

# MPP userland matching the camera's kernel drivers (HI3519DV500_MPP B040).
# The V2.0.2.0 sample tree (bootstrap_hisi_mpp.sh) is B050, whose ioctl
# structs differ from the factory ot_*.ko, so the hisi backend compiles
# against these headers and loads these libraries instead. Nothing here is
# committed; the camera keeps its own kernel modules.
#   openhisilicon: the SDK V1.0.1.1 (B040) public headers.
#   OpenIPC firmware: the SDK V1.0.1.1 (B040) MPP shared libraries (musl).
#   open-source-toolkit/c0e5d: an IMX678 sensor driver for this SDK.
root=${1:-build/deps/hi3519dv500-b040}
ohs_url=https://github.com/OpenIPC/openhisilicon.git
ohs_commit=c3076b23dd3fd48d10772dbbb58399615db2320a
ohs_paths="/LICENSE /kernel/include/hi3519dv500/ /libraries/isp/include/hi3519dv500/"
oipc_url=https://github.com/OpenIPC/firmware.git
oipc_commit=ff44f281f063fb7df0f046fba650983ba9cde4cc
oipc_lib=general/package/hisilicon-osdrv-hi3519dv500/files/lib
sns_url=https://gitcode.com/open-source-toolkit/c0e5d.git
sns_commit=8afc05c37659fbdc7b62f2374e5c55be5c8e98d6
sns_zip=sony_imx678_19DV500.zip
sns_sha256=aac3e703eb58b4e198731fa2b2edbfa92f619929834487a63ef221d4d54848f4
required="openhisilicon/kernel/include/hi3519dv500/ss_mpi_venc.h
openhisilicon/libraries/isp/include/hi3519dv500/ot_mpi_isp.h
openipc/$oipc_lib/libss_mpi.so openipc/$oipc_lib/libot_mpi_isp.so
imx678/imx678_cmos.c imx678/imx678_sensor_ctl.c"

for command in git mkdir mktemp mv rm sleep sha256sum unzip; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing dependency tool: $command" >&2
        exit 1
    }
done

check_tree() {
    for path in $required; do
        [ -e "$1/$path" ] || {
            echo "Incomplete Hi3519DV500 B040 tree in $1: missing $path" >&2
            return 1
        }
    done
}

stamp="$ohs_commit $oipc_commit $sns_commit"
if [ -e "$root/.apcam-pins" ]; then
    [ "$(cat "$root/.apcam-pins")" = "$stamp" ] || {
        echo "Unexpected Hi3519DV500 B040 revisions in $root; remove it to refetch" >&2
        exit 1
    }
    check_tree "$root"
    exit 0
elif [ -e "$root" ]; then
    # A builder-supplied tree (not a pinned fetch) is used as-is.
    check_tree "$root"
    exit 0
fi

# fetch <url> <commit> <dir> <sparse paths...>; retried because the hosts
# occasionally reset or stall connections.
fetch() {
    url=$1 commit=$2 dir=$3
    shift 3
    attempt=1
    while :; do
        if git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=60 \
            clone --filter=blob:none --no-checkout "$url" "$dir" &&
            git -C "$dir" config core.sparseCheckout true &&
            printf '%s\n' "$@" > "$dir/.git/info/sparse-checkout" &&
            git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=60 \
                -C "$dir" checkout --detach "$commit"; then
            return 0
        fi
        rm -rf "$dir"
        if [ "$attempt" -ge 3 ]; then
            echo "Failed to fetch $url after $attempt attempts" >&2
            return 1
        fi
        echo "Fetch of $url failed (attempt $attempt/3); retrying" >&2
        sleep 5
        attempt=$((attempt + 1))
    done
}

mkdir -p "$(dirname "$root")"
temporary=$(mktemp -d "$(dirname "$root")/.hi3519dv500-b040.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
trap 'exit 1' HUP INT TERM
fetch "$ohs_url" "$ohs_commit" "$temporary/tree/openhisilicon" $ohs_paths
fetch "$oipc_url" "$oipc_commit" "$temporary/tree/openipc" /LICENSE "/$oipc_lib/"
fetch "$sns_url" "$sns_commit" "$temporary/sensor" "/$sns_zip" /LICENSE
echo "$sns_sha256  $temporary/sensor/$sns_zip" | sha256sum -c - >/dev/null || {
    echo "IMX678 driver archive SHA-256 mismatch" >&2
    exit 1
}
unzip -q "$temporary/sensor/$sns_zip" -d "$temporary/sensor/unpacked"
mv "$temporary/sensor/unpacked/sony_imx678_19DV500" "$temporary/tree/imx678"
check_tree "$temporary/tree"
printf '%s\n' "$stamp" > "$temporary/tree/.apcam-pins"
mv "$temporary/tree" "$root"
rm -rf "$temporary"
trap - EXIT HUP INT TERM
echo "Hi3519DV500 B040 MPP ready in $root"
