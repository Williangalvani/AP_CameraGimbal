#!/usr/bin/env python3
"""Checks a camera running this firmware the way MAVLink Camera Manager and
4K Cam Manager talk to it, and saves its ONVIF replies for onvif-check.

camera.ini and the web's camera.ini.web.bak are backed up over telnet
(tools/camsh.py) before the first change and restored with their modes at the
end, then camera-app is restarted through /action/restart.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
BACKUP = '/run/4kcam-check.ini'
BACKUP_WEB_BAK = '/run/4kcam-check.ini.web.bak'
FIXTURE_MESSAGE_ID = 'uuid:00000000-0000-4000-8000-000000000000'

U8 = range(256)
U16 = range(65536)
IMAGE_TYPES = {
    'brightness': U8, 'contrast': U8, 'saturation': U8, 'antiFog': {0, 1},
    'frameTurbo_pro': {0, 1, 2}, 'AE_strategy_mode': {0, 1}, 'auto_exposureEx': {0, 1},
    'exposure_time': {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464},
    'auto_awb': {0, 1}, 'awb_red': U8, 'awb_green': U8, 'awb_blue': U8, 'awb_auto_mode': {0, 1},
    'rotate': {0, 1, 2, 3},
}
IMAGE_EX_TYPES = {'mirror': {0, 1}, 'flip': {0, 1}, 'onceAWB': U8}
VENC_TYPES = {
    'channel': {0, 1, 2}, 'encode_profile': {0, 1, 2}, 'encode_type': {1, 5}, 'pic_width': U16,
    'pic_height': U16, 'rc_mode': {0, 1}, 'bitrate': U16, 'max_framerate': U16, 'frame_rate': U16, 'gop': U16,
}
IDENTITY = ('code', 'device_mac', 'device_id', 'device_ip', 'log', 'sign_tby')


class Check:
    def __init__(self):
        self.failures = 0

    def __call__(self, ok, what):
        print(('PASS ' if ok else 'FAIL ') + what, flush=True)
        if not ok:
            self.failures += 1
        return ok


def camsh(args, command, timeout=30):
    result = subprocess.run([sys.executable, args.camsh, '-t', str(timeout), command],
                            check=True, capture_output=True, text=True)
    return result.stdout


def post(url, body, content_type, origin=None):
    headers = {'Content-Type': content_type}
    if origin is not None:
        headers['Origin'] = origin
    request = urllib.request.Request(url, data=body.encode(), method='POST', headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as reply:
            return reply.status, reply.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def get(url):
    try:
        with urllib.request.urlopen(url, timeout=30) as reply:
            return reply.status, reply.read().decode(errors='replace')
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode(errors='replace')


def is_action_reply(text):
    try:
        return 'device_mac' in json.loads(text)
    except (ValueError, TypeError):
        return False


def action(args, name, fields=None, origin=None):
    body = json.dumps(fields) if fields is not None else ''
    status, text = post(f'http://{args.camera}/action/{name}', body, 'application/json', origin)
    return status, (json.loads(text) if status == 200 else None)


def bad_types(reply, types):
    return [key for key, allowed in types.items()
            if key in reply and not (isinstance(reply[key], int) and reply[key] in allowed)]


def username_token(password, user='admin'):
    nonce = os.urandom(16)
    created = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000Z')
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + password.encode()).digest()).decode()
    return ('<s:Header><wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd" '
            'xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"><wsse:UsernameToken>'
            f'<wsse:Username>{user}</wsse:Username>'
            '<wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">'
            f'{digest}</wsse:Password>'
            '<wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">'
            f'{base64.b64encode(nonce).decode()}</wsse:Nonce><wsu:Created>{created}</wsu:Created>'
            '</wsse:UsernameToken></wsse:Security></s:Header>')


def soap(args, operation, password, origin=None):
    header = username_token(password) if password is not None else ''
    envelope = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" '
                f'xmlns:tt="http://www.onvif.org/ver10/schema">{header}<s:Body>{operation}</s:Body></s:Envelope>')
    return post(f'http://{args.camera}/onvif/device_service', envelope, 'application/soap+xml; charset=utf-8', origin)


def rtsp_describe(host, port, path):
    try:
        with socket.create_connection((host, port), timeout=5) as sock:
            sock.sendall(f'DESCRIBE rtsp://{host}:{port}/{path} RTSP/1.0\r\nCSeq: 1\r\n'
                         'Accept: application/sdp\r\n\r\n'.encode())
            return sock.recv(4096).decode(errors='replace')
    except OSError as error:
        return str(error)


def save(args, kind, text):
    with open(os.path.join(args.out, kind + '.xml'), 'w') as file:
        file.write(text)


def check_onvif(args, check):
    status, text = soap(args, '<tds:GetSystemDateAndTime/>', None)
    check(status == 200 and 'UTCDateTime' in text, 'GetSystemDateAndTime without a login')
    status, text = soap(args, '<tds:GetSystemDateAndTime/>', None, origin='http://example.com')
    check(status == 403 and 'UTCDateTime' not in text, f'GetSystemDateAndTime from a browser (Origin) is 403 ({status})')
    status, text = soap(args, '<tds:GetDeviceInformation/>', None)
    check(status == 400 and 'ter:NotAuthorized' in text, 'GetDeviceInformation without a login is NotAuthorized')
    save(args, 'not-authorized', text)
    status, text = soap(args, '<tds:GetDeviceInformation/>', 'wrong')
    check(status == 400 and 'ter:NotAuthorized' in text, 'GetDeviceInformation with a wrong password is NotAuthorized')
    status, text = soap(args, '<tds:GetDeviceInformation/>', args.password)
    check(status == 200 and '<tds:Manufacturer>RadCam</tds:Manufacturer><tds:Model>HDIPC</tds:Model>' in text
          and '<tds:HardwareId>IPCamera</tds:HardwareId>' in text, 'GetDeviceInformation')
    save(args, 'device-information', text)
    status, text = soap(args, '<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>',
                        args.password)
    check(status == 200 and f'<tds:XAddr>http://{args.camera}/onvif/device_service</tds:XAddr>' in text
          and 'ver10/media/wsdl' in text, 'GetServices')
    save(args, 'services', text)
    status, text = soap(args, '<trt:GetProfiles/>', args.password)
    check(status == 200 and 'token="main"' in text and 'token="sub"' in text, 'GetProfiles')
    save(args, 'profiles', text)
    for token, path in (('main', 'stream_0'), ('sub', 'stream_1')):
        status, text = soap(args, '<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>'
                                  '<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>'
                                  f'<trt:ProfileToken>{token}</trt:ProfileToken></trt:GetStreamUri>', args.password)
        check(status == 200 and f'<tt:Uri>rtsp://{args.camera}:8554/{path}</tt:Uri>' in text, f'GetStreamUri {token}')
        if token == 'main':
            save(args, 'stream-uri', text)
        check(rtsp_describe(args.camera, 8554, path).startswith('RTSP/1.0 200'), f'RTSP serves /{path}')
    status, text = soap(args, '<tds:GetCapabilities/>', args.password)
    check(status == 400 and 'ter:ActionNotSupported' in text, 'an unsupported operation is ActionNotSupported')


def round_trip(args, check, name, fields, getter, getter_fields, expect):
    status, reply = action(args, name, fields)
    _, after = action(args, getter, getter_fields)
    got = {key: (after or {}).get(key) for key in expect}
    check(status == 200 and reply['code'] == 0 and got == expect, f'{name} {fields} reads back {expect}, got {got}')


def check_actions(args, check):
    status, reply = action(args, 'getSysConfig')
    check(status == 200 and reply['code'] == 0 and all(key in reply for key in IDENTITY)
          and reply['device_ip'] == args.camera, 'getSysConfig identity')
    check(status == 200 and re.fullmatch(r'[0-9a-f]{2}(-[0-9a-f]{2}){5}', reply['device_mac'])
          and reply['device_mac'] != '00-00-00-00-00-00' and reply['device_id'].strip('0') != '',
          f"getSysConfig MAC {reply and reply['device_mac']} and ID {reply and reply['device_id']} are set")
    status, text = get(f'http://{args.camera}/action/getSysConfig')
    check(not (status == 200 and is_action_reply(text)), f'GET /action needs a login ({status})')
    status, text = post(f'http://{args.camera}/action/getSysConfig', '', 'application/json', 'http://example.com')
    check(status == 403 and not is_action_reply(text), f'/action/getSysConfig from a browser (Origin) is 403 ({status})')
    status, reply = action(args, 'noSuchAction', {})
    check(status == 200 and reply['code'] == -1, 'an unknown action is code -1')
    status, image = action(args, 'getImageAdjustment')
    check(status == 200 and image['code'] == 0 and not bad_types(image, IMAGE_TYPES),
          f'getImageAdjustment types {bad_types(image, IMAGE_TYPES)}')
    status, ex = action(args, 'getImageAdjustmentEx')
    check(status == 200 and ex['code'] == 0 and not bad_types(ex, IMAGE_EX_TYPES), 'getImageAdjustmentEx types')
    for channel in (0, 1, 2):
        status, venc = action(args, 'getVencConf', {'channel': channel})
        check(status == 200 and venc['code'] == 0 and not bad_types(venc, VENC_TYPES) and venc['pixel_list'],
              f'getVencConf channel {channel} types')

    level = 204 if image['brightness'] != 204 else 128
    status, _ = action(args, 'setImageAdjustment', {'brightness': level}, origin='http://example.com')
    _, after = action(args, 'getImageAdjustment')
    check(status == 403 and after['brightness'] == image['brightness'],
          f"setImageAdjustment from a browser (Origin) is 403 and changes nothing ({status}, {after['brightness']})")
    round_trip(args, check, 'setImageAdjustment', {'brightness': level}, 'getImageAdjustment', None, {'brightness': level})
    round_trip(args, check, 'setImageAdjustment', {'auto_awb': 1, 'awb_red': 40, 'awb_blue': 60},
               'getImageAdjustment', None, {'auto_awb': 1, 'awb_red': 40, 'awb_blue': 60})
    round_trip(args, check, 'setImageAdjustment', {'awb_auto_mode': 1}, 'getImageAdjustment', None, {'awb_auto_mode': 1})
    round_trip(args, check, 'setImageAdjustment', {'awb_auto_mode': 0}, 'getImageAdjustment', None, {'awb_auto_mode': 0})
    round_trip(args, check, 'setImageAdjustment', {'auto_exposureEx': 1, 'exposure_time': 400},
               'getImageAdjustment', None, {'auto_exposureEx': 1, 'exposure_time': 400})
    round_trip(args, check, 'setImageAdjustment', {'auto_exposureEx': 0}, 'getImageAdjustment', None, {'auto_exposureEx': 0})
    round_trip(args, check, 'setImageAdjustmentEx', {'mirror': 1, 'flip': 1}, 'getImageAdjustmentEx', None,
               {'mirror': 1, 'flip': 1})
    round_trip(args, check, 'setImageAdjustmentEx', {'mirror': 0, 'flip': 0}, 'getImageAdjustmentEx', None,
               {'mirror': 0, 'flip': 0})
    # the extension's ImageTab sends mirror or flip alone
    round_trip(args, check, 'setImageAdjustmentEx', {'mirror': 1}, 'getImageAdjustmentEx', None,
               {'mirror': 1, 'flip': 1})
    round_trip(args, check, 'setImageAdjustmentEx', {'flip': 0}, 'getImageAdjustmentEx', None,
               {'mirror': 0, 'flip': 0})
    round_trip(args, check, 'setImageAdjustmentEx', {'flip': 1}, 'getImageAdjustmentEx', None,
               {'mirror': 1, 'flip': 1})
    round_trip(args, check, 'setImageAdjustmentEx', {'mirror': 0}, 'getImageAdjustmentEx', None,
               {'mirror': 0, 'flip': 0})
    round_trip(args, check, 'setVencConf', {'channel': 0, 'bitrate': 8000}, 'getVencConf', {'channel': 0},
               {'bitrate': 8000})
    status, reply = action(args, 'setVencConf', {'channel': 2, 'bitrate': 32})
    check(status == 200 and reply['code'] == 0, 'setVencConf channel 2 is accepted')
    _, sub = action(args, 'getVencConf', {'channel': 1})
    sub_settings = {key: sub[key] for key in VENC_TYPES}
    round_trip(args, check, 'setVencConf', {'channel': 1, 'bitrate': sub['bitrate'] // 2 + 1, 'frame_rate': 15, 'gop': 15},
               'getVencConf', {'channel': 1}, sub_settings)

    # set_default restores the web's defaults (50 %, auto) and ignores the other fields
    round_trip(args, check, 'setImageAdjustment', {'brightness': 204, 'contrast': 51, 'auto_awb': 1, 'awb_red': 40,
                                                   'awb_auto_mode': 1, 'auto_exposureEx': 1, 'exposure_time': 400},
               'getImageAdjustment', None, {'brightness': 204, 'contrast': 51, 'auto_awb': 1, 'awb_auto_mode': 1,
                                            'auto_exposureEx': 1})
    round_trip(args, check, 'setImageAdjustment', {'set_default': 1, 'brightness': 10}, 'getImageAdjustment', None,
               {'brightness': 128, 'contrast': 128, 'saturation': 128, 'auto_awb': 0, 'awb_auto_mode': 0,
                'auto_exposureEx': 0})

    round_trip(args, check, 'setImageAdjustment', {'auto_awb': 0}, 'getImageAdjustment', None, {'auto_awb': 0})
    started = time.time()
    status, reply = action(args, 'setImageAdjustmentEx', {'onceAWB': 1})
    check(status == 200 and reply['code'] == 0 and time.time() - started < 2, 'onceAWB replies at once')
    args.one_push_pending = True
    deadline = time.time() + 10
    image = None
    while time.time() < deadline:
        _, image = action(args, 'getImageAdjustment')
        if image and image['auto_awb'] == 1:
            break
        time.sleep(0.5)
    if check(image is not None and image['auto_awb'] == 1, f'onceAWB leaves manual gains within 10 s: {image}'):
        args.one_push_pending = False


def wait_one_push(args, timeout=20):
    """Waits until camera-app has taken the one-push request and written its
    outcome, so a late measurement can't save its gains over the restore."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if 'finished' in camsh(args, f'[ ! -e {args.ini}.wb.command ] && [ -e {args.ini}.wb.status ] && '
                                     'echo finished || true'):
            return True
        time.sleep(1)
    return False


