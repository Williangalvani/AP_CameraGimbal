#!/bin/sh
# Test-only appfs image: an existing appfs image's files plus an MCM bundle
# in ap_cameragimbal/mcm, built and checked with hisi_firmware.py the way
# `make hisi_appfs` and hisi-web do. For sizing; not meant to be flashed.
# Runs in a debian:trixie container with mcm_trial on /work and
# hisi_firmware.py in /work/tools.
# Usage: fit_appfs.sh APPFS_IMAGE BUNDLE_DIR OUTPUT_IMAGE
set -eu
IMG=$1
BUNDLE=$2
OUTPUT=$3
apt-get update -q >/dev/null
apt-get install -y -q --no-install-recommends python3 squashfs-tools >/dev/null
T=$(mktemp -d)
python3 - "$IMG" "$T/appfs.sqfs" <<'EOF'
import sys
sys.path.insert(0, '/work/tools')
import hisi_firmware as f
image = f.check_appfs_image(open(sys.argv[1], 'rb').read())
pebs = len(image) // f.PEB_SIZE
volume = b''.join(image[peb * f.PEB_SIZE + f.DATA_OFFSET:(peb + 1) * f.PEB_SIZE]
                  for peb in range(2, pebs))
open(sys.argv[2], 'wb').write(volume)
print(f'existing appfs: {pebs - 2} of {f.APPFS_VOLUME_MAX_PEBS} LEBs')
EOF
unsquashfs -q -n -d "$T/staging" "$T/appfs.sqfs"
cp -a "$BUNDLE" "$T/staging/ap_cameragimbal/mcm"
python3 - "$T/staging" "$OUTPUT" <<'EOF'
import sys
from pathlib import Path
sys.path.insert(0, '/work/tools')
import hisi_firmware as f
f.build_appfs(Path(sys.argv[1]), Path(sys.argv[2]))
image = f.check_appfs_image(Path(sys.argv[2]).read_bytes())
lebs = len(image) // f.PEB_SIZE - 2
free = (f.APPFS_VOLUME_MAX_PEBS - lebs) * f.LEB_SIZE
print(f'with MCM: {lebs} of {f.APPFS_VOLUME_MAX_PEBS} LEBs, '
      f'{free} bytes ({free / 1e6:.1f} MB) left; hisi-web check passes')
EOF
