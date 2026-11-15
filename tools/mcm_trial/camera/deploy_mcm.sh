#!/bin/sh
# On the camera: fetch a bundle into RAM and mount it on /run/mcm/root, where
# its loader path points. Nothing is written to flash; a reboot undoes it.
# Usage: deploy_mcm.sh http://HOST:PORT [lean|full]
set -eu
H=$1
NAME=${2:-lean}
D=/run/mcm
grep MemAvailable /proc/meminfo
if mountpoint -q $D/root; then
    umount $D/root
fi
mkdir -p $D/root $D/state
for file in start_mcm.sh measure.sh; do
    wget -q -O $D/$file "$H/camera/$file"
    chmod +x $D/$file
done
wget -q -O $D/mcm.sqfs.part "$H/out/mcm-$NAME.sqfs"
mv $D/mcm.sqfs.part $D/mcm.sqfs
mount -t squashfs -o loop,ro $D/mcm.sqfs $D/root
echo "mounted mcm-$NAME.sqfs ($(wc -c <$D/mcm.sqfs) bytes in /run)"
grep MemAvailable /proc/meminfo