def probe(args, message_id=None, timeout=3.0, types=''):
    message_id = message_id or f'uuid:{uuid.uuid4()}'
    with open(os.path.join(HERE, 'fixtures', 'onvif', 'Probe.xml')) as file:
        message = file.read().replace(FIXTURE_MESSAGE_ID, message_id)
    if types:
        message = message.replace('<d:Types></d:Types>',
                                  '<d:Types xmlns:dn="http://www.onvif.org/ver10/network/wsdl" '
                                  f'xmlns:wsdp="http://schemas.xmlsoap.org/ws/2006/02/devprof">{types}</d:Types>')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.local, 0))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(args.local))
    sock.sendto(message.encode(), ('239.255.255.250', 3702))
    sock.settimeout(0.5)
    replies = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, source = sock.recvfrom(16384)
        except socket.timeout:
            continue
        if source[0] == args.camera:
            replies.append(data.decode())
    sock.close()
    return message_id, replies


def check_discovery(args, check):
    message_id, replies = probe(args)
    reply = replies[0] if len(replies) == 1 else ''
    relates = re.search(r'<a:RelatesTo>([^<]*)</a:RelatesTo>', reply)
    check(len(replies) == 1 and relates is not None and relates.group(1) == message_id,
          f'one ProbeMatch for our Probe ({len(replies)} replies)')
    check('onvif://www.onvif.org/name/hd' in reply and 'onvif://www.onvif.org/hardware/HW0100302' in reply,
          'ProbeMatch scopes name/hd and hardware/HW0100302')
    check(f'<d:XAddrs>http://{args.camera}/onvif/device_service</d:XAddrs>' in reply, 'ProbeMatch XAddrs')
    _, replies = probe(args, types='dn:NetworkVideoTransmitter')
    check(len(replies) == 1, f'one ProbeMatch for Types dn:NetworkVideoTransmitter ({len(replies)} replies)')
    _, replies = probe(args, types='wsdp:Printer')
    check(not replies, f'no ProbeMatch for Types wsdp:Printer ({len(replies)} replies)')
    # onvif-check accepts only a reply to the fixture's MessageID
    path = os.path.join(args.out, 'probe.xml')
    if os.path.exists(path):
        os.remove(path)
    _, replies = probe(args, FIXTURE_MESSAGE_ID)
    if check(len(replies) == 1, f"one ProbeMatch for the fixture's MessageID ({len(replies)} replies)"):
        save(args, 'probe', replies[0])


