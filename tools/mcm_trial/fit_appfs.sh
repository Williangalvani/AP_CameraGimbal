#!/bin/sh
# Test-only appfs image: an existing appfs image's files plus an MCM bundle
# in ap_cameragimbal/mcm, built and checked with hisi_firmware.py the way
# `make hisi_appfs` and hisi-web do. For sizing; not meant to be flashed.
# DEVICE_CODE (updatetype.ini DEV/SDK/PLATFORM, default 19/2/51) picks the
# appfs layout: a UBI volume in mtd4 (19/2/51) or a raw squashfs in mtd5
# (19/2/35).
# Runs in a debian:trixie container with mcm_trial on /work and a
# hisi_firmware.py with LAYOUTS (4kcam-sdcard) in /work/tools.
# Usage: fit_appfs.sh APPFS_IMAGE BUNDLE_DIR OUTPUT_IMAGE [DEVICE_CODE]
set -eu
IMG=$1
BUNDLE=$2
OUTPUT=$3
DEVICE_CODE=${4:-19/2/51}
apt-get update -q >/dev/null
apt-get install -y -q --no-install-recommends python3 squashfs-tools >/dev/null
T=$(mktemp -d)
python3 - "$IMG" "$T/appfs.sqfs" "$DEVICE_CODE" <<'EOF'
import struct
import sys
sys.path.insert(0, '/work/tools')
import hisi_firmware as f
code = f.parse_device_code(sys.argv[3])
layout = f.layout_for(code)
image = f.check_appfs_image(open(sys.argv[1], 'rb').read(), device_code=code)
if layout.ubi:
    pebs = len(image) // f.PEB_SIZE
    volume = b''.join(image[peb * f.PEB_SIZE + f.DATA_OFFSET:(peb + 1) * f.PEB_SIZE]
                      for peb in range(2, pebs))
    print(f'existing appfs: {pebs - 2} of {f.APPFS_VOLUME_MAX_PEBS} LEBs')
else:
    used, = struct.unpack_from('<Q', image, 40)
    volume = image[:used]
    print(f'existing appfs: squashfs {used} bytes ({used / 1e6:.1f} MB) in {layout.mtd}')
open(sys.argv[2], 'wb').write(volume)
EOF
unsquashfs -q -n -d "$T/staging" "$T/appfs.sqfs"
cp -a "$BUNDLE" "$T/staging/ap_cameragimbal/mcm"
python3 - "$T/staging" "$OUTPUT" "$DEVICE_CODE" <<'EOF'
import struct
import sys
from pathlib import Path
sys.path.insert(0, '/work/tools')
import hisi_firmware as f
code = f.parse_device_code(sys.argv[3])
layout = f.layout_for(code)
f.build_appfs(Path(sys.argv[1]), Path(sys.argv[2]), device_code=code)
image = f.check_appfs_image(Path(sys.argv[2]).read_bytes(), device_code=code)
if layout.ubi:
    lebs = len(image) // f.PEB_SIZE - 2
    free = (f.APPFS_VOLUME_MAX_PEBS - lebs) * f.LEB_SIZE
    print(f'with MCM: {lebs} of {f.APPFS_VOLUME_MAX_PEBS} LEBs, '
          f'{free} bytes ({free / 1e6:.1f} MB) left; hisi-web check passes')
else:
    used, = struct.unpack_from('<Q', image, 40)
    size = layout.pebs * f.PEB_SIZE
    free = size - used
    print(f'with MCM: squashfs {used} bytes ({used / 1e6:.1f} MB) of {size}-byte '
          f'{layout.mtd}, {free} bytes ({free / 1e6:.1f} MB) left; hisi-web check passes')
EOF
