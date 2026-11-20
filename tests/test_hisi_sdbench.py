#!/usr/bin/env python3
"""Run the Hi3516DV500 microSD benchmark with fake dd, df, sysfs and uptime."""
from pathlib import Path
import errno
import fcntl
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

repo = Path(__file__).resolve().parents[1]
script = repo / 'packaging/hisi/sdbench.sh'
# The board runs BusyBox ash; the host's sh may be bash.
shell = os.environ.get('SDBENCH_TEST_SHELL', 'sh').split()
real_timeout = shutil.which('timeout')
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
    # Its output is BusyBox's: an error first, then the record counts.
    (commands / 'dd').write_text('''#!/bin/sh
echo "dd $*" >>"$TEST_ROOT/calls"
count=1
for arg; do
    case $arg in of=/dev/null) ;; of=*) out=${arg#of=}; echo data >"$out" ;; count=*) count=${arg#count=} ;; esac
done
[ -e "$TEST_ROOT/dd-hangs" ] && exec sleep 30
stats()
{
    echo "$1+0 records in" >&2
    echo "$2+0 records out" >&2
    echo "$(($2 * 1048576)) bytes ($2.0MB) copied, 8.612345 seconds, 14.9MB/s" >&2
}
if [ -n "${out-}" ] && [ -e "$TEST_ROOT/dd-fails" ]; then
    echo "dd: error writing '$out': No space left on device" >&2
    stats "$count" $((count - 1))
    exit 1
fi
if [ -z "${out-}" ] && [ -e "$TEST_ROOT/dd-read-fails" ]; then
    echo "dd: error reading '${1#if=}': Input/output error" >&2
    stats 3 3
    exit 1
fi
stats "$count" "$count"
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
    # timeout(1) as BusyBox before 1.30 spells it, and one that cannot be
    # used, which leaves the script's own watchdog.
    old_timeout = root / 'old-timeout'
    no_timeout = root / 'no-timeout'
    for path in (old_timeout, no_timeout):
        path.mkdir()
    (old_timeout / 'timeout').write_text(f'''#!/bin/sh
[ "$1" = -t ] || {{ echo "timeout: can't execute '$1'" >&2; exit 127; }}
echo "timeout $*" >>"$TEST_ROOT/timeout.log"
seconds=$2
shift 2
exec {real_timeout} "$seconds" "$@"
''')
    (no_timeout / 'timeout').write_text('#!/bin/sh\nexit 127\n')
    for path in (*commands.iterdir(), old_timeout / 'timeout', no_timeout / 'timeout'):
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
    bench_lock = root / 'sdbench.lock'
    drop_caches = root / 'drop_caches'
    path = str(commands) + ':' + os.environ['PATH']
    env = dict(os.environ, PATH=path,
               TEST_ROOT=str(root), SDCARD_MEDIA=str(media), SDCARD_STATE=str(state),
               SDCARD_LOCK=str(lock), SDBENCH_LOCK=str(bench_lock), SDBENCH_PROC=str(proc),
               SDBENCH_SYSFS=str(sysfs), SDBENCH_DEBUGFS=str(debugfs),
               SDBENCH_UPTIME=str(root / 'uptime'), SDBENCH_DROP_CACHES=str(drop_caches))
    env.pop('SDBENCH_LOCK_INHERITED', None)
    env.pop('SDBENCH_TIMEOUT', None)
    write = f'dd if=/dev/zero of={temp} bs=1M count=256 conv=fsync'
    read = f'dd if={temp} of=/dev/null bs=1M'

    def prepare(free=1024 * 1024, ticks='120.80 60.00\n133.00 70.00\n'):
        calls.write_text('')
        drop_caches.write_text('')
        (root / 'free').write_text(f'{free}\n')
        (root / 'uptime').write_text('100.00 50.00\n')
        (root / 'ticks').write_text(ticks)

    def start(*args, **kwargs):
        prepare(**kwargs)
        return subprocess.Popen([*shell, str(script), *args], env=env, start_new_session=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def run(*args, expect=0, **kwargs):
        prepare(**kwargs)
        result = subprocess.run([*shell, str(script), *args], env=env,
                                capture_output=True, text=True, timeout=60)
        assert result.returncode == expect, (args, result.returncode, result.stdout, result.stderr)
        return result

    def refused(message, *args, expect, **kwargs):
        result = run(*args, expect=expect, **kwargs)
        assert message in result.stderr, (message, result.stderr)
        assert result.stderr.startswith('sdbench: ') and result.stderr.count('\n') == 1, result.stderr
        assert result.stdout == '' and calls.read_text() == '', (result.stdout, calls.read_text())
        return result

    def locked(path):
        """Whether something holds path's lock, probed as camera-app and the web do."""
        with open(path) as probe:
            try:
                fcntl.flock(probe, fcntl.LOCK_SH | fcntl.LOCK_NB)
            except OSError as error:
                assert error.errno == errno.EWOULDBLOCK, error
                return True
            return False

    # Only a ready card is measured.
    refused('card not ready (state: unknown)', expect=3)
    state.write_text('formatting\n')
    refused('card not ready (state: formatting)', expect=3)
    state.write_text(f'unformatted {root}/card\n')
    refused('card not ready (state: unformatted', expect=3)
    state.write_text('ready\n')

    # Sizes are whole MiB from 1 to 4096.
    for size in ('0', '4097', '010', '-1', '1.5', 'abc', '99999999999999999999'):
        result = run(size, expect=2)
        assert 'usage:' in result.stderr and calls.read_text() == ''

    # A held lock (sdcard.sh formatting) is refused, and the file of the
    # sdbench that may be holding it is left alone.
    temp.write_text('another run')
    with open(lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        refused('card busy (sdcard.sh is formatting or mounting it)', expect=4)
    assert temp.exists()
    # So is another benchmark: one at a time, from the web or a shell.
    with open(bench_lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        refused('card busy (another benchmark is running)', expect=4)
        assert temp.exists()
    temp.unlink()

    # A lock that frees up within the second try is taken, and the card's
    # state read again: here a format took it meanwhile.
    with open(lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        child = start()
        time.sleep(0.3)
        state.write_text('formatting\n')
    stdout, stderr = child.communicate(timeout=30)
    assert child.returncode == 3 and stderr == 'sdbench: card not ready (state: formatting)\n', \
        (child.returncode, stderr)
    assert stdout == '' and calls.read_text() == ''
    state.write_text('ready\n')

    # A process with a file open on the card: camera-app recording, a BIN
    # log, a download. A stale file from a run that lost power goes anyway.
    (proc / '1' / 'fd').mkdir(parents=True)
    (proc / '1/fd/0').symlink_to('/dev/null')
    (proc / '1/fd/1').symlink_to(f'{media}.old/x.mp4')
    (proc / '1/fd/2').symlink_to(media)
    (proc / 'self').mkdir()
    run('1')
    (proc / '612/fd').mkdir(parents=True)
    (proc / '612/fd/9').symlink_to(media / 'DCIM/record/20261119-170000.mp4')
    temp.write_text('left by a run that lost power')
    refused('a file on the card is open (recording, logging or a download): pid 612', expect=5)
    assert not temp.exists()
    (proc / '612/comm').write_text('camera-app\n')
    refused('a file on the card is open (recording, logging or a download): camera-app, pid 612',
            expect=5)
    (proc / '612/fd/9').unlink()

    # The card needs the file and 64 MiB more.
    refused('not enough space on', free=(256 + 64) * 1024 - 1, expect=6)
    run(free=(256 + 64) * 1024)
    refused('not enough space on', '4096', free=4096 * 1024, expect=6)

    # A run: card details without the serial number, then both speeds. dd's
    # record counts stay out of the report.
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
    # The page cache only: dentries and inodes of a live camera stay.
    assert drop_caches.read_text() == '1\n'
    assert not temp.exists()
    assert not locked(bench_lock) and not locked(lock)

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

    # A failed write is reported by dd's error, not its record counts, and
    # the file removed.
    (root / 'dd-fails').touch()
    result = run(expect=1)
    assert result.stderr == f"sdbench: write failed: dd: error writing '{temp}': No space left on device\n", \
        result.stderr
    assert 'ok' not in result.stdout and not temp.exists()
    assert calls.read_text().splitlines() == [write]
    (root / 'dd-fails').unlink()
    (root / 'dd-read-fails').touch()
    result = run(expect=1)
    assert result.stderr == f"sdbench: read failed: dd: error reading '{temp}': Input/output error\n", \
        result.stderr
    assert result.stdout.splitlines()[-1].startswith('write: ') and not temp.exists()
    (root / 'dd-read-fails').unlink()

    # While it runs it holds both locks: camera-app does not start recording
    # or a BIN log, and hisi-web and MAVLink do not format. A second one is
    # refused. Stopped mid-write: the file is removed too, and the locks freed.
    (root / 'dd-hangs').touch()
    child = start()
    deadline = time.monotonic() + 10
    while not temp.exists():
        assert time.monotonic() < deadline, 'dd never started'
        time.sleep(0.05)
    assert locked(bench_lock) and locked(lock)
    second = subprocess.run([*shell, str(script)], env=env, capture_output=True, text=True, timeout=30)
    assert second.returncode == 4 and second.stderr == 'sdbench: card busy (another benchmark is running)\n', \
        (second.returncode, second.stderr)
    os.killpg(child.pid, signal.SIGTERM)
    assert child.wait(timeout=10) != 0
    child.communicate()
    assert not temp.exists()
    deadline = time.monotonic() + 10
    while locked(bench_lock) or locked(lock):
        assert time.monotonic() < deadline, 'locks still held after the benchmark stopped'
        time.sleep(0.05)

    # A caller holding the benchmark lock (hisi-web) passes it on.
    (root / 'dd-hangs').unlink()
    with open(bench_lock, 'w') as held:
        fcntl.flock(held, fcntl.LOCK_EX)
        env['SDBENCH_LOCK_INHERITED'] = '1'
        result = run()
        assert result.stdout.endswith('ok\n') and calls.read_text().splitlines() == [write, 'sync', read]
        del env['SDBENCH_LOCK_INHERITED']
        refused('card busy (another benchmark is running)', expect=4)

    # A dd that hangs is stopped after SDBENCH_TIMEOUT seconds, by timeout(1)
    # in either form or by the watchdog, and the file and locks are freed.
    env['SDBENCH_TIMEOUT'] = '1'
    timeout_log = root / 'timeout.log'
    for mode, extra in (('timeout', None), ('timeout -t', old_timeout), ('watchdog', no_timeout)):
        if mode == 'timeout' and real_timeout is None:
            print('hisi sdbench: no timeout(1) on this host; its form not tested')
            continue
        env['PATH'] = (f'{extra}:' if extra else '') + path
        timeout_log.write_text('')
        result = run()
        assert result.stdout.endswith('ok\n'), (mode, result.stdout, result.stderr)
        if mode == 'timeout -t':
            assert [line for line in timeout_log.read_text().splitlines() if ' dd ' in line] == \
                [f'timeout -t 1 {write}', f'timeout -t 1 {read}'], timeout_log.read_text()
        (root / 'dd-hangs').touch()
        began = time.monotonic()
        result = run(expect=1)
        assert result.stderr == 'sdbench: write timed out after 1 s\n', (mode, result.stderr)
        assert time.monotonic() - began < 10, mode
        assert not temp.exists() and not locked(bench_lock) and not locked(lock), mode
        (root / 'dd-hangs').unlink()
    env['PATH'] = path
    env['SDBENCH_TIMEOUT'] = 'soon'
    assert run().stdout.endswith('ok\n')
    del env['SDBENCH_TIMEOUT']

    # With the real /proc: this shell's own files on the card do not count.
    env['SDBENCH_PROC'] = '/proc'
    prepare()
    subprocess.run(['sh', '-c', 'exec "$@" 1 >"$0"', str(media / 'out.log'), *shell, str(script)],
                   env=env, check=True, timeout=120)
    assert (media / 'out.log').read_text().endswith('ok\n')
print(f"hisi sdbench tests passed ({' '.join(shell)})")
if 'SDBENCH_TEST_SHELL' not in os.environ:
    for other in ('dash', 'busybox sh'):
        if shutil.which(other.split()[0]):
            subprocess.run([sys.executable, __file__], env=dict(os.environ, SDBENCH_TEST_SHELL=other),
                           check=True)
        else:
            print(f'hisi sdbench tests skipped under {other}: not installed')