def check_disabled(args, check):
    compat_key = "sed -n '/^\\[compat\\]/,/^\\[/p' {0} | grep -q '^[[:space:]]*fourkcam_manager[[:space:]]*='"
    camsh(args, f"if {compat_key.format(args.ini)}; then sed -i '/^\\[compat\\]/,/^\\[/"
                f" s/^[[:space:]]*fourkcam_manager[[:space:]]*=.*/fourkcam_manager = false/' {args.ini};"
                f" elif grep -q '^\\[compat\\]' {args.ini}; then sed -i '/^\\[compat\\]/a fourkcam_manager = false' {args.ini};"
                f" else printf '\\n[compat]\\nfourkcam_manager = false\\n' >> {args.ini}; fi")
    count = camsh(args, f"sed -n '/^\\[compat\\]/,/^\\[/p' {args.ini} | grep -c '^[[:space:]]*fourkcam_manager' || true")
    check(count.strip() == '1', f'one fourkcam_manager key under [compat] ({count.strip()})')
    status, _ = post(f'http://{args.camera}/action/getSysConfig', '', 'application/json')
    check(status == 404, f'/action is 404 with Allow 4K Cam Manager off ({status})')
    status, _ = soap(args, '<tds:GetSystemDateAndTime/>', None)
    check(status == 404, f'/onvif is 404 with Allow 4K Cam Manager off ({status})')
    _, replies = probe(args)
    check(not replies, 'no ProbeMatch with Allow 4K Cam Manager off')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera', default='192.168.2.10')
    parser.add_argument('--local', default='192.168.2.15', help="this machine's address on the camera's LAN")
    parser.add_argument('--camsh', required=True, help='path to tools/camsh.py')
    parser.add_argument('--password', default='blue')
    parser.add_argument('--ini', default='/date/ap_cameragimbal/camera.ini')
    parser.add_argument('--out', default=os.path.join(HERE, 'onvif-check', 'replies'))
    parser.add_argument('--skip-discovery', action='store_true')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    check = Check()
    web_bak = f'{args.ini}.web.bak'
    camsh(args, f'cp -p {args.ini} {BACKUP} && '
                f'if [ -e {web_bak} ]; then cp -p {web_bak} {BACKUP_WEB_BAK}; else rm -f {BACKUP_WEB_BAK}; fi')
    try:
        if not args.skip_discovery:
            check_discovery(args, check)
        check_onvif(args, check)
        check_actions(args, check)
        if not args.skip_discovery:
            check_disabled(args, check)
    finally:
        # a pending one-push would save its gains over the restored file
        if getattr(args, 'one_push_pending', False):
            check(wait_one_push(args), 'a late one-push finished before the restore')
        camsh(args, f'rm -f {args.ini}.wb.command {args.ini}.wb.status && '
                    f'cp -p {BACKUP} {args.ini}.restore && mv {args.ini}.restore {args.ini} && '
                    f'if [ -e {BACKUP_WEB_BAK} ]; then cp -p {BACKUP_WEB_BAK} {web_bak}.restore && '
                    f'mv {web_bak}.restore {web_bak}; else rm -f {web_bak}; fi')
    first = []
    restarting = threading.Thread(target=lambda: first.append(action(args, 'restart', {})))
    restarting.start()
    time.sleep(1)
    started = time.time()
    status, reply = action(args, 'restart', {})
    check(status == 200 and reply['code'] == -1 and reply['log'] == 'restart in progress' and time.time() - started < 2,
          f"a restart during a restart is refused at once: {reply and reply['log']}")
    restarting.join()
    status, reply = first[0] if first else (None, None)
    check(status == 200 and reply['code'] == 0, 'restart restarts camera-app')
    status, reply = action(args, 'getSysConfig')
    check(status == 200 and reply['code'] == 0, 'getSysConfig after restart')
    print(f'{check.failures} failure(s)')
    return 1 if check.failures else 0


if __name__ == '__main__':
    sys.exit(main())
