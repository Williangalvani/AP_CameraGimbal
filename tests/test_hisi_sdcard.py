#!/usr/bin/env python3
"""Run the Hi3516DV500 microSD helper against an image file with fake mount tools."""
from pathlib import Path
import fcntl
import os
import shutil
import struct
import subprocess
import tempfile
import sys
import time

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hisi-sdcard-') as directory:
    root = Path(directory)
    media = root / 'mnt'
    commands = root / 'commands'
    media.mkdir()
    commands.mkdir()
    disk = root / 'card'
    partition = root / 'card1'
    mounts = root / 'mounts'
    mounts.write_text('')
    calls = root / 'calls'
    # Only "formatted" devices mount; TEST_RO mounts them read-only.
    (commands / 'mount').write_text('''#!/bin/sh
echo "mount $*" >>"$TEST_ROOT/calls"
[ "$1" = -o ] && shift 2
grep -qx "$1" "$TEST_ROOT/formatted" 2>/dev/null || exit 32
mode=rw
[ -e "$TEST_ROOT/ro" ] && mode=ro
echo "$1 $2 vfat $mode,noatime 0 0" >>"$TEST_ROOT/mounts"
''')
    (commands / 'umount').write_text('''#!/bin/sh
echo "umount $*" >>"$TEST_ROOT/calls"
[ -e "$TEST_ROOT/umount-fails" ] && exit 1
[ "$1" = -l ] && shift
awk -v dir="$1" '$2 != dir' "$TEST_ROOT/mounts" >"$TEST_ROOT/mounts.new"
mv "$TEST_ROOT/mounts.new" "$TEST_ROOT/mounts"
''')
    # Keeps the state published while it runs, which the web watches.
    (commands / 'mkfs.vfat').write_text('''#!/bin/sh
echo "mkfs.vfat $*" >>"$TEST_ROOT/calls"
cp "$TEST_ROOT/sdcard.state" "$TEST_ROOT/mkfs.state"
[ -e "$TEST_ROOT/mkfs-fails" ] && exit 1
for device; do :; done
touch "$device"
echo "$device" >>"$TEST_ROOT/formatted"
''')
    # The board tool's output, over a register file; a second argument writes.
    (commands / 'bspmm').write_text('''#!/bin/sh
echo "*** Board tools : ver0.0.1_20121120 *** "
old=$(cat "$TEST_ROOT/clock")
if [ $# -eq 2 ]; then
    echo "bspmm $*" >>"$TEST_ROOT/calls"
    echo "$2" >"$TEST_ROOT/clock"
    echo "$1: $old --> $2 "
else
    echo "$1: $old"
    echo "NewValue:scanf_s failed!"
fi
echo "[END]"
''')
    for path in commands.iterdir():
        path.chmod(0o755)
    state = root / 'sdcard.state'
    lock = root / 'sdcard.lock'
    uptime = root / 'uptime'
    # The web's Status page compares this with the one it loaded with.
    result = root / 'sdcard.state.format'
    env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
               TEST_ROOT=str(root), SDCARD_MEDIA=str(media), SDCARD_STATE=str(state),
               SDCARD_LOCK=str(lock), SDCARD_DISKS=str(disk), SDCARD_MOUNTS=str(mounts),
               SDCARD_UPTIME=str(uptime))
    # The shells camera-app may find on the board, where the host has them.
    shell = os.environ.get('SDCARD_TEST_SHELL', 'sh').split()

    def run(command, expect=0, at=None):
        calls.write_text('')
        if at is not None:
            uptime.write_text(f'{at} 1.00\n')
        result = subprocess.run([*shell, str(repo / 'packaging/hisi/sdcard.sh'), command],
                                env=env, capture_output=True, text=True, timeout=60)
        assert result.returncode == expect, (command, result.returncode, result.stderr)
        return calls.read_text().splitlines()

    def current():
        return state.read_text().split()[0]

    def formatted():
        assert not Path(f'{result}.new').exists()
        return result.read_text()

    # No card.
    assert run('mount') == []
    assert current() == 'absent'
    assert not (media / 'DCIM').exists()

    # A card the kernel cannot mount (here: an old DVR filesystem) is tried
    # once, and again only after it is reinserted.
    sectors = 16 * 1024 * 1024 // 512 * 64
    with open(disk, 'wb') as card:
        card.write(b'WFS0.2'.ljust(512, b'\0'))
        card.truncate(sectors * 512)
    assert run('mount') == [f'mount -o noatime {disk} {media}']
    assert state.read_text() == f'unformatted {disk}\n'
    assert run('mount') == []
    assert current() == 'unformatted'

    # A held lock (a format running) leaves the card alone.
    with open(lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        assert run('mount') == []

    # A failed format is reported and leaves the card unformatted.
    assert not result.exists()
    (root / 'mkfs-fails').touch()
    assert run('format', expect=1, at='100.25')[-1] == f'mkfs.vfat -n APCAM {partition}'
    assert (root / 'mkfs.state').read_text() == 'formatting\n'
    assert current() == 'unformatted'
    assert formatted() == 'failed mkfs 100.25\n'
    (root / 'mkfs-fails').unlink()

    # A format waiting for the lock (a mount stuck in I/O, or a benchmark)
    # reports nothing until it has run.
    uptime.write_text('105.50 1.00\n')
    with open(lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        waiting = subprocess.Popen([*shell, str(repo / 'packaging/hisi/sdcard.sh'), 'format'],
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        assert waiting.poll() is None and formatted() == 'failed mkfs 100.25\n'
        assert current() == 'unformatted'
    assert waiting.wait(timeout=60) == 0
    assert formatted() == 'ok 105.50\n' and current() == 'ready'

    # Format: one FAT32 LBA partition from 4 MiB to the end, then mounted.
    (root / 'mkfs.state').unlink()
    mounts.write_text('')
    assert run('format', at='110.00') == [f'mkfs.vfat -n APCAM {partition}',
                                          f'mount -o noatime {partition} {media}']
    assert (root / 'mkfs.state').read_text() == 'formatting\n'
    assert current() == 'ready'
    assert formatted() == 'ok 110.00\n'
    with open(disk, 'rb') as card:
        mbr = card.read(1024 * 1024)
    assert disk.stat().st_size == sectors * 512
    assert mbr[:446] == bytes(446) and mbr[512:] == bytes(len(mbr) - 512)
    status, kind, start, size = struct.unpack('<B3xB3xII', mbr[446:462])
    assert (status, kind, start, size) == (0, 0x0c, 8192, sectors - 8192)
    assert mbr[462:510] == bytes(48) and mbr[510:512] == b'\x55\xaa'
    for folder in ('DCIM/record', 'DCIM/capture', 'logs'):
        assert (media / folder).is_dir(), folder

    # Mounted: nothing to do.
    assert run('mount') == []
    assert current() == 'ready'

    # Formatting a mounted card unmounts it first.
    assert run('format', at='120.00')[:2] == [f'umount {media}', f'mkfs.vfat -n APCAM {partition}']
    assert current() == 'ready' and formatted() == 'ok 120.00\n'

    # One it cannot unmount stays mounted, and ready, but the format failed.
    (root / 'umount-fails').touch()
    assert run('format', expect=1, at='125.00') == [f'umount {media}', f'umount -l {media}']
    assert current() == 'ready' and formatted() == 'failed unmount 125.00\n'
    (root / 'umount-fails').unlink()

    # Removed: detached lazily, since a recording may still be open on it.
    disk.unlink()
    partition.unlink()
    assert run('mount') == [f'umount -l {media}']
    assert current() == 'absent' and mounts.read_text() == ''
    assert run('format', expect=1, at='130.00') == []
    assert current() == 'absent' and formatted() == 'failed absent 130.00\n'

    # A card that only mounts read-only is not used.
    with open(disk, 'wb') as card:
        card.truncate(sectors * 512)
    partition.touch()
    (root / 'ro').touch()
    assert run('mount') == [f'mount -o noatime {partition} {media}', f'umount {media}',
                            f'mount -o noatime {disk} {media}']
    assert current() == 'unformatted' and mounts.read_text() == ''
    (root / 'ro').unlink()

    # Too small for FAT32.
    with open(disk, 'wb') as card:
        card.truncate(32 * 1024 * 1024)
    assert run('format', expect=1, at='140.00') == []
    assert current() == 'unformatted' and formatted() == 'failed small 140.00\n'

    # A card that cannot be read (here a directory): the controller is bound
    # to its driver again, twice at most, then the card is reported.
    disk.unlink()
    partition.unlink()
    assert run('mount') == [] and current() == 'absent'
    disk.mkdir()
    driver = root / 'driver'
    driver.mkdir()
    resets = root / 'sdcard.state.resets'
    env.update(SDCARD_HOST='10030000.SDIO', SDCARD_HOST_DRIVER=str(driver))
    for count in (1, 2):
        (driver / 'unbind').unlink(missing_ok=True)
        (driver / 'bind').unlink(missing_ok=True)
        assert run('mount') == [f'mount -o noatime {disk} {media}']
        assert current() == 'absent' and resets.read_text() == f'{count}\n'
        assert (driver / 'unbind').read_text() == (driver / 'bind').read_text() == '10030000.SDIO\n'
    assert run('mount') == [f'mount -o noatime {disk} {media}']
    assert state.read_text() == f'error {disk}\n'
    assert run('mount') == []
    # A card that mounts allows resets again.
    disk.rmdir()
    assert run('mount') == [] and current() == 'absent'
    with open(disk, 'wb') as card:
        card.truncate(sectors * 512)
    partition.touch()
    run('mount')
    assert current() == 'ready' and not resets.exists()

    # The SDIO clock: a card left at 400 kHz (select 0 in bits 24-26) gets
    # 25 MHz (select 1), other bits kept; a removed card gives 400 kHz back.
    clock = root / 'clock'
    clock.write_text('0x80000003\n')
    env.update(SDCARD_CLOCK_REG='0x110135c0')
    assert run('mount') == ['bspmm 0x110135c0 0x81000003']
    assert clock.read_text() == '0x81000003\n'
    assert run('mount') == []
    disk.unlink()
    partition.unlink()
    assert run('mount') == ['bspmm 0x110135c0 0x80000003', f'umount -l {media}']
    assert current() == 'absent' and clock.read_text() == '0x80000003\n'
    assert run('mount') == []

    # A clock the kernel did raise, or one changed since, is left alone.
    with open(disk, 'wb') as card:
        card.truncate(sectors * 512)
    partition.touch()
    clock.write_text('0x03000003\n')
    run('mount')
    assert current() == 'ready' and clock.read_text() == '0x03000003\n'
    clock.write_text('0x00000003\n')
    run('mount')
    clock.write_text('0x02000003\n')
    disk.unlink()
    partition.unlink()
    assert run('mount') == [f'umount -l {media}']
    assert clock.read_text() == '0x02000003\n'

    # Binding the controller again starts from 400 kHz too.
    clock.write_text('0x00000003\n')
    disk.mkdir()
    (driver / 'unbind').unlink()
    assert run('mount') == ['bspmm 0x110135c0 0x01000003', f'mount -o noatime {disk} {media}',
                            'bspmm 0x110135c0 0x00000003']
    assert current() == 'absent' and (driver / 'unbind').exists()
    disk.rmdir()
    run('mount')
print(f"hisi sdcard tests passed ({' '.join(shell)})")
# The board runs BusyBox ash; the host's sh may be bash.
if 'SDCARD_TEST_SHELL' not in os.environ:
    for other in ('dash', 'busybox sh'):
        if shutil.which(other.split()[0]):
            subprocess.run([sys.executable, __file__], env=dict(os.environ, SDCARD_TEST_SHELL=other),
                           check=True)
        else:
            print(f'hisi sdcard tests skipped under {other}: not installed')
