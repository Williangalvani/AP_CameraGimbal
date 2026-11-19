#!/usr/bin/env python3
"""Run the Hi3516DV500 microSD benchmark with fake dd, df, sysfs and uptime."""
from pathlib import Path
import fcntl
import os
import signal
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
script = repo / 'packaging/hisi/sdbench.sh'
with tempfile.TemporaryDirectory(prefix='hisi-sdbench-') as directory:
    root = Path(directory)
    media = root / 'mnt'
    commands = root / 'commands'
    proc = root / 'proc'
    for path in (media, commands, proc):
        path.mkdir()
    calls = root / 'calls'
    temp = media / '.sdbench.tmp'
    # Each dd creates its output file, then moves the clock to the next tick.
    (commands / 'dd').write_text('''#!/bin/sh
echo "dd $*" >>"$TEST_ROOT/calls"
for arg; do
    case $arg in of=/dev/null) ;; of=*) echo data >"${arg#of=}" ;; esac
done
[ -e "$TEST_ROOT/dd-hangs" ] && sleep 30
[ -e "$TEST_ROOT/dd-fails" ] && { echo "dd: No space left on device" >&2; exit 1; }
sed -n 1p "$TEST_ROOT/ticks" >"$TEST_ROOT/uptime"
sed -i 1d "$TEST_ROOT/ticks"
''')
    # BusyBox puts a long device name on a line of its own.
    (commands / 'df').write_text('''#!/bin/sh
echo "Filesystem           1K-blocks      Used Available Use% Mounted on"
echo "/dev/mmcblk0p1-with-a-long-name"
echo "                      62000000      1000 $(cat "$TEST_ROOT/free")   1% $2"
''')
    (commands / 'sync').write_text('#!/bin/sh\necho sync >>"$TEST_ROOT/calls"\n')
    (commands / 'dmesg').write_text('#!/bin/sh\ncat "$TEST_ROOT/dmesg"\n')
    for path in commands.iterdir():
        path.chmod(0o755)

    sysfs = root / 'sys'
    card = sysfs / 'mmc0/mmc0:aaaa'
    (card / 'block/mmcblk0').mkdir(parents=True)
    (sysfs / 'mmc1/mmc1:0001').mkdir(parents=True)
    (sysfs / 'mmc1/mmc1:0001/type').write_text('SDIO\n')
    for name, value in (('type', 'SD'), ('name', 'SB64G'), ('manfid', '0x000003'),
                        ('oemid', '0x5344'), ('serial', '0x8badf00d'),
                        ('cid', '035344534236344780badf00d0145100'),
                        ('block/mmcblk0/size', '124735488')):
        (card / name).write_text(value + '\n')
    debugfs = root / 'debug'
    (debugfs / 'mmc0').mkdir(parents=True)
    (debugfs / 'mmc0/ios').write_text(
        'clock:\t\t25000000 Hz\nactual clock:\t0 Hz\nvdd:\t\t21 (3.3 ~ 3.4 V)\n'
        'bus mode:\t2 (push-pull)\nbus width:\t2 (4 bits)\ntiming spec:\t2 (sd high-speed)\n'
        'signal voltage:\t0 (3.30 V)\n')
    (root / 'dmesg').write_text(
        '[    1.0] usb 1-1: new device\n'
        '[   12.3] mmc0: Problem setting bus speed mode!\n'
        '[   13.0] mmcblk0: mmc0:aaaa SB64G 59.5 GiB\n'
        '[   14.0] eth0: link up\n')

    state = root / 'sdcard.state'
    lock = root / 'sdcard.lock'
    drop_caches = root / 'drop_caches'
    env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
               TEST_ROOT=str(root), SDCARD_MEDIA=str(media), SDCARD_STATE=str(state),
               SDCARD_LOCK=str(lock), SDBENCH_PROC=str(proc), SDBENCH_SYSFS=str(sysfs),
               SDBENCH_DEBUGFS=str(debugfs), SDBENCH_UPTIME=str(root / 'uptime'),
               SDBENCH_DROP_CACHES=str(drop_caches))
    write = f'dd if=/dev/zero of={temp} bs=1M count=256 conv=fsync'
    read = f'dd if={temp} of=/dev/null bs=1M'

    def prepare(free=1024 * 1024, ticks='120.80 60.00\n133.00 70.00\n'):
        calls.write_text('')
        drop_caches.write_text('')
        (root / 'free').write_text(f'{free}\n')
        (root / 'uptime').write_text('100.00 50.00\n')
        (root / 'ticks').write_text(ticks)

    def run(*args, expect=0, **kwargs):
        prepare(**kwargs)
        result = subprocess.run(['sh', str(script), *args], env=env,
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == expect, (args, result.returncode, result.stdout, result.stderr)
        return result

    def refused(message, *args, expect=1, **kwargs):
        result = run(*args, expect=expect, **kwargs)
        assert message in result.stderr, (message, result.stderr)
        assert result.stdout == '' and calls.read_text() == '', (result.stdout, calls.read_text())

    # Only a ready card is measured.
    refused('card not ready (state: unknown)')
    state.write_text('formatting\n')
    refused('card not ready (state: formatting)')
    state.write_text(f'unformatted {root}/card\n')
    refused('card not ready (state: unformatted')
    state.write_text('ready\n')

    # Sizes are whole MiB from 1 to 4096.
    for size in ('0', '4097', '010', '-1', '1.5', 'abc', '99999999999999999999'):
        refused('usage:', size, expect=2)

    # A held lock (sdcard.sh formatting) is refused, and the file of the
    # sdbench that may be holding it is left alone.
    temp.write_text('another run')
    with open(lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        refused('card busy')
    assert temp.exists()
    temp.unlink()

    # A process with a file open on the card: camera-app recording.
    (proc / '1' / 'fd').mkdir(parents=True)
    (proc / '1/fd/0').symlink_to('/dev/null')
    (proc / '1/fd/1').symlink_to(f'{media}.old/x.mp4')
    (proc / '1/fd/2').symlink_to(media)
    (proc / 'self').mkdir()
    run('1')
    (proc / '612/fd').mkdir(parents=True)
    (proc / '612/fd/9').symlink_to(media / 'DCIM/record/20261119-170000.mp4')
    refused('recording in progress; stop it first')
    (proc / '612/fd/9').unlink()

    # The card needs the file and 64 MiB more.
    refused('not enough space on', free=(256 + 64) * 1024 - 1)
    run(free=(256 + 64) * 1024)
    refused('not enough space on', '4096', free=4096 * 1024)

    # A run: card details without the serial number, then both speeds.
    result = run()
    assert result.stdout.splitlines() == [
        'card: SB64G (SDXC, 59.5 GiB, manfid 0x000003, oemid 0x5344)',
        'bus: sd high-speed, 4 bits, 25.0 MHz',
        'kernel: [   12.3] mmc0: Problem setting bus speed mode!',
        'kernel: [   13.0] mmcblk0: mmc0:aaaa SB64G 59.5 GiB',
        'write: 12.3 MB/s (256 MiB in 20.8 s)',
        'read: 21.0 MB/s (256 MiB in 12.2 s)',
        'ok',
    ], result.stdout
    assert result.stderr == '', result.stderr
    assert '8badf00d' not in result.stdout
    assert calls.read_text().splitlines() == [write, 'sync', read]
    assert drop_caches.read_text() == '3\n'
    assert not temp.exists()

    # Another size; a read that crosses a whole second (".05" is not octal).
    result = run('8', ticks='100.95 0\n101.05 0\n')
    assert result.stdout.splitlines()[-3:-1] == ['write: 8.4 MB/s (8 MiB in 1.0 s)',
                                                   'read: 80.0 MB/s (8 MiB in 0.1 s)'], result.stdout

    # Without the page cache dropped the read is still measured, with a warning.
    env['SDBENCH_DROP_CACHES'] = str(root / 'missing/drop_caches')
    result = run()
    assert 'read speed may be cached' in result.stderr and result.stdout.endswith('ok\n')
    env['SDBENCH_DROP_CACHES'] = str(drop_caches)

    # No card details anywhere: only the speeds.
    env.update(SDBENCH_SYSFS=str(root / 'none'), SDBENCH_DEBUGFS=str(root / 'none'))
    (root / 'dmesg').write_text('')
    result = run()
    assert [line.split(':')[0] for line in result.stdout.splitlines()] == ['write', 'read', 'ok']
    assert result.stderr == ''

    # A failed write is reported and the file removed.
    (root / 'dd-fails').touch()
    result = run(expect=1)
    assert 'write failed: dd: No space left on device' in result.stderr, result.stderr
    assert 'ok' not in result.stdout and not temp.exists()
    assert calls.read_text().splitlines() == [write]
    (root / 'dd-fails').unlink()

    # Stopped mid-write: the file is removed too.
    (root / 'dd-hangs').touch()
    prepare()
    child = subprocess.Popen(['sh', str(script)], env=env, start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 10
    while not temp.exists():
        assert time.monotonic() < deadline, 'dd never started'
        time.sleep(0.05)
    os.killpg(child.pid, signal.SIGTERM)
    assert child.wait(timeout=10) != 0
    assert not temp.exists()
    (root / 'dd-hangs').unlink()

    # With the real /proc: this shell's own files on the card do not count.
    env['SDBENCH_PROC'] = '/proc'
    prepare()
    subprocess.run(['sh', '-c', 'exec sh "$0" 1 >"$1"', str(script), str(media / 'out.log')],
                   env=env, check=True, timeout=120)
    assert (media / 'out.log').read_text().endswith('ok\n')
print('hisi sdbench tests passed')
