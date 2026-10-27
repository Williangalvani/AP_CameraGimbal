#!/usr/bin/env python3
"""Run the actual Hi3516DV500 supervisor and web UI with fake drivers and camera."""
from pathlib import Path
import base64
import fcntl
import http.client
import os
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time

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
#include <unistd.h>
int main(void) {
    if (access(getenv("TEST_IGNORE_TERM"), F_OK) == 0) signal(SIGTERM, SIG_IGN);
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
    char path[4096];
    uint64_t hash;
    if (apcam_config_file_hash(getenv("CAMERA_APP_CONFIG"), &hash)) return 5;
    snprintf(path, sizeof(path), "%s.config", getenv("CAMERA_APP_READY_PATH"));
    FILE *status = fopen(path, "w");
    if (!status) return 4;
    fprintf(status, "%016llx %ld applied\nConfiguration applied.\n",
            (unsigned long long)hash, (long)getpid());
    fclose(status);
    FILE *ready = fopen(getenv("CAMERA_APP_READY_PATH"), "w");
    if (!ready) return 4;
    fprintf(ready, "backend=hisi\npid=%ld\n", (long)getpid());
    fclose(ready);
    while (1) pause();
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
    for command, body in (('mount', 'exit 1'), ('ifconfig', 'exit 0'),
                          ('reboot', 'echo reboot >>"$TEST_ROOT/reboots"')):
        (commands / command).write_text(f'#!/bin/sh\n{body}\n')
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
    }
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

    # A broken bring-up override and a seed cut short by a full /date.
    config.mkdir(parents=True)
    (config / 'mpp.conf').write_text('MPP_SENSOR=imx678\nif then\n')
    (config / 'web.pass').write_text('')
    env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
               TEST_ROOT=str(root), TEST_HARDWARE=str(root / 'hardware'),
               TEST_OVERLAP=str(root / 'overlap'), TEST_FAIL=str(root / 'fail'),
               TEST_IGNORE_TERM=str(root / 'ignore-term'), TEST_STALE=str(root / 'stale'))
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

    def web_request(path, body=None):
        headers = {'Authorization': 'Basic ' + base64.b64encode(b'admin:test-password').decode()}
        if body is not None:
            headers['Content-Type'] = 'application/x-www-form-urlencoded'
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=90)
        connection.request('GET' if body is None else 'POST', path, body, headers)
        response = connection.getresponse()
        result = response.status, response.read()
        connection.close()
        return result

    def csrf():
        status, page = web_request('/')
        assert status == 200, status
        return re.search(rb'name=csrf value="([a-f0-9]{64})"', page).group(1).decode()

    def loads(name):
        return [line for line in insmod_log.read_text().splitlines()
                if line.split()[0].endswith('/' + name + '.ko')]

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

        # A web restart replaces the instance; a card mounted since is prepared.
        mounts.write_text(f'/dev/mmcblk0p1 {media} vfat rw 0 0\n')
        old = ready.read_text()
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        assert ready.exists() and ready.read_text() != old
        for folder in ('DCIM/record', 'DCIM/capture', 'logs'):
            assert (media / folder).is_dir(), folder

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

        old_web = pids('hisi-web')[0]
        os.kill(old_web, signal.SIGKILL)
        wait_for(lambda: pids('hisi-web') and pids('hisi-web')[0] != old_web, 'web did not respawn')
        assert web_request('/restart', f'csrf={csrf()}')[0] == 303
        assert len(loads('sys_config')) == 1, 'drivers reloaded on restart'
        assert not (root / 'overlap').exists(), 'concurrent camera-app instances'
        print('PASS hisi: xipc driver set and MMZ, broken mpp.conf, seeding, driver retry, SD folders, '
              'restart, crash restart, retry limit, stale-ISP reboot, SIGTERM-proof stop, web respawn')
    finally:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
        log.close()
