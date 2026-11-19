#!/usr/bin/env python3
"""Run the actual Hi3516DV500 supervisor and web UI with fake drivers and camera."""
from pathlib import Path
import base64
import fcntl
import html
import http.client
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hisi-service-') as directory:
    root = Path(directory)
    app = root / 'appfs/ap_cameragimbal'
    config = root / 'date/ap_cameragimbal'
    runtime = root / 'run'
    media = root / 'mnt'
    komod = root / 'komod'
    commands = root / 'commands'
    for path in (app / 'bin', root / 'date', runtime, media, komod / 'extdrv', commands):
        path.mkdir(parents=True, exist_ok=True)
    (app / 'camera.ini.default').write_bytes((repo / 'packaging/hisi/camera.ini').read_bytes())
    (app / 'web.pass.default').write_text('test-password\n')
    shutil.copytree(repo / 'web/webroot', app / 'webroot')
    # The camera's U-Boot bootargs.
    (root / 'cmdline').write_text(
        'mem=384M console=ttyAMA0,115200 clk_ignore_unused root=/dev/ubiblock0_0 ubi.mtd=3 '
        'ubi.block=0,0 rootfstype=squashfs mtdparts=nand:1M(boot),128K(bl31),15M(kernel),'
        '16M(rootfs),36M(appfs),12M(fconfigs),-(date)\n')
    (root / 'modules').write_text('')
    mounts = root / 'mounts'
    mounts.write_text('')
    insmod_log = root / 'insmod.log'
    insmod_fail = root / 'insmod.fail'
    insmod_fail.write_text('ot_vi\n')

    source = root / 'fake.c'
    # camera-app takes an exclusive "MPP" lock; two owners is a test failure.
    source.write_text(r'''
#include <sys/file.h>
#include <apcam/config_status.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Loses the first ignore_terms SIGTERMs, as camera-app does after a live media
 * reconfigure while the vendor ISP thread has SIGTERM ignored. */
static int ignore_terms;
static volatile sig_atomic_t terms;
static void on_term(int signal_number) {
    (void)signal_number;
    if (++terms <= ignore_terms) return;
    char text[64];
    int length = snprintf(text, sizeof(text), "fake camera-app %d stopped by SIGTERM %d\n",
                          (int)getpid(), (int)terms);
    if (write(STDOUT_FILENO, text, (size_t)length) < 0) _exit(1);
    _exit(0);
}
static int acknowledge(uint64_t hash) {
    char path[4096];
    snprintf(path, sizeof(path), "%s.config", getenv("CAMERA_APP_READY_PATH"));
    FILE *status = fopen(path, "w");
    if (!status) return -1;
    fprintf(status, "%016llx %ld applied\nConfiguration applied.\n",
            (unsigned long long)hash, (long)getpid());
    return fclose(status);
}
int main(void) {
    if (access(getenv("TEST_IGNORE_TERM"), F_OK) == 0) signal(SIGTERM, SIG_IGN);
    FILE *ignore = fopen(getenv("TEST_IGNORE_TERMS"), "r");
    if (ignore) {
        if (fscanf(ignore, "%d", &ignore_terms) != 1) ignore_terms = 0;
        fclose(ignore);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_term;
        sigaction(SIGTERM, &action, NULL);
    }
    int fd = open(getenv("TEST_HARDWARE"), O_CREAT|O_RDWR, 0600);
    if (fd < 0 || flock(fd, LOCK_EX|LOCK_NB)) {
        FILE *bad = fopen(getenv("TEST_OVERLAP"), "w");
        if (bad) fclose(bad);
        return 3;
    }
    printf("fake camera-app pid=%d config=%s record=%s\n", getpid(),
           getenv("CAMERA_APP_CONFIG"), getenv("CAMERA_APP_RECORD_ROOT"));
    fflush(stdout);
    if (access(getenv("TEST_FAIL"), F_OK) == 0) return 2;
    if (access(getenv("TEST_STALE"), F_OK) == 0) {
        puts("[Func]:isp_check_mem_init_state [Line]:219 [Info]:ISP[0] already inited!");
        return 1;
    }
    /* The web accepts a restart once the new PID acknowledges the saved INI. */
    uint64_t hash, acknowledged;
    if (apcam_config_file_hash(getenv("CAMERA_APP_CONFIG"), &hash)) return 5;
    if (acknowledge(hash)) return 4;
    acknowledged = hash;
    FILE *ready = fopen(getenv("CAMERA_APP_READY_PATH"), "w");
    if (!ready) return 4;
    fprintf(ready, "backend=hisi\npid=%ld\n", (long)getpid());
    fclose(ready);
    /* With TEST_LIVE_APPLY, a saved INI is applied live, as camera-app does. */
    while (1) {
        usleep(100000);
        if (access(getenv("TEST_LIVE_APPLY"), F_OK) != 0 ||
            apcam_config_file_hash(getenv("CAMERA_APP_CONFIG"), &hash) || hash == acknowledged) continue;
        if (acknowledge(hash) == 0) acknowledged = hash;
    }
}
''')
    subprocess.run(['cc', '-O2', '-I' + str(repo / 'include'), str(source),
                    '-o', str(app / 'bin/camera-app')], check=True)
    # Driver loading and mounts are mocked; module state lives in a fake /proc/modules.
    (commands / 'insmod').write_text('''#!/bin/sh
printf '%s\\n' "$*" >>"$TEST_ROOT/insmod.log"
name=${1##*/}
name=${name%.ko}
grep -qx "$name" "$TEST_ROOT/insmod.fail" 2>/dev/null && exit 1
echo "$name 16384 0 - Live 0x0" >>"$TEST_ROOT/modules"
''')
    (commands / 'mountpoint').write_text('''#!/bin/sh
[ "$1" = -q ] && shift
awk -v dir="$1" '$2 == dir {found=1} END {exit !found}' "$TEST_ROOT/mounts"
''')
    (commands / 'mount').write_text('''#!/bin/sh
[ "$1" = -o ] && shift 2
echo "$1 $2 vfat rw,noatime 0 0" >>"$TEST_ROOT/mounts"
''')
    (commands / 'umount').write_text('''#!/bin/sh
[ "$1" = -l ] && shift
awk -v dir="$1" '$2 != dir' "$TEST_ROOT/mounts" >"$TEST_ROOT/mounts.new"
mv "$TEST_ROOT/mounts.new" "$TEST_ROOT/mounts"
''')
    for command, body in (('ifconfig', 'exit 0'),
                          ('reboot', 'echo reboot >>"$TEST_ROOT/reboots"')):
        (commands / command).write_text(f'#!/bin/sh\n{body}\n')
    # A DHCP client that runs until SIGTERM, as udhcpc -f does.
    (commands / 'udhcpc').write_text('''#!/bin/sh
echo "started $*" >>"$TEST_ROOT/udhcpc.log"
trap 'echo released >>"$TEST_ROOT/udhcpc.log"; exit 0' TERM
while :; do sleep 1; done
''')
    for path in commands.iterdir():
        path.chmod(0o755)

    paths = {
        'APP_INSTALL_DIR': app, 'APP_DIR': config, 'MEDIA_ROOT': media,
        'REPLACEMENT_CAMERA_PATH': app / 'bin/camera-app',
        'WEB_PATH': app / 'bin/hisi-web',
        'PASSWORD_PATH': config / 'web.pass',
        'REPLACEMENT_CONFIG_PATH': config / 'camera.ini',
        'REPLACEMENT_CONFIG_BACKUP_PATH': config / 'camera.ini.web.bak',
        'APP_REQUEST_PATH': runtime / 'camera-app.request',
        'APP_REQUEST_LOCK_PATH': runtime / 'camera-app.request.lock',
        'APP_STARTED_PATH': runtime / 'camera-app.started',
        'CAMERA_READY_PATH': runtime / 'camera-app.ready',
        'REPLACEMENT_LOG_PATH': runtime / 'camera_app.log',
        'REPLACEMENT_LOG_OLD_PATH': runtime / 'camera_app.log.1',
        'SWITCH_LOCK_PATH': runtime / 'switch.lock',
        'UPGRADE_LOCK_PATH': runtime / 'upgrade.lock',
        'USER_LOCK_PATH': runtime / 'users.lock',
        'SESSION_PATH': runtime / 'sessions', 'RUNTIME_DIR': runtime,
        'HISI_UPDATE_TYPE_PATH': root / 'updatetype.ini',
    }
    # The served asset allowlist, as make generates it.
    subprocess.run([sys.executable, str(repo / 'tools/build_webroot.py'), str(repo / 'web/webroot'),
                    str(repo / 'web/build/webroot.h')], check=True)
    subprocess.run(['c++', '-std=gnu++17', '-O2', '-Wno-missing-field-initializers',
                    '-Wno-address-of-packed-member', '-DAPCAM_TARGET=APCAM_TARGET_HISI',
                    '-DMT11_WEB_TEST',
                    '-I' + str(repo / 'camera_app/build/mavlink/all/include'),
                    *[f'-D{key}="{value}"' for key, value in paths.items()],
                    str(repo / 'web/mt11-web.cpp'), '-o', str(app / 'bin/hisi-web'), '-lm'],
                   check=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    # camera-app's MAVLink TCP port, where the web sends its commands.
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        camera_port = listener.getsockname()[1]

    # Change paths only; run the supervisor's real loading and restart logic.
    service = (repo / 'packaging/hisi/service.sh').read_text()
    replacements = {
        'CONFIG_ROOT=/date/': f'CONFIG_ROOT={root}/date/',
        'MEDIA=/mnt\n': f'MEDIA={media}\n',
        'KOMOD=/komod\n': f'KOMOD={komod}\n',
        'CMDLINE=/proc/cmdline\n': f'CMDLINE={root}/cmdline\n',
        'MODULES=/proc/modules\n': f'MODULES={root}/modules\n',
        'WEB_PORT=80\n': f'WEB_PORT={port}\n',
        "RESTART_DELAYS='2 10 30'\n": "RESTART_DELAYS='1 1 1'\n",
        'STOP_TIMEOUT_S=15\n': 'STOP_TIMEOUT_S=2\n',
    }
    for old, new in replacements.items():
        assert old in service, old
        service = service.replace(old, new)
    service = re.sub(r'(?<![\w.])/run/', str(runtime) + '/', service)
    script = app / 'service.sh'
    script.write_text(service)
    shutil.copy(repo / 'packaging/hisi/sdcard.sh', app / 'sdcard.sh')
    # The real mcm.sh, and an MCM bundle whose run.sh records its PID and
    # arguments, then stays up as MCM does.
    mcm = re.sub(r'(?<![\w.])/run/', str(runtime) + '/', (repo / 'packaging/hisi/mcm.sh').read_text())
    (app / 'mcm.sh').write_text(mcm)
    (app / 'mcm.sh').chmod(0o755)
    mcm_settings = (repo / 'packaging/hisi/mcm-settings.json').read_bytes()
    (app / 'mcm-settings.json').write_bytes(mcm_settings)
    (app / 'mcm/bin').mkdir(parents=True)
    mcm_run = app / 'mcm/run.sh'
    mcm_run.write_text('#!/bin/sh\necho "$$ $*" >>"$TEST_ROOT/mcm.starts"\nexec sleep 1000\n')
    mcm_run.chmod(0o755)

    # A broken bring-up override and a seed cut short by a full /date.
    config.mkdir(parents=True)
    (config / 'mpp.conf').write_text('MPP_SENSOR=imx678\nif then\n')
    (config / 'web.pass').write_text('')
    env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
               TEST_ROOT=str(root), TEST_HARDWARE=str(root / 'hardware'),
               TEST_OVERLAP=str(root / 'overlap'), TEST_FAIL=str(root / 'fail'),
               TEST_IGNORE_TERM=str(root / 'ignore-term'), TEST_IGNORE_TERMS=str(root / 'ignore-terms'),
               TEST_LIVE_APPLY=str(root / 'live-apply'), TEST_STALE=str(root / 'stale'),
               SDCARD_DISKS=str(root / 'mmcblk0'), SDCARD_MOUNTS=str(mounts), SDCARD_HOST='',
               MT11_WEB_CAMERA_PORT=str(camera_port))
    log = open(root / 'service.log', 'w+')
    process = subprocess.Popen(['sh', str(script)], env=env, stdout=log, stderr=log,
                               start_new_session=True)
    request_file = runtime / 'camera-app.request'
    ready = runtime / 'camera-app.ready'

    def wait_for(predicate, message, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if process.poll() is not None:
                break
            time.sleep(0.1)
        log.flush()
        web_log = runtime / 'hisi-web.log'
        raise AssertionError('\n'.join((message, (root / 'service.log').read_text(),
                                        web_log.read_text() if web_log.exists() else '')))

    def pids(name):
        found = []
        for path in Path('/proc').glob('[0-9]*/exe'):
            try:
                if (path.parent / 'stat').read_text().split(') ')[1].startswith('Z '):
                    continue
                exe = os.readlink(path)
            except OSError:
                continue
            if exe.startswith(str(root) + '/') and Path(exe).name == name:
                found.append(int(path.parent.name))
        return found

    def request_restart():
        # The web's request publication without its PID scan and wait.
        with open(runtime / 'camera-app.request.lock', 'w') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            generation = int(request_file.read_text()) if request_file.exists() else 0
            request_file.write_text(f'{generation + 1}\n')

    def web_response(path, body=None, auth=True):
        headers = {}
        if auth:
            headers['Authorization'] = 'Basic ' + base64.b64encode(b'admin:test-password').decode()
        if body is not None:
            headers['Content-Type'] = 'application/x-www-form-urlencoded'
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=90)
        connection.request('GET' if body is None else 'POST', path, body, headers)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def web_request(path, body=None):
        status, _, page = web_response(path, body)
        return status, page

    def storage_state():
        status, headers, body = web_response('/storage/state')
        assert status == 200 and headers['Cache-Control'] == 'no-store', (status, headers)
        assert headers['Content-Type'] == 'application/json; charset=utf-8', headers
        return json.loads(body)

    def x25(data):
        crc = 0xffff
        for byte in data:
            byte ^= crc & 0xff
            byte = (byte ^ (byte << 4)) & 0xff
            crc = (crc >> 8) ^ (byte << 8) ^ (byte << 3) ^ (byte >> 4)
        return crc

    def format_card():
        # camera-app's answer to MAV_CMD_STORAGE_FORMAT (526): a MAVLink 1
        # COMMAND_ACK (77, CRC extra 143) with MAV_RESULT_IN_PROGRESS (5).
        frame = bytes((3, 0, 1, 100, 77)) + struct.pack('<HB', 526, 5)
        frame = b'\xfe' + frame + struct.pack('<H', x25(frame + bytes((143,))))
        token = csrf()
        with socket.socket() as camera:
            camera.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            camera.bind(('127.0.0.1', camera_port))
            camera.listen(1)
            camera.settimeout(30)

            def answer():
                connection, _ = camera.accept()
                with connection:
                    connection.recv(512)
                    connection.sendall(frame)
            thread = threading.Thread(target=answer)
            thread.start()
            result = web_request('/storage/format', f'csrf={token}&confirm=yes')
            thread.join(30)
        return result

    def action(name, fields):
        # 4K Cam Manager's /action requests: no login, no Origin.
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=90)
        connection.request('POST', '/action/' + name, json.dumps(fields))
        response = connection.getresponse()
        result = response.status, json.loads(response.read())
        connection.close()
        return result

    def csrf():
        status, page = web_request('/')
        assert status == 200, status
        return re.search(rb'name=csrf value="([a-f0-9]{64})"', page).group(1).decode()

    def loads(name):
        return [line for line in insmod_log.read_text().splitlines()
                if line.split()[0].endswith('/' + name + '.ko')]

    def alive(pid):
        try:
            return not Path(f'/proc/{pid}/stat').read_text().split(') ')[1].startswith('Z ')
        except OSError:
            return False

    mcm_starts = root / 'mcm.starts'

    def mcm_runs():
        return mcm_starts.read_text().splitlines() if mcm_starts.exists() else []

    def mcm_pid():
        return int(mcm_runs()[-1].split()[0])

    def service_log():
        return (root / 'service.log').read_text()

    try:
        # A driver failure leaves only the web UI running.
        wait_for(lambda: pids('hisi-web') and 'did not load' in (root / 'service.log').read_text(),
                 'MPP failure was not reported')
        assert not pids('camera-app'), 'camera-app started without its drivers'
        assert 'Ignoring' in (root / 'service.log').read_text(), 'broken mpp.conf not reported'
        assert (config / 'camera.ini').read_bytes() == (repo / 'packaging/hisi/camera.ini').read_bytes()
        assert (config / 'web.pass').read_text() == 'test-password\n', 'empty web.pass not reseeded'
        assert not (config / 'web.pass.new').exists()
        system = loads('sys_config')
        assert system == [f'{komod}/sys_config.ko sensors=sns0=imx515,sns1=null,sns2=null,'
                          'sns3=null board=dmeb vo_intf=bt1120 ir_auto=1'], system
        mmz = loads('ot_mmz')
        assert mmz == [f'{komod}/ot_mmz.ko anony=1 mmz_allocator=ot '
                       'mmz=anonymous,0,0x58200000,638M mem_process_isolation=0'], mmz
        assert loads('ot_pwm') == [f'{komod}/extdrv/ot_pwm.ko'], loads('ot_pwm')
        assert not loads('ot_wdt'), 'watchdog loaded with nothing to feed it'
        wait_for(lambda: web_request('/')[0] == 200, 'web UI not reachable')
        time.sleep(2)
        assert not mcm_runs(), 'MCM started before camera-app was ready'

        # The retry loads only the missing driver, then starts camera-app.
        insmod_fail.unlink()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        wait_for(ready.exists, 'camera-app did not start after driver retry')
        assert len(loads('sys_config')) == 1 and len(loads('ot_mmz')) == 1
        assert len(loads('ot_vi')) == 2
        assert not (media / 'DCIM').exists(), 'created media folders on an unmounted card'
        wait_for(lambda: 'fake camera-app' in (runtime / 'camera_app.log').read_text(),
                 'camera-app output not logged')
        assert f'config={config}/camera.ini record={media}/DCIM/record' in \
            (runtime / 'camera_app.log').read_text()

        # MCM starts once camera-app is ready, from /run/mcm/root, with fresh settings.
        wait_for(mcm_runs, 'MCM not started once camera-app was ready')
        mcm_state = runtime / 'mcm/state'
        assert mcm_runs()[0].split(' ', 1)[1] == (
            '--mavlink udpin:127.0.0.1:14560 --rtsp-port 8564 --disable-onvif '
            f'--settings-file {mcm_state}/settings.json --log-path {mcm_state}/logs'), mcm_runs()
        assert os.readlink(runtime / 'mcm/root') == str(app / 'mcm')
        assert (mcm_state / 'settings.json').read_bytes() == mcm_settings
        first_mcm = mcm_pid()

        # A card inserted while running is mounted and prepared, and detached
        # once removed.
        sdcard_state = runtime / 'sdcard.state'
        assert sdcard_state.read_text() == 'absent\n'
        status_page = web_request('/')[1]
        assert b'<th>microSD</th><td id=sdcard-state>no card inserted</td>' in status_page
        assert b'/storage/format' not in status_page
        assert b'http-equiv=refresh' not in status_page and b'/storage.js' not in status_page
        # The card's state for the Status page's script, behind the login
        # without a Basic challenge (a dialog over a polling page).
        status, headers, _ = web_response('/storage/state', auth=False)
        assert status == 401 and 'WWW-Authenticate' not in headers, (status, headers)
        assert storage_state() == {'state': 'absent', 'text': 'no card inserted'}
        sensors_page = web_request('/sensors')[1]
        assert b'action=/sensors/record' in sensors_page
        assert b'No microSD card is inserted' in sensors_page
        (root / 'mmcblk0').touch()
        (root / 'mmcblk0p1').touch()
        wait_for(lambda: sdcard_state.read_text() == 'ready\n', 'inserted card not mounted')
        assert mounts.read_text() == f'{root}/mmcblk0p1 {media} vfat rw,noatime 0 0\n'
        for folder in ('DCIM/record', 'DCIM/capture', 'logs'):
            assert (media / folder).is_dir(), folder
        status_page = web_request('/')[1]
        assert b'action=/storage/format' in status_page
        assert b'no card inserted' not in status_page
        # Formatting needs the confirmation box; recording a known action.
        status, page = web_request('/storage/format', f'csrf={csrf()}')
        assert status == 400 and b'Tick the box to confirm erasing' in page, page
        status, page = web_request('/sensors/record', f'csrf={csrf()}&action=pause')
        assert status == 400 and b'Unknown recording action' in page, page
        # A ready card: its row as the Status page shows it.
        card = storage_state()
        assert card['state'] == 'ready' and ('size' in card) == ('free' in card), card
        status_page = web_request('/')[1].decode()
        assert f'<td id=sdcard-state>{card["text"]}</td>' in status_page, status_page
        assert 'http-equiv=refresh' not in status_page and '/storage.js' not in status_page
        # The page answering the button watches the format before sdcard.sh
        # reports it, and reloads itself without JavaScript.
        status, page = format_card()
        page = page.decode()
        assert status == 200, page
        assert '<noscript><meta http-equiv=refresh content="3;url=/"></noscript>' in page, page
        assert '<script src=/storage.js data-started defer></script>' in page, page
        assert '<form id=sdcard-format method=post action=/storage/format hidden>' in page, page
        assert 'This page updates automatically and shows when the card is ready.' in page, page
        assert 'Refresh this page' not in page
        status, headers, script = web_response('/storage.js')
        assert status == 200 and headers['Content-Type'] == 'application/javascript; charset=utf-8'
        assert script.startswith(b"const L = {") and b"readySize:'microSD card ready (%s, %s free)'" in script
        assert b"'/storage/state?t='" in script
        # While sdcard.sh formats (holding its lock), the Status page polls,
        # or reloads itself, and hides the form.
        with open(runtime / 'sdcard.lock', 'w') as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            sdcard_state.write_text('formatting\n')
            assert storage_state() == {'state': 'formatting', 'text': 'formatting…'}
            status_page = web_request('/')[1].decode()
            assert '<td id=sdcard-state>formatting…</td>' in status_page, status_page
            assert '<noscript><meta http-equiv=refresh content="3;url=/"></noscript>' in status_page
            assert '<script src=/storage.js defer></script>' in status_page
            assert 'action=/storage/format hidden>' in status_page
            assert 'This page updates automatically' in status_page
        wait_for(lambda: sdcard_state.read_text() == 'ready\n', 'card not ready after the format')
        assert storage_state()['state'] == 'ready'
        (root / 'mmcblk0').unlink()
        (root / 'mmcblk0p1').unlink()
        wait_for(lambda: sdcard_state.read_text() == 'absent\n', 'removed card not detached')
        assert mounts.read_text() == ''

        # A web restart replaces the instance.
        old = ready.read_text()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        assert ready.exists() and ready.read_text() != old
        assert len(mcm_runs()) == 1 and alive(first_mcm), 'a camera-app restart restarted MCM'

        # An MCM that exits is restarted after 2 s, then 4 s, with its settings
        # copied again.
        (mcm_state / 'settings.json').write_text('{}')
        os.kill(first_mcm, signal.SIGKILL)
        wait_for(lambda: len(mcm_runs()) == 2, 'MCM not restarted after it exited')
        assert 'MCM exited; restarting it in 2 s' in service_log()
        assert (mcm_state / 'settings.json').read_bytes() == mcm_settings
        killed_at = time.monotonic()
        os.kill(mcm_pid(), signal.SIGKILL)
        wait_for(lambda: len(mcm_runs()) == 3, 'MCM not restarted after it exited again')
        assert 'MCM exited; restarting it in 4 s' in service_log()
        assert time.monotonic() - killed_at >= 3, 'MCM restart delay did not double'

        # mcm.disable stops MCM and keeps it stopped; so does a missing bundle.
        disable = config / 'mcm.disable'
        for remove, restore in ((disable.touch, disable.unlink),
                                (lambda: mcm_run.rename(mcm_run.with_suffix('.off')),
                                 lambda: mcm_run.with_suffix('.off').rename(mcm_run))):
            runs, pid = len(mcm_runs()), mcm_pid()
            remove()
            wait_for(lambda: not alive(pid), 'MCM not stopped')
            time.sleep(2)
            assert len(mcm_runs()) == runs, 'MCM started while disabled or without its bundle'
            restore()
            wait_for(lambda: len(mcm_runs()) == runs + 1, 'MCM not started again')
        assert service_log().count('Stopping MCM') == 2

        # MCM's log folder is kept under 4 MiB, oldest files first.
        mcm_logs = mcm_state / 'logs'
        mcm_logs.mkdir(exist_ok=True)
        for age, name in enumerate(('new', 'middle', 'old')):
            path = mcm_logs / f'{name}.log'
            path.write_bytes(b'x' * (1536 << 10))
            os.utime(path, (time.time() - 100 * (age + 1),) * 2)
        wait_for(lambda: not (mcm_logs / 'old.log').exists(), 'oldest MCM log not removed', timeout=12)
        assert sorted(path.name for path in mcm_logs.iterdir()) == ['middle.log', 'new.log']

        # The DHCP client runs only while camera.ini asks for it, follows a
        # saved change without a camera-app restart, and survives restarts.
        udhcpc_log = root / 'udhcpc.log'
        assert not udhcpc_log.exists(), 'DHCP client started while disabled'
        parameters_page = web_request('/parameters')[1].decode()
        dhcp_field = parameters_page[parameters_page.index('<select required name="network_dhcp"'):]
        dhcp_help = dhcp_field[:dhcp_field.index('</div>')]
        assert 'Applies without restarting' in dhcp_help and 'Requires' not in dhcp_help, dhcp_help
        ini = config / 'camera.ini'
        ini.write_text(ini.read_text().replace('dhcp = false', 'dhcp = true'))
        wait_for(lambda: udhcpc_log.exists() and 'started' in udhcpc_log.read_text(),
                 'DHCP client not started')
        assert udhcpc_log.read_text().splitlines() == [
            f'started -f -R -i eth0 -s {app}/udhcpc.sh -p {runtime}/udhcpc.pid '
            '-x hostname:ardupilot-4kcam'], udhcpc_log.read_text()
        started_at = time.monotonic()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        assert udhcpc_log.read_text().count('started') == 1, 'a second DHCP client started'
        # A client that dies is started again, no sooner than 10 s after the last start.
        os.kill(int((runtime / 'udhcpc.pid').read_text()), signal.SIGKILL)
        wait_for(lambda: udhcpc_log.read_text().count('started') == 2, 'dead DHCP client not restarted')
        assert time.monotonic() - started_at >= 9, 'DHCP client restarted without a pause'
        ini.write_text(ini.read_text().replace('dhcp = true', 'dhcp = false'))
        wait_for(lambda: 'released' in udhcpc_log.read_text(), 'DHCP client not stopped')
        assert 'Stopping the DHCP client' in (root / 'service.log').read_text()

        # A crash is restarted without a request.
        old = ready.read_text()
        os.kill(pids('camera-app')[0], signal.SIGKILL)
        wait_for(lambda: ready.exists() and ready.read_text() != old, 'crashed camera-app not restarted')
        assert 'restarting in 1 s (attempt 1 of 3)' in (root / 'service.log').read_text()

        # A camera-app that keeps failing is retried three times, then waits for the web.
        def starts():
            return (runtime / 'camera_app.log').read_text().count('fake camera-app pid=')
        before = starts()
        (root / 'fail').touch()
        request_restart()
        wait_for(lambda: 'failed 4 times in a row' in (root / 'service.log').read_text(),
                 'repeated failures did not stop the retries', timeout=30)
        assert starts() - before == 4, starts() - before
        time.sleep(2)
        assert not pids('camera-app') and starts() - before == 4, 'retried past the limit'
        assert web_request('/')[0] == 200
        assert not (root / 'reboots').exists(), 'rebooted for an ordinary failure'
        (root / 'fail').unlink()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        wait_for(ready.exists, 'restart did not recover camera')

        # An ISP left initialised by a crash reboots, a bounded number of times.
        def reboots():
            path = root / 'reboots'
            return len(path.read_text().splitlines()) if path.exists() else 0
        stale_count = config / 'stale-isp-reboots'
        (root / 'stale').touch()
        for attempt in range(1, 4):
            request_restart()
            wait_for(lambda: reboots() == attempt, f'stale ISP reboot {attempt} missing')
            assert stale_count.read_text() == f'{attempt}\n', stale_count.read_text()
        request_restart()
        wait_for(lambda: 'reboots did not clear it' in (root / 'service.log').read_text(),
                 'stale ISP reboots not bounded')
        assert reboots() == 3
        (root / 'stale').unlink()
        request_restart()
        wait_for(ready.exists, 'camera did not recover after the stale ISP')
        wait_for(lambda: not stale_count.exists(), 'reboot count not cleared once ready')

        # An instance that ignores SIGTERM is killed, and gone before the next one starts.
        (root / 'ignore-term').touch()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        old = ready.read_text()
        (root / 'ignore-term').unlink()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        wait_for(lambda: ready.exists() and ready.read_text() != old, 'SIGTERM-proof camera-app not replaced')
        assert len(pids('camera-app')) == 1, pids('camera-app')

        # One that loses its first SIGTERMs, as camera-app does after a live
        # media reconfigure, stops on a repeated one before the SIGKILL: the
        # supervisor's alone, then the web's and the supervisor's.
        app_log = runtime / 'camera_app.log'
        for ignored, restart in ((1, request_restart),
                                 (2, lambda: web_request('/restart', f'csrf={csrf()}'))):
            (root / 'ignore-terms').write_text(f'{ignored}\n')
            old = ready.read_text()
            request_restart()
            wait_for(lambda: ready.exists() and ready.read_text() != old, 'camera-app not restarted')
            (root / 'ignore-terms').unlink()
            pid = pids('camera-app')[0]
            old = ready.read_text()
            restart()
            wait_for(lambda: ready.exists() and ready.read_text() != old,
                     'camera-app that lost SIGTERMs not replaced')
            assert f'fake camera-app {pid} stopped by SIGTERM {ignored + 1}\n' in app_log.read_text(), \
                f'camera-app {pid} not stopped by a repeated SIGTERM\n{app_log.read_text()}'
            assert len(pids('camera-app')) == 1, pids('camera-app')

        # A 4K Cam Manager codec change that camera-app applies live doesn't restart it.
        web_log = runtime / 'hisi-web.log'

        def main_codec():
            return re.search(r'\[stream\.main\][^\[]*?^codec = (\w+)$', ini.read_text(), re.M).group(1)
        assert main_codec() == 'h264'
        (root / 'live-apply').touch()
        old = ready.read_text()
        status, reply = action('setVencConf', {'channel': 0, 'encode_type': 5})
        assert status == 200 and reply['code'] == 0, reply
        assert main_codec() == 'h265'
        time.sleep(2)
        assert ready.read_text() == old, 'a codec change applied live restarted camera-app'
        assert 'restarting to apply the codec' not in web_log.read_text()
        # One that isn't acknowledged as applied restarts camera-app after the reply.
        (root / 'live-apply').unlink()
        status, reply = action('setVencConf', {'channel': 0, 'encode_type': 1})
        assert status == 200 and reply['code'] == 0, reply
        try:
            replaced = ready.read_text() != old
        except FileNotFoundError:
            replaced = False
        assert not replaced, 'setVencConf replied only after the restart'
        assert main_codec() == 'h264'
        wait_for(lambda: ready.exists() and ready.read_text() != old,
                 'unacknowledged codec change did not restart camera-app')
        assert 'restarting to apply the codec' in web_log.read_text(), web_log.read_text()

        old_web = pids('hisi-web')[0]
        os.kill(old_web, signal.SIGKILL)
        wait_for(lambda: pids('hisi-web') and pids('hisi-web')[0] != old_web, 'web did not respawn')
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        assert len(loads('sys_config')) == 1, 'drivers reloaded on restart'
        assert not (root / 'overlap').exists(), 'concurrent camera-app instances'

        # Iris mode: without a working DC iris the row stays, disabled with a
        # note. camera-app's <ready>.iris decides once the running instance
        # wrote it; until then the device code does, as camera-app will.
        wait_for(ready.exists, 'camera-app not ready for the iris checks')
        app_pid = int(re.search(r'^pid=(\d+)$', ready.read_text(), re.M).group(1))
        iris_file = runtime / 'camera-app.ready.iris'
        update_type = root / 'updatetype.ini'

        def iris_field():
            page = web_request('/parameters')[1].decode()
            start = page.index('<label for="iris_mode">')
            return page[start:page.index('<div class=field>', start)]

        def iris_status(pid=app_pid, dc_iris=1, active=0, requested=0, applied=0):
            iris_file.write_text(f'pid={pid}\ndc_iris={dc_iris}\npriority_active={active}\n'
                                 f'shutter_requested_us={requested}\nshutter_applied_us={applied}\n')

        def device_code(platform):
            update_type.write_text(f'[UPDATE_SET]\nDEV_TYPE = 19\nSDK_TYPE = 2\nPLATFORM_TYPE = {platform}\n')

        note = 'Not available: this camera has no working DC iris.'

        def disabled(field):
            return '<select disabled required name="iris_mode"' in field and note in field

        def enabled(field):
            return '<select required name="iris_mode"' in field and note not in field

        assert not iris_file.exists()
        field = iris_field()
        assert disabled(field) and f'title="{note}"' in field, field
        device_code(51)
        assert enabled(iris_field()), iris_field()
        device_code(35)
        assert disabled(iris_field()), iris_field()
        # camera-app's word wins over the device code, but only from the running instance
        iris_status(dc_iris=1)
        assert enabled(iris_field()) and 'Applied shutter' not in iris_field(), iris_field()
        iris_status(pid=app_pid + 100000, dc_iris=1)
        assert disabled(iris_field()), iris_field()
        device_code(51)
        iris_status(dc_iris=0)
        assert disabled(iris_field()), iris_field()
        # the shutter iris priority applied, and why it is shorter than requested
        iris_status(active=1, requested=83333, applied=33333)
        field = iris_field()
        assert enabled(field) and \
            'Applied shutter: 1/30 s (requested 1/12 s, limited by the frame rate)' in field, field
        iris_status(active=1, requested=16666, applied=16666)
        field = iris_field()
        assert 'Applied shutter: 1/60 s' in field and 'requested' not in field, field

        def browser_form(page):
            # what a browser submits: disabled controls are left out
            page = page[page.index('<form method=post action=/parameters>'):]
            page = page[:page.index('</form>')]
            fields = {'csrf': re.search(r'name=csrf value="([a-f0-9]{64})"', page).group(1), 'action': 'save'}
            for attributes, options in re.findall(r'<select ([^>]*)>(.*?)</select>', page, re.S):
                if re.match(r'disabled\b', attributes):
                    continue
                selected = re.search(r'<option (?:value="([^"]*)" selected|selected value="([^"]*)")>', options)
                name = re.search(r'name="([^"]+)"', attributes).group(1)
                assert selected, f'{name} has no selected value'
                fields[name] = html.unescape(selected.group(1) if selected.group(1) is not None
                                             else selected.group(2))
            for name, value in re.findall(r'<input (?:required )?name="([^"]+)"[^>]*? value="([^"]*)"', page):
                fields[name] = html.unescape(value)
            return fields

        def save(changes=None, drop=()):
            fields = browser_form(web_request('/parameters')[1].decode())
            fields.update(changes or {})
            for name in drop:
                fields.pop(name, None)
            return web_request('/parameters', urllib.parse.urlencode(fields))

        def saved_iris_mode():
            match = re.search(r'^\[image\][^\[]*?^iris_mode = (\w+)$', ini.read_text(), re.M)
            return match.group(1) if match else None

        # Unavailable: the disabled control keeps the saved value; priority can't be newly chosen.
        (root / 'live-apply').touch()
        iris_status(dc_iris=0)
        ini.write_text(ini.read_text() + '\n[image]\niris_mode = priority\n')
        status, page = save()
        assert status == 200 and b'Parameters saved' in page, page
        assert saved_iris_mode() == 'priority', ini.read_text()
        ini.write_text(ini.read_text().replace('iris_mode = priority', 'iris_mode = auto'))
        before = ini.read_text()
        status, page = save({'iris_mode': 'priority'})
        page = page.decode()
        assert status == 400 and 'Iris priority needs a working DC iris' in page, (status, page)
        assert ini.read_text() == before, 'a refused iris priority was saved'
        rejected = page[page.index('<label for="iris_mode">'):]
        assert '<option value="auto" selected>' in rejected[:rejected.index('</select>')], rejected
        # Available: an ordinary setting.
        iris_status(dc_iris=1)
        status, page = save({'iris_mode': 'priority'})
        assert status == 200 and b'Parameters saved' in page, page
        assert saved_iris_mode() == 'priority', ini.read_text()
        status, page = save({'iris_mode': 'auto'})
        assert status == 200 and saved_iris_mode() == 'auto', ini.read_text()

        # Keyframe intervals: number rows saved to [stream.main] / [stream.sub], applied live.
        def keyint_field(name):
            page = web_request('/parameters')[1].decode()
            start = page.index(f'<label for="{name}">')
            return page[start:page.index('</div>', page.index('<div class=help>', start))]

        def saved_keyint(stream):
            match = re.search(rf'^\[stream\.{stream}\][^\[]*?^keyframe_interval_ms = (\d+)$', ini.read_text(), re.M)
            return match.group(1) if match else None

        rows = (('main_keyframe_interval', 'main', 'Main RTSP keyframe interval (ms)'),
                ('sub_keyframe_interval', 'sub', 'Sub RTSP keyframe interval (ms)'))
        for name, stream, label in rows:
            field = keyint_field(name)
            assert f'>{label}</label><input required name="{name}" id="{name}" type=number min="30" ' \
                   f'max="10000" step="1" value="1000">' in field, field
            assert 'at the constant bitrate frequent keyframes lower the quality' in field, field
            assert saved_keyint(stream) == '1000', ini.read_text()
        old = ready.read_text()
        status, page = save({'main_keyframe_interval': '500', 'sub_keyframe_interval': '2000'})
        assert status == 200 and b'Parameters saved' in page, page
        assert (saved_keyint('main'), saved_keyint('sub')) == ('500', '2000'), ini.read_text()
        assert 'value="500">' in keyint_field('main_keyframe_interval')
        assert 'value="2000">' in keyint_field('sub_keyframe_interval')
        assert ready.read_text() == old, 'a keyframe interval applied live restarted camera-app'
        before = ini.read_text()
        for value in ('29', '10001', '1.5'):
            status, page = save({'main_keyframe_interval': value})
            assert status == 400 and ini.read_text() == before, (value, status, page)
        status, page = save({'main_keyframe_interval': '1000', 'sub_keyframe_interval': '1000'})
        assert status == 200 and (saved_keyint('main'), saved_keyint('sub')) == ('1000', '1000'), ini.read_text()
        (root / 'live-apply').unlink()
        iris_file.unlink()
        update_type.unlink()

        # Stopping the service stops MCM too.
        pid = mcm_pid()
        assert alive(pid), 'MCM not running before the stop'
        process.terminate()
        assert process.wait(timeout=30) == 0, service_log()
        assert not alive(pid), 'MCM left running by stop'
        print('PASS hisi: xipc driver set and MMZ, broken mpp.conf, seeding, driver retry, SD folders, '
              'restart, DHCP switch, crash restart, retry limit, stale-ISP reboot, SIGTERM-proof stop, '
              'repeated SIGTERM, live and restarting codec change, '
              'web respawn, iris mode row and save, keyframe interval rows and save, MCM start after ready, restart backoff, mcm.disable, '
              'missing bundle, log pruning and stop')
    finally:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
        log.close()
