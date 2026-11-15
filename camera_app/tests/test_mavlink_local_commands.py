#!/usr/bin/env python3
"""Without a flight controller, only a TCP client on this host (the web UI) is answered."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from test_mavlink_integration import (
    STORAGE_TOOL, mavutil, reserve_tcp_udp_port, terminate, wait_path,
)

M = mavutil.mavlink


def web_link(host, port):
    # The web UI's identity and its broadcast-target command.
    return mavutil.mavlink_connection(f'tcp:{host}:{port}', source_system=255, source_component=191)


def format_card(link):
    link.mav.command_long_send(0, 0, M.MAV_CMD_STORAGE_FORMAT, 0, 1, 1, 0, 0, 0, 0, 0)


def received(link, seconds):
    messages = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        message = link.recv_match(blocking=True, timeout=max(0.05, deadline - time.monotonic()))
        if message is not None and message.get_type() != 'BAD_DATA':
            messages.append(message)
    return messages


def other_ipv4():
    interfaces = json.loads(subprocess.check_output(['ip', '-j', '-4', 'address', 'show', 'up'], text=True))
    for interface in interfaces:
        if 'LOOPBACK' in interface['flags']:
            continue
        for address in interface.get('addr_info', []):
            if address['family'] == 'inet':
                return address['local']
    return None


def main():
    binary, simulator = [str(Path(arg).resolve()) for arg in sys.argv[1:]]
    vendor_port, mav_port, gimbal_port = [reserve_tcp_udp_port() for _ in range(3)]
    with tempfile.TemporaryDirectory(prefix='mavlink-local-') as directory:
        root = Path(directory)
        config, ready, gimbal_ready, state, tool = [
            root / name for name in ('camera.ini', 'ready', 'gimbal.ready', 'storage.state', 'sdcard.sh')]
        config.write_text('[mavlink]\nsystem_id=0\n')
        state.write_text('unformatted /dev/mmcblk0\n')
        tool.write_text(STORAGE_TOOL)
        tool.chmod(0o755)
        env = dict(os.environ, CAMERA_APP_UART=f'udp://127.0.0.1:{gimbal_port}',
                   CAMERA_APP_PORT=str(vendor_port), CAMERA_APP_CONFIG=str(config),
                   CAMERA_APP_MAVLINK_TCP_PORT=str(mav_port), CAMERA_APP_MAVLINK_UDP_PORT='0',
                   CAMERA_APP_READY_PATH=str(ready), CAMERA_APP_LOG_ROOT=str(root / 'logs'),
                   CAMERA_APP_STORAGE_STATE=str(state), CAMERA_APP_STORAGE_TOOL=str(tool))
        camera = gimbal = None
        with (root / 'camera.log').open('w+') as log:
            try:
                gimbal = subprocess.Popen([sys.executable, simulator, '--port', str(gimbal_port),
                    '--ready-file', str(gimbal_ready)], stdout=log, stderr=log)
                wait_path(gimbal_ready, gimbal)
                camera = subprocess.Popen([binary], env=env, stdout=log, stderr=log)
                wait_path(ready, camera)

                remote = other_ipv4()
                if remote is None:
                    print('SKIP non-loopback client: no up non-loopback IPv4 interface')
                else:
                    link = web_link(remote, mav_port)
                    format_card(link)
                    assert received(link, 1.0) == []
                    link.close()
                    assert not tool.with_suffix('.sh.log').exists()

                link = web_link('127.0.0.1', mav_port)
                format_card(link)
                messages = received(link, 3.0)
                assert [m.get_type() for m in messages] == ['COMMAND_ACK', 'COMMAND_ACK'], messages
                assert [m.result for m in messages] == [M.MAV_RESULT_IN_PROGRESS, M.MAV_RESULT_ACCEPTED]
                assert all(m.command == M.MAV_CMD_STORAGE_FORMAT and m.target_system == 255 and
                           m.target_component == 191 for m in messages), messages
                assert tool.with_suffix('.sh.log').read_text() == 'format\n'
                assert state.read_text() == 'ready\n'
                link.close()
            except BaseException:
                log.flush()
                log.seek(0)
                print(f'\n--- camera.log ---\n{log.read()}', file=sys.stderr)
                raise
            finally:
                for process in (camera, gimbal):
                    if process is not None:
                        terminate(process)
    print('PASS local MAVLink commands are answered before a flight controller sets the system ID')


if __name__ == '__main__':
    main()
