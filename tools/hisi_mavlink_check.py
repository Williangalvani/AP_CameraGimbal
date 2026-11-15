#!/usr/bin/env python3
"""Check a HiSilicon camera's MAVLink image controls against its video.

Usage:
  hisi_mavlink_check.py list
  hisi_mavlink_check.py run [CHECK...] [--save-dir DIR]

Talks to camera-app's MAVLink UDP port as a GCS, after a heartbeat that
stands in for a flight controller (camera-app is silent until it sees one).
It downloads camera.xml over MAVLink FTP and checks it against the
PARAM_EXT list, then sets each image parameter with PARAM_EXT_SET, reads it
back over MAVLink and in the web form, and judges the RTSP frames with the
verdicts of hisi_image_check.py. The other settings a check needs are put
in place through the web form, and every setting is restored afterwards.
The stand-in flight controller is forgotten by restarting camera-app at
the end.

Settings apply to the camera while this runs, so don't fly or record with it.
CAM_HOST selects the camera (default 192.168.2.10), CAM_MAV_PORT its MAVLink
UDP port (default 14550) and CAM_MAV_SYSID the stand-in flight controller's
system ID (default 1). Needs pymavlink, ffmpeg, ffprobe and numpy.
"""
import argparse
import os
import struct
import sys
import threading
import time
import urllib.parse
import xml.etree.ElementTree as ET

os.environ.setdefault('MAVLINK20', '1')
from pymavlink import mavutil  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hisi_image_check import (AE, AUTO_WB, FAIL, NEUTRAL_MANUAL, PASS, SHUTTERS,  # noqa: E402
                              UNSURE, Runner, colour_changes, cross, grab, inverted, measure,
                              metering, save_png, stream_is, trend)
from hisi_web_param import CameraWeb  # noqa: E402

M = mavutil.mavlink
CAMERA = M.MAV_COMP_ID_CAMERA
GCS = (255, 190)


class CameraLink:
    """A GCS on camera-app's MAVLink UDP port, with a stand-in flight
    controller heartbeat sent once a second."""

    def __init__(self, host, port, system):
        self.system = system
        self.link = mavutil.mavlink_connection(f'udpout:{host}:{port}', source_system=GCS[0],
                                               source_component=GCS[1])
        self.autopilot = M.MAVLink(self.link, srcSystem=system, srcComponent=1)
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.heartbeats, daemon=True)
        self.thread.start()
        self.parameters = {}

    def heartbeats(self):
        while not self.stop.is_set():
            with self.lock:
                self.autopilot.heartbeat_send(M.MAV_TYPE_QUADROTOR, M.MAV_AUTOPILOT_ARDUPILOTMEGA,
                                              0, 0, M.MAV_STATE_ACTIVE)
                self.link.mav.heartbeat_send(M.MAV_TYPE_GCS, M.MAV_AUTOPILOT_INVALID, 0, 0,
                                             M.MAV_STATE_ACTIVE)
            self.stop.wait(1)

    def send(self, name, *args):
        with self.lock:
            getattr(self.link.mav, name + '_send')(*args)

    def drain(self):
        while self.link.recv_match(blocking=False) is not None:
            pass

    def receive(self, kind, predicate=lambda m: True, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            message = self.link.recv_match(type=kind, blocking=True, timeout=0.1)
            if (message is not None and message.get_srcComponent() == CAMERA and
                    predicate(message)):
                return message
        raise TimeoutError(f'no {kind} from the camera')

    def wait_camera(self, timeout=60):
        """The camera's first heartbeat after it has seen the stand-in."""
        return self.receive('HEARTBEAT', lambda m: m.get_srcSystem() == self.system, timeout)

    def command(self, command, *params, timeout=10):
        self.drain()
        params = list(params) + [0] * (7 - len(params))
        self.send('command_long', self.system, CAMERA, command, 0, *params)
        return self.receive('COMMAND_ACK', lambda m: m.command == command, timeout)

    def information(self):
        self.drain()
        self.send('command_long', self.system, CAMERA, M.MAV_CMD_REQUEST_MESSAGE, 0,
                  M.MAVLINK_MSG_ID_CAMERA_INFORMATION, 0, 0, 0, 0, 0, 0)
        return self.receive('CAMERA_INFORMATION')

    def download(self, path):
        """Read PATH with MAVLink FTP."""
        sequence = 0

        def ftp(opcode, session=0, data=b'', size=None, offset=0):
            nonlocal sequence
            sequence = (sequence + 2) & 65535
            payload = struct.pack('<HBBBBBBI', sequence, session, opcode,
                                  len(data) if size is None else size, 0, 0, 0, offset) + data
            self.send('file_transfer_protocol', 0, self.system, CAMERA, payload.ljust(251, b'\0'))
            reply = self.receive('FILE_TRANSFER_PROTOCOL', lambda m:
                                 struct.unpack('<H', bytes(m.payload[:2]))[0] == (sequence + 1) & 65535)
            raw = bytes(reply.payload)
            header = struct.unpack('<HBBBBBBI', raw[:12])
            if header[2] != 128:
                raise RuntimeError(f'FTP opcode {opcode} refused: error {raw[12]}')
            return header, raw[12:12 + header[3]]

        header, data = ftp(4, data=path.encode())
        size, session, content = struct.unpack('<I', data[:4])[0], header[1], b''
        while len(content) < size:
            _, data = ftp(5, session, size=239, offset=len(content))
            content += data
        ftp(1, session)
        return content

    def param_list(self, timeout=10):
        self.drain()
        self.send('param_ext_request_list', self.system, CAMERA)
        seen, count = {}, None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and (count is None or len(seen) < count):
            try:
                value = self.receive('PARAM_EXT_VALUE', timeout=1)
            except TimeoutError:
                continue
            count = value.param_count
            seen[value.param_index] = value.param_id
        return count, seen

    @staticmethod
    def encode(kind, value):
        if kind == M.MAV_PARAM_EXT_TYPE_UINT8:
            return struct.pack('<B', int(value)).ljust(128, b'\0')
        if kind == M.MAV_PARAM_EXT_TYPE_REAL32:
            return struct.pack('<f', float(value)).ljust(128, b'\0')
        return struct.pack('<i', int(value)).ljust(128, b'\0')

    @staticmethod
    def raw_value(message):
        """param_value as sent: pymavlink decodes the char array as text.
        It follows two uint16s and the id in PARAM_EXT_VALUE, the id alone
        in PARAM_EXT_ACK; MAVLink 2 trims trailing zero bytes."""
        packet = bytes(message.get_msgbuf())
        payload = packet[10:10 + packet[1]]
        offset = 20 if message.get_type() == 'PARAM_EXT_VALUE' else 16
        return payload[offset:offset + 4].ljust(4, b'\0')

    @staticmethod
    def decode(kind, raw):
        if kind == M.MAV_PARAM_EXT_TYPE_UINT8:
            return raw[0]
        if kind == M.MAV_PARAM_EXT_TYPE_REAL32:
            return struct.unpack('<f', raw[:4])[0]
        return struct.unpack('<i', raw[:4])[0]

    def read(self, name):
        self.drain()
        self.send('param_ext_request_read', self.system, CAMERA, name.encode(), -1)
        value = self.receive('PARAM_EXT_VALUE', lambda m: m.param_id == name)
        return self.decode(value.param_type, self.raw_value(value))

    def write(self, name, value, kind=None):
        kind = self.parameters[name]['type'] if kind is None else kind
        self.drain()
        self.send('param_ext_set', self.system, CAMERA, name.encode(), self.encode(kind, value), kind)
        ack = self.receive('PARAM_EXT_ACK', lambda m: m.param_id == name and
                           m.param_result != M.PARAM_ACK_IN_PROGRESS, timeout=60)
        return ack.param_result

    def legacy_read(self, name):
        self.drain()
        self.send('param_request_read', self.system, CAMERA, name.encode(), -1)
        return self.receive('PARAM_VALUE', lambda m: m.param_id == name).param_value

    def legacy_write(self, name, value):
        self.drain()
        self.send('param_set', self.system, CAMERA, name.encode(), float(value),
                  M.MAV_PARAM_TYPE_REAL32)
        return self.receive('PARAM_VALUE', lambda m: m.param_id == name, timeout=60).param_value

    def close(self):
        self.stop.set()
        self.thread.join()
        self.link.close()


def parse_definition(xml):
    types = {'int32': M.MAV_PARAM_EXT_TYPE_INT32, 'bool': M.MAV_PARAM_EXT_TYPE_UINT8,
             'float': M.MAV_PARAM_EXT_TYPE_REAL32}
    parameters = {}
    for node in ET.fromstring(xml).iter('parameter'):
        options = {o.get('name'): int(o.get('value')) for o in node.iter('option')}
        parameters[node.get('name')] = {
            'type': types[node.get('type')], 'options': options,
            'min': float(node.get('min', 'nan')), 'max': float(node.get('max', 'nan'))}
    return parameters


def wire_value(parameter, value):
    """The PARAM_EXT value for a web form value."""
    if parameter['options']:
        if value not in parameter['options']:
            raise KeyError(f'{value!r} is not one of {", ".join(parameter["options"])}')
        return parameter['options'][value]
    if parameter['type'] == M.MAV_PARAM_EXT_TYPE_UINT8:
        return 1 if value == 'true' else 0
    return int(value)


def check(name, parameter, field, values, verdict, context=None, settle=2.5, kind='image',
          restart=False, note='', options=None):
    """OPTIONS gives the values of a parameter camera.xml does not define,
    which is then set with PARAM_SET."""
    return {'name': name, 'parameter': parameter, 'setting': field, 'values': values,
            'verdict': verdict, 'context': context or {}, 'settle': settle, 'kind': kind,
            'restart': restart, 'note': note, 'options': options}


CHECKS = [
    check('definition', None, None, [], None, kind='definition',
          note='camera.xml over MAVLink FTP matches the PARAM_EXT list'),
    check('readback', None, None, [], None, kind='readback',
          note='every image parameter reads the value the web form shows'),
    check('limits', None, None, [], None, kind='limits',
          note='out-of-range and unknown option values are refused'),
    check('legacy_param_set', 'IMG_BRIGHTNESS', 'brightness', ['35', '65'], trend('luma', 1, 0.1),
          AUTO_WB, kind='legacy', note='PARAM_SET as well as PARAM_EXT_SET'),
    check('brightness', 'IMG_BRIGHTNESS', 'brightness', ['30', '70'], trend('luma', 1, 0.15), AUTO_WB),
    check('contrast', 'IMG_CONTRAST', 'contrast', ['20', '80'], trend('contrast', 1, 0.15), AUTO_WB),
    check('saturation', 'IMG_SATURATION', 'saturation', ['20', '80'], trend('saturation', 1, 0.3),
          AUTO_WB),
    check('exposure', 'IMG_EXPOSURE', 'exposure_compensation', ['-6', '0', '6'],
          trend('luma', 1, 0.15, each=0.05), AE, settle=4),
    check('iso', 'IMG_ISO', 'iso', ['100', '200', '400', '800', '1600', '3200'],
          trend('luma', 1, 0.3), dict(AE, shutter='1/250'), settle=3),
    check('shutter', 'IMG_SHUTTER', 'shutter', SHUTTERS, trend('luma', 1, 0.3),
          dict(AE, iso='400'), settle=3, note='above 1/30 the frame time limits it'),
    check('metering_center', 'IMG_METERING', 'metering', ['average', 'center'], metering('centre'),
          AE, settle=4),
    check('metering_spot', 'IMG_METERING', 'metering', ['average', 'spot'], metering('spot'),
          AE, settle=4),
    check('white_balance', 'IMG_WHITE_BAL', 'white_balance',
          ['incandescent', 'fluorescent', 'daylight', 'cloudy'], trend('blue', -1, 0.1)),
    check('white_balance_auto', 'IMG_WHITE_BAL', 'white_balance', ['manual', 'auto'],
          colour_changes(0.1), {'wb_red': '256', 'wb_green': '256', 'wb_blue': '256'}),
    check('wb_temp', 'IMG_WB_TEMP', 'wb_temp', ['2500', '4000', '6500', '10000'],
          trend('blue', -1, 0.1), {'white_balance': 'temperature'}),
    check('wb_red', 'IMG_WB_RED', 'wb_red', ['300', '600'], trend('red', 1, 0.2), NEUTRAL_MANUAL),
    check('wb_green', 'IMG_WB_GREEN', 'wb_green', ['256', '400'], trend('green', 1, 0.15),
          NEUTRAL_MANUAL),
    check('wb_blue', 'IMG_WB_BLUE', 'wb_blue', ['300', '600'], trend('blue', 1, 0.2), NEUTRAL_MANUAL),
    check('wb_onepush', 'CAM_WB_ONEPUSH', None, [], None,
          dict(NEUTRAL_MANUAL, wb_red='256', wb_blue='256'), kind='one_push',
          note='green water; the gains it measures are saved as manual'),
    check('osd_cross', 'OSD_CROSS', 'osd_cross', ['false', 'true'], cross, AUTO_WB, kind='centre'),
    check('orientation', 'MOUNT_ORIENT', 'orientation', ['upright', 'inverted'], inverted, AUTO_WB,
          restart=True, note='PARAM_SET only, a mount setting; applies when camera-app starts',
          options={'auto': 0, 'upright': 1, 'inverted': 2}),
    check('main_resolution', 'VIDEO_MAIN_RES', 'main_resolution', ['3840x2160', '1920x1080'],
          stream_is('size', ['3840x2160', '1920x1080']), kind='main'),
    check('main_codec', 'VIDEO_MAIN_CODEC', 'main_codec', ['h264', 'h265'],
          stream_is('codec', ['h264', 'hevc']), kind='main'),
    check('main_bitrate', 'VIDEO_MAIN_KBPS', 'main_bitrate', ['4000', '16000'],
          trend('kbps', 1, 1.0), kind='main'),
    check('main_frame_rate', 'VIDEO_MAIN_FPS', 'main_frame_rate', ['30', '15'],
          stream_is('fps', [30.0, 15.0]), kind='main', restart=True,
          note='applies when camera-app starts'),
    check('sub_resolution', 'VIDEO_SUB_RES', 'sub_resolution', ['1280x720', '1920x1080'],
          stream_is('size', ['1280x720', '1920x1080']), kind='sub'),
    check('sub_codec', 'VIDEO_SUB_CODEC', 'sub_codec', ['h264', 'h265'],
          stream_is('codec', ['h264', 'hevc']), kind='sub'),
    check('capture', None, None, [], None, kind='capture',
          note='MAV_CMD_IMAGE_START_CAPTURE and CAMERA_IMAGE_CAPTURED'),
]
IMAGE_PARAMETERS = {c['parameter']: c['setting'] for c in CHECKS
                    if c['parameter'] and c['setting'] and c['kind'] != 'legacy' and not c['options']}
LEGACY_PARAMETERS = {c['parameter']: c for c in CHECKS if c['options']}


class MavlinkRunner(Runner):
    def __init__(self, web, mav, save_dir=None):
        super().__init__(web, save_dir)
        self.mav = mav

    def restart(self):
        self.web.save({}, restart=True)
        self.wait_stream('sub')
        self.mav.wait_camera()

    def apply(self, item, value, legacy=False):
        if item['options']:
            parameter = {'options': item['options'], 'type': M.MAV_PARAM_EXT_TYPE_INT32}
            legacy = True
        elif item['parameter'] not in self.mav.parameters:
            raise RuntimeError(f'{item["parameter"]} is not in camera.xml')
        else:
            parameter = self.mav.parameters[item['parameter']]
        wire = wire_value(parameter, value)
        if legacy:
            got = self.mav.legacy_write(item['parameter'], wire)
            if got != wire:
                raise RuntimeError(f'PARAM_SET {item["parameter"]}={wire} answered {got}')
        else:
            result = self.mav.write(item['parameter'], wire)
            if result != M.PARAM_ACK_ACCEPTED:
                raise RuntimeError(f'PARAM_EXT_SET {item["parameter"]}={wire}: result {result}')
        read = (self.mav.legacy_read if legacy else self.mav.read)(item['parameter'])
        shown = self.web.form().get(item['setting'])
        if read != wire or shown != value:
            raise RuntimeError(f'{item["parameter"]}={value}: MAVLink reads {read}, web shows {shown}')

    def run(self, item):
        kind = item['kind']
        if kind in ('definition', 'readback', 'limits', 'capture', 'one_push'):
            return getattr(self, kind)(item)
        touched = set(item['context']) | {item['setting']}
        results = []
        try:
            if item['context']:
                self.save(item['context'])
            for value in item['values']:
                self.apply(item, value, legacy=kind == 'legacy')
                if item['restart']:
                    self.restart()
                if kind in ('main', 'sub'):
                    time.sleep(3)
                    self.wait_stream(kind)
                time.sleep(item['settle'])
                results.append(self.measure_value(item))
        finally:
            self.restore(sorted(touched), item['restart'])
            if item['restart']:
                self.mav.wait_camera()
        if self.save_dir and results and 'frame' in results[0]:
            save_png(os.path.join(self.save_dir, f'{item["name"]}.png'), [r['frame'] for r in results])
        return item['verdict'](results)

    def measure_value(self, item):
        if item['kind'] == 'legacy':
            item = dict(item, kind='image')
        return super().measure_value(item)

    def definition(self, item):
        info = self.mav.information()
        uri = info.cam_definition_uri
        path = uri.split(']', 1)[-1] if uri.startswith('mftp://') else None
        if not path:
            return FAIL, f'definition URI {uri!r} is not mftp://'
        xml = self.mav.download(path)
        self.mav.parameters = parse_definition(xml)
        count, listed = self.mav.param_list()
        names = set(self.mav.parameters)
        missing = sorted(names - set(listed.values()))
        extra = sorted(set(listed.values()) - names)
        text = (f'{uri}: {len(xml)} bytes, version {info.cam_definition_version}, '
                f'{len(names)} parameters; PARAM_EXT list {len(listed)} of {count}')
        absent = sorted(set(IMAGE_PARAMETERS) - names)
        if missing or extra or count != len(listed) or absent:
            return FAIL, text + f' (missing {missing}, extra {extra}, not defined {absent})'
        return PASS, text

    def readback(self, item):
        form = self.web.form()
        wrong = []
        for name, field in IMAGE_PARAMETERS.items():
            if name not in self.mav.parameters:
                wrong.append(f'{name} not in camera.xml')
                continue
            want = wire_value(self.mav.parameters[name], form[field])
            got, legacy = self.mav.read(name), self.mav.legacy_read(name)
            if got != want or legacy != want:
                wrong.append(f'{name} PARAM_EXT {got} PARAM {legacy}, web {form[field]} = {want}')
        for name, item in LEGACY_PARAMETERS.items():
            want, got = item['options'][form[item['setting']]], self.mav.legacy_read(name)
            if got != want:
                wrong.append(f'{name} PARAM {got}, web {form[item["setting"]]} = {want}')
        if wrong:
            return FAIL, '; '.join(wrong)
        return PASS, (f'{len(IMAGE_PARAMETERS) + len(LEGACY_PARAMETERS)} parameters agree with '
                      f'the web form')

    def limits(self, item):
        before = self.web.form()
        cases = [('IMG_BRIGHTNESS', 101), ('IMG_BRIGHTNESS', -1), ('IMG_EXPOSURE', 11),
                 ('IMG_WB_RED', 127), ('IMG_WB_TEMP', 15001), ('IMG_ISO', 9999),
                 ('IMG_SHUTTER', -5), ('IMG_WHITE_BAL', 999), ('OSD_CROSS', 2)]
        wrong = []
        for name, value in cases:
            result = self.mav.write(name, value)
            if result == M.PARAM_ACK_ACCEPTED:
                wrong.append(f'{name}={value} accepted')
        result = self.mav.write('IMG_BRIGHTNESS', 50, kind=M.MAV_PARAM_EXT_TYPE_REAL32)
        if result == M.PARAM_ACK_ACCEPTED:
            wrong.append('IMG_BRIGHTNESS as REAL32 accepted')
        after = self.web.form()
        changed = {k: (before[k], after[k]) for k in before if k != 'csrf' and after.get(k) != before[k]}
        if changed:
            wrong.append(f'settings changed: {changed}')
        if wrong:
            return FAIL, '; '.join(wrong)
        return PASS, f'{len(cases) + 1} bad writes refused, nothing changed'

    def capture(self, item):
        before = self.photos()
        # CAMERA_IMAGE_CAPTURED comes before the COMMAND_ACK
        self.mav.drain()
        self.mav.send('command_long', self.mav.system, CAMERA, M.MAV_CMD_IMAGE_START_CAPTURE, 0,
                      0, 0, 1, 0, 0, 0, 0)
        got = {}
        deadline = time.monotonic() + 20
        while len(got) < 2 and time.monotonic() < deadline:
            try:
                message = self.mav.receive(['COMMAND_ACK', 'CAMERA_IMAGE_CAPTURED'],
                                           lambda m: m.get_type() != 'COMMAND_ACK' or
                                           m.command == M.MAV_CMD_IMAGE_START_CAPTURE,
                                           timeout=deadline - time.monotonic())
            except TimeoutError:
                break
            got[message.get_type()] = message
        if 'COMMAND_ACK' not in got or got['COMMAND_ACK'].result != M.MAV_RESULT_ACCEPTED:
            return FAIL, f'IMAGE_START_CAPTURE answered {got.get("COMMAND_ACK")}'
        if 'CAMERA_IMAGE_CAPTURED' not in got:
            return FAIL, 'no CAMERA_IMAGE_CAPTURED'
        captured = got['CAMERA_IMAGE_CAPTURED']
        time.sleep(1)
        new = sorted(self.photos() - before)
        text = (f'CAMERA_IMAGE_CAPTURED index {captured.image_index} result {captured.capture_result} '
                f'{captured.file_url or "(no url)"}; new on card: '
                f'{urllib.parse.unquote(new[-1]) if new else "none"}')
        return (PASS if captured.capture_result == 1 and new else FAIL), text

    def one_push(self, item):
        names = ['white_balance', 'wb_red', 'wb_green', 'wb_blue']
        try:
            self.save(item['context'])
            time.sleep(2.5)
            before = measure(grab(self.host, 'sub', attempts=5))
            option = self.mav.parameters['CAM_WB_ONEPUSH']['options']
            water = next(v for k, v in option.items() if 'green' in k.lower())
            result = self.mav.write('CAM_WB_ONEPUSH', water)
            if result != M.PARAM_ACK_ACCEPTED:
                return FAIL, f'CAM_WB_ONEPUSH={water}: result {result}'
            status = self.mav.receive('STATUSTEXT', lambda m: 'white balance' in m.text.lower(), 60)
            time.sleep(2.5)
            after = measure(grab(self.host, 'sub', attempts=5))
            form = self.web.form()
            read = {n: self.mav.read(n) for n in ('IMG_WHITE_BAL', 'IMG_WB_RED', 'IMG_WB_BLUE')}
        finally:
            self.restore(names)
        manual = self.mav.parameters['IMG_WHITE_BAL']['options']['manual']
        gains = '/'.join(form[n] for n in names[1:])
        summary = (f'cast {before["cast"]:.3f} -> {after["cast"]:.3f}, saved {form["white_balance"]} '
                   f'R/G/B {gains}, MAVLink {read} ({status.text})')
        ok = (form['white_balance'] == 'manual' and read['IMG_WHITE_BAL'] == manual and
              read['IMG_WB_RED'] == int(form['wb_red']) and after['cast'] < 0.7 * before['cast'])
        return (PASS if ok else FAIL), summary


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split('\n\n', 1)[1])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    run = sub.add_parser('run')
    run.add_argument('checks', nargs='*')
    run.add_argument('--save-dir')
    args = parser.parse_args()

    if args.command == 'list':
        for item in CHECKS:
            print(f'{item["name"]:20} {item["parameter"] or "":16} {", ".join(item["values"]):36} '
                  f'{item["note"]}')
        return 0

    unknown = set(args.checks) - {c['name'] for c in CHECKS}
    if unknown:
        sys.exit(f'unknown checks: {", ".join(sorted(unknown))}; see "list"')
    selected = [c for c in CHECKS if not args.checks or c['name'] in args.checks]
    if selected[0]['name'] != 'definition':
        selected.insert(0, CHECKS[0])
    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)
    web = CameraWeb()
    mav = CameraLink(web.host, int(os.environ.get('CAM_MAV_PORT', '14550')),
                     int(os.environ.get('CAM_MAV_SYSID', '1')))
    runner = MavlinkRunner(web, mav, args.save_dir)
    counts = {PASS: 0, FAIL: 0, UNSURE: 0}
    try:
        mav.wait_camera(timeout=15)
        for item in selected:
            start = time.time()
            try:
                state, text = runner.run(item)
            except Exception as error:  # report it and go on to the next check
                state, text = FAIL, f'{type(error).__name__}: {error}'
            counts[state] += 1
            print(f'{state:6} {item["name"]:20} {text}  [{time.time() - start:.0f} s]', flush=True)
            if item['name'] == 'definition' and not mav.parameters:
                break
    finally:
        left = runner.verify_restored()
        if left:
            print('NOT RESTORED:', left)
            runner.save({k: v for k, (v, _) in left.items()})
        mav.close()
        web.save({}, restart=True)
        runner.wait_stream('sub')
    print(f'{counts[PASS]} passed, {counts[FAIL]} failed, {counts[UNSURE]} unsure')
    return 1 if counts[FAIL] else 0


if __name__ == '__main__':
    sys.exit(main())
