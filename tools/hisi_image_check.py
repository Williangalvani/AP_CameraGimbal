#!/usr/bin/env python3
"""Check that HiSilicon camera settings change the video as they should.

Usage:
  hisi_image_check.py list
  hisi_image_check.py stats [--main] [--save FILE.png]
  hisi_image_check.py set NAME=VALUE... [--keep] [--main] [--save FILE.png]
  hisi_image_check.py run [CHECK...] [--save-dir DIR]

stats grabs frames from the RTSP video and prints what they measure: luma
mean and percentiles, contrast, saturation, colour ratios, hue and
sharpness. set changes Parameters form fields, prints the measurements before
and after, and puts the old values back unless --keep is given. run steps each
check's setting through its values, with the other fields it needs held in a
known state, and judges the change in the frames (or the stream for format
settings); every field is restored afterwards. A check whose effect this scene
cannot show is reported UNSURE rather than failed.

Settings apply to the camera while this runs, so don't fly or record with it.
CAM_HOST selects the camera (default 192.168.2.10); CAM_WEB_USER and CAM_WEB_PW
the web login. Needs ffmpeg, ffprobe and numpy.
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.parse

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hisi_web_param import CameraWeb, notices  # noqa: E402

STREAMS = {'main': 'video1', 'sub': 'video2'}
SIZE = (320, 180)              # sub stream frames are scaled to this
SHARP_CROP = (960, 540)        # native-resolution centre of the main stream
CROSS_CROP = (480, 270)        # holds the whole centre cross on a 4K stream
# AE zones are 15 rows by 17 columns; centre metering weights rows 5-9 and
# columns 6-10, spot metering only rows 6-8 and columns 7-9.
CENTRE = ((5 / 15, 10 / 15), (6 / 17, 11 / 17))
SPOT = ((6 / 15, 9 / 15), (7 / 17, 10 / 17))


class GrabError(RuntimeError):
    pass


def rtsp(host, stream):
    return f'rtsp://{host}:8554/{STREAMS[stream]}'


def grab(host, stream='sub', frames=12, keep=4, crop=None, attempts=1):
    """Average of the last KEEP of FRAMES decoded frames, as float RGB."""
    width, height = crop or SIZE
    vf = f'crop={width}:{height}' if crop else f'scale={width}:{height}'
    command = ['ffmpeg', '-v', 'error', '-rtsp_transport', 'tcp', '-i', rtsp(host, stream),
               '-frames:v', str(frames), '-vf', vf, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-']
    size = width * height * 3
    for attempt in range(attempts):
        try:
            out = subprocess.run(command, capture_output=True, timeout=40).stdout
        except subprocess.TimeoutExpired:
            out = b''
        count = len(out) // size
        if count >= keep:
            video = np.frombuffer(out[:count * size], np.uint8).reshape(count, height, width, 3)
            return video[-keep:].astype(np.float32).mean(0)
        if attempt + 1 < attempts:
            time.sleep(2)
    raise GrabError(f'{rtsp(host, stream)}: got {count} of {frames} frames')


def region(image, rows, columns):
    h, w = image.shape[:2]
    return image[int(rows[0] * h):int(rows[1] * h), int(columns[0] * w):int(columns[1] * w)]


def measure(rgb):
    """Measurements of an RGB frame."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u, v = b - y, r - y
    green = max(g.mean(), 1e-3)
    red, blue = r.mean() / green, b.mean() / green
    p10, p25, p50, p90 = np.percentile(y, [10, 25, 50, 90])
    return {
        'luma': float(y.mean()), 'p10': float(p10), 'p25': float(p25), 'p50': float(p50),
        'p90': float(p90), 'contrast': float(y.std()),
        'saturation': float(np.hypot(u, v).mean()),
        'red': float(red), 'blue': float(blue),
        'green': float(2 * g.mean() / max(r.mean() + b.mean(), 1e-3)),
        'cast': float(math.hypot(math.log(max(red, 1e-3)), math.log(max(blue, 1e-3)))),
        'hue': float(math.degrees(math.atan2(v.mean(), u.mean()))),
        'centre': float(region(y, *CENTRE).mean()), 'spot': float(region(y, *SPOT).mean()),
    }


def sharpness(host):
    """Mean absolute Laplacian of the main stream's centre at full resolution."""
    rgb = grab(host, 'main', frames=8, keep=3, crop=SHARP_CROP, attempts=3)
    y = rgb @ np.array([0.299, 0.587, 0.114], np.float32)
    lap = 4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]
    return float(np.abs(lap).mean())


def probe(host, stream, seconds=5):
    """Codec, size, frame rate, bitrate and keyframe spacing (s) the RTSP stream delivers.
    The spacing is the median between keyframes, so the one the RTSP PLAY asks for
    counts for little; it is 0 with fewer than three keyframes."""
    url = rtsp(host, stream)
    info = json.loads(subprocess.run(
        ['ffprobe', '-v', 'error', '-rtsp_transport', 'tcp', '-select_streams', 'v:0',
         '-show_entries', 'stream=codec_name,width,height', '-of', 'json', url],
        capture_output=True, text=True, timeout=30).stdout or '{}').get('streams', [{}])[0]
    packets = json.loads(subprocess.run(
        ['ffprobe', '-v', 'error', '-rtsp_transport', 'tcp', '-select_streams', 'v:0',
         '-show_entries', 'packet=pts_time,size,flags', '-of', 'json', '-read_intervals',
         f'%+{seconds}', url], capture_output=True, text=True, timeout=seconds + 30).stdout
        or '{}').get('packets', [])
    times = [float(p['pts_time']) for p in packets if 'pts_time' in p]
    span = times[-1] - times[0] if len(times) > 1 else 0
    if not info or span <= 0:
        raise GrabError(f'{url}: no stream')
    keys = [float(p['pts_time']) for p in packets if 'pts_time' in p and 'K' in p.get('flags', '')]
    gaps = sorted(b - a for a, b in zip(keys, keys[1:]))
    return {'codec': info.get('codec_name'), 'size': f'{info.get("width")}x{info.get("height")}',
            'fps': (len(times) - 1) / span,
            'kbps': sum(int(p['size']) for p in packets) * 8 / span / 1000,
            'keyframe_s': gaps[len(gaps) // 2] if len(gaps) >= 2 else 0.0}


def save_png(path, images):
    """Write IMAGES side by side as one PNG."""
    height = max(i.shape[0] for i in images)
    strip = np.concatenate([np.pad(i, ((0, height - i.shape[0]), (0, 4), (0, 0)))
                            for i in images], axis=1)
    strip = np.clip(strip, 0, 255).astype(np.uint8)
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                    '-s', f'{strip.shape[1]}x{strip.shape[0]}', '-i', '-', path],
                   input=strip.tobytes(), check=True)


def format_stats(stats):
    return ' '.join(f'{k}={v:.3f}' if abs(v) < 10 else f'{k}={v:.1f}' for k, v in stats.items())


# ---- verdicts: each takes the measurements per value and returns (state, text)

PASS, FAIL, UNSURE = 'PASS', 'FAIL', 'UNSURE'


def trend(metric, direction, minimum, relative=True, each=None):
    """METRIC moves in DIRECTION at every step (ties allowed, as when the
    picture clips, unless EACH sets a relative minimum per step) and by at
    least MINIMUM overall."""
    def verdict(results):
        xs = [r[metric] for r in results]
        scale = max(abs(x) for x in xs) or 1.0
        slack = -(0.01 * scale + 0.3) if each is None else None
        against = [i for i, (a, b) in enumerate(zip(xs, xs[1:]))
                   if (b - a) * direction < (slack if slack is not None else each * abs(a))]
        total = (xs[-1] - xs[0]) * direction
        # relative to the start, but not to a black or near-zero start
        need = minimum * max(abs(xs[0]), 0.05 * scale) if relative else minimum
        text = f'{metric} ' + ' -> '.join(f'{x:.3g}' for x in xs)
        if against:
            return FAIL, text + f' ({"wrong way" if each is None else "too little"} after value {against[0] + 1})'
        if total < need:
            return FAIL, text + f' (needs {"+" if direction > 0 else "-"}{need:.3g})'
        return PASS, text
    return verdict


def steady(metric, tolerance=0.03):
    """METRIC stays within TOLERANCE (relative) across the values."""
    def verdict(results):
        xs = [r[metric] for r in results]
        spread = max(xs) - min(xs)
        text = f'{metric} ' + ' -> '.join(f'{x:.3g}' for x in xs)
        return (PASS if spread <= tolerance * max(max(abs(x) for x in xs), 5.0) else FAIL), text
    return verdict


def colour_changes(minimum):
    def verdict(results):
        a, b = results[0], results[-1]
        change = math.hypot(math.log(b['red'] / a['red']), math.log(b['blue'] / a['blue']))
        text = (f'red {a["red"]:.3f} -> {b["red"]:.3f}, blue {a["blue"]:.3f} -> {b["blue"]:.3f} '
                f'(shift {change:.3f})')
        return (PASS if change >= minimum else FAIL), text
    return verdict


def hue_turns(minimum_degrees):
    def verdict(results):
        angles = np.degrees(np.unwrap(np.radians([r['hue'] for r in results])))
        steps = np.diff(angles)
        text = 'hue ' + ' -> '.join(f'{a:.1f}' for a in angles) + ' degrees'
        if len(steps) and not (np.all(steps > 0) or np.all(steps < 0)):
            return FAIL, text + ' (not one direction)'
        return (PASS if abs(angles[-1] - angles[0]) >= minimum_degrees else FAIL), text
    return verdict


def gamma_curves(results):
    """gamma 20, 70, 40, 230 select curves 0, 2, 1 and keep 1: shadows rise
    0 < 2 < 1, and 224-255 changes nothing."""
    xs = [r['p25'] for r in results]
    text = 'p25 ' + ' -> '.join(f'{x:.1f}' for x in xs)
    ok = xs[0] < xs[1] < xs[2] and abs(xs[3] - xs[2]) <= 0.02 * xs[2] + 0.3 and xs[2] > 1.03 * xs[0]
    return (PASS if ok else FAIL), text


def metering(zone):
    """Weighting ZONE makes AE expose for it: darker if the zone is clearly
    brighter than the frame, brighter if darker. AE meters before gamma, so
    when the zone is close to the frame average only a change is required."""
    def verdict(results):
        a, b = results
        ratio = a[zone] / max(a['luma'], 1e-3)
        change = (b['luma'] - a['luma']) / max(a['luma'], 5.0)
        text = (f'{zone} {a[zone]:.1f} vs frame {a["luma"]:.1f}; '
                f'luma {a["luma"]:.1f} -> {b["luma"]:.1f} ({change:+.0%})')
        if 0.85 < ratio < 1.15:
            if abs(change) >= 0.1:
                return PASS, text
            return UNSURE, text + ' (zone too close to the frame average to show)'
        wanted = -1 if ratio > 1 else 1
        return (PASS if change * wanted >= 0.03 else FAIL), text
    return verdict


def visible_or_unsure(metric, minimum, why):
    """A setting whose effect depends on the scene: a change is a pass,
    none is UNSURE."""
    def verdict(results):
        xs = [r[metric] for r in results]
        text = f'{metric} ' + ' -> '.join(f'{x:.3g}' for x in xs)
        change = abs(xs[-1] - xs[0]) / max(abs(xs[0]), 5.0)
        return (PASS, text) if change >= minimum else (UNSURE, text + f' ({why})')
    return verdict


def cross(results):
    a, b = results[0]['frame'], results[1]['frame']
    diff = np.abs(b - a).mean(axis=2)
    h, w = diff.shape
    centre = diff[int(0.3 * h):int(0.7 * h), int(0.3 * w):int(0.7 * w)]
    lines = float(np.percentile(centre, 99))
    elsewhere = float(np.concatenate([diff[:int(0.2 * h)].ravel(), diff[int(0.8 * h):].ravel()]).mean())
    text = f'centre change p99 {lines:.1f}, edges {elsewhere:.1f}'
    return (PASS if lines >= 20 and elsewhere < 5 else FAIL), text


def inverted(results):
    a, b = results[0]['frame'].mean(axis=2), results[1]['frame'].mean(axis=2)

    def corr(x, y):
        x, y = x - x.mean(), y - y.mean()
        return float((x * y).sum() / math.sqrt((x * x).sum() * (y * y).sum()))
    same, turned = corr(a, b), corr(a[::-1, ::-1], b)
    text = f'correlation upright {same:.2f}, rotated 180 {turned:.2f}'
    return (PASS if turned > 0.6 and turned > same + 0.2 else FAIL), text


def stream_is(key, values):
    def verdict(results):
        got = [r[key] for r in results]
        ok = all(abs(g - v) <= 0.1 * v if isinstance(v, float) else g == v
                 for g, v in zip(got, values))
        text = f'{key} ' + ' -> '.join(f'{g:.1f}' if isinstance(g, float) else str(g) for g in got)
        return (PASS if ok else FAIL), text
    return verdict


# ---- the checks

AUTO_WB = {'white_balance': 'auto'}
AE = {'iso': 'auto', 'shutter': 'auto', 'gain_mode': 'auto', 'exposure_compensation': '0',
      'backlight': '50', 'metering': 'average', 'max_exposure': '25', 'slow_shutter': 'false',
      'max_gain': '12', 'max_again': '32', 'max_dgain': '0'}
MANUAL_GAINS = dict(AE, shutter='1/2000', gain_mode='manual', manual_again='true', again='4',
                    manual_dgain='false', dgain='4')
NEUTRAL_MANUAL = {'white_balance': 'manual', 'wb_red': '469', 'wb_green': '256', 'wb_blue': '514'}
SHUTTERS = ['1/100000', '1/10000', '1/8000', '1/4000', '1/2000', '1/1000', '1/800', '1/750',
            '1/500', '1/400', '1/250', '1/200', '1/100', '1/60', '1/50', '1/30', '1/25', '1/12']


def check(name, setting, values, verdict, context=None, settle=2.5, kind='image',
          restart=False, note=''):
    return {'name': name, 'setting': setting, 'values': values, 'verdict': verdict,
            'context': context or {}, 'settle': settle, 'kind': kind, 'restart': restart,
            'note': note}


CHECKS = [
    check('brightness', 'brightness', ['30', '70'], trend('luma', 1, 0.15), AUTO_WB),
    check('contrast', 'contrast', ['20', '80'], trend('contrast', 1, 0.15), AUTO_WB),
    check('saturation', 'saturation', ['20', '80'], trend('saturation', 1, 0.3), AUTO_WB),
    check('hue', 'hue', ['64', '128', '192'], hue_turns(10), AUTO_WB),
    check('sharpness', 'sharpness', ['0', '255'], trend('sharpness', 1, 0.15), AUTO_WB,
          kind='sharp'),
    check('gamma', 'gamma', ['20', '70', '40', '230'], gamma_curves, AUTO_WB,
          note='20/70/40 select curves 0/2/1; 224-255 keeps the curve'),
    check('white_balance', 'white_balance',
          ['incandescent', 'fluorescent', 'daylight', 'cloudy'], trend('blue', -1, 0.1),
          note='presets 2800/4000/5500/6500 K: less blue added as the light warms less'),
    check('white_balance_auto', 'white_balance', ['manual', 'auto'], colour_changes(0.1),
          {'wb_red': '256', 'wb_green': '256', 'wb_blue': '256'}),
    check('wb_temp', 'wb_temp', ['2500', '4000', '6500', '10000'], trend('blue', -1, 0.1),
          {'white_balance': 'temperature'}),
    check('wb_red', 'wb_red', ['300', '600'], trend('red', 1, 0.2), NEUTRAL_MANUAL),
    check('wb_green', 'wb_green', ['256', '400'], trend('green', 1, 0.15), NEUTRAL_MANUAL),
    check('wb_blue', 'wb_blue', ['300', '600'], trend('blue', 1, 0.2), NEUTRAL_MANUAL),
    check('wb_water', 'wb_water', ['green', 'blue'], colour_changes(0.02), AUTO_WB,
          note='calibration and colour matrices for green or blue water'),
    check('wb_style_red', 'wb_style_red', ['0', '255'], trend('red', 1, 0.15), AUTO_WB),
    check('wb_style_green', 'wb_style_green', ['0', '255'], trend('green', 1, 0.15), AUTO_WB),
    check('wb_style_blue', 'wb_style_blue', ['0', '255'], trend('blue', 1, 0.15), AUTO_WB),
    check('exposure_compensation', 'exposure_compensation', ['-6', '0', '6'],
          trend('luma', 1, 0.15, each=0.05), AE, settle=4),
    check('backlight', 'backlight', ['30', '90'], trend('luma', 1, 0.15), AE, settle=4,
          note='the AE target luminance'),
    check('metering_center', 'metering', ['average', 'center'], metering('centre'), AE, settle=4),
    check('metering_spot', 'metering', ['average', 'spot'], metering('spot'), AE, settle=4),
    check('iso', 'iso', ['100', '200', '400', '800', '1600', '3200'], trend('luma', 1, 0.3),
          dict(AE, shutter='1/250'), settle=3),
    check('shutter', 'shutter', SHUTTERS, trend('luma', 1, 0.3), dict(AE, iso='400'), settle=3,
          note='in order of exposure time; above 1/30 the frame time limits it'),
    check('max_exposure', 'max_exposure', ['8000', '1000', '100', '25'], trend('luma', 1, 0.3),
          dict(AE, max_gain='0', max_again='0'), settle=4, note='gains held at 1x'),
    check('max_again', 'max_again', ['0', '4', '16'], trend('luma', 1, 0.3),
          dict(AE, shutter='1/4000', max_gain='255'), settle=4),
    check('max_gain', 'max_gain', ['0', '1', '4'], trend('luma', 1, 0.3),
          dict(AE, shutter='1/4000', max_again='255'), settle=4),
    check('max_dgain', 'max_dgain', ['0', '4', '32'], trend('luma', 1, 0.3),
          dict(AE, shutter='1/4000', max_again='0', max_gain='255'), settle=4),
    check('again', 'again', ['4', '16', '64'], trend('luma', 1, 0.3), MANUAL_GAINS, settle=3,
          note='manual analogue gain 1x, 4x, 16x'),
    check('dgain', 'dgain', ['4', '16', '64'], trend('luma', 1, 0.3),
          dict(MANUAL_GAINS, manual_dgain='true'), settle=3),
    check('manual_again', 'again', ['4', '64'], steady('luma', 0.1),
          dict(MANUAL_GAINS, manual_again='false'), settle=3,
          note='with manual_again off the again value is ignored'),
    check('manual_dgain', 'dgain', ['4', '64'], steady('luma', 0.1), MANUAL_GAINS, settle=3,
          note='with manual_dgain off the dgain value is ignored'),
    check('gain_mode', 'gain_mode', ['auto', 'manual'], trend('luma', -1, 0.2),
          dict(MANUAL_GAINS, manual_dgain='true'), settle=4,
          note='manual 1x gains are darker than AE gain at 1/2000'),
    check('ae_strategy', 'ae_strategy', ['highlight', 'lowlight'],
          visible_or_unsure('luma', 0.03, 'needs a scene with clipped highlights'), AE, settle=4),
    check('slow_shutter', 'slow_shutter', ['false', 'true'],
          visible_or_unsure('luma', 0.05, 'needs a scene too dark for 1/30 at the gain limit'),
          dict(AE, max_gain='0', max_again='0', max_exposure='12'), settle=5),
    check('osd_cross', 'osd_cross', ['false', 'true'], cross, AUTO_WB, kind='centre',
          note='looked for in the centre of the main stream'),
    check('orientation', 'orientation', ['upright', 'inverted'], inverted, AUTO_WB,
          restart=True, note='applies when camera-app starts'),
    check('main_resolution', 'main_resolution', ['3840x2160', '1920x1080'],
          stream_is('size', ['3840x2160', '1920x1080']), kind='main'),
    check('main_codec', 'main_codec', ['h264', 'h265'], stream_is('codec', ['h264', 'hevc']),
          kind='main'),
    check('main_frame_rate', 'main_frame_rate', ['30', '15'], stream_is('fps', [30.0, 15.0]),
          kind='main', restart=True, note='applies when camera-app starts'),
    check('main_bitrate', 'main_bitrate', ['4000', '16000'], trend('kbps', 1, 1.0), kind='main',
          note='measured over 5 s; a still scene may not fill the larger budget'),
    check('main_keyframe_interval', 'main_keyframe_interval', ['500', '2000'],
          stream_is('keyframe_s', [0.5, 2.0]), kind='main', note='median keyframe spacing over 5 s'),
    check('sub_resolution', 'sub_resolution', ['1280x720', '1920x1080'],
          stream_is('size', ['1280x720', '1920x1080']), kind='sub'),
    check('sub_codec', 'sub_codec', ['h264', 'h265'], stream_is('codec', ['h264', 'hevc']),
          kind='sub'),
    check('sub_keyframe_interval', 'sub_keyframe_interval', ['500', '2000'],
          stream_is('keyframe_s', [0.5, 2.0]), kind='sub', note='median keyframe spacing over 5 s'),
    check('capture', None, [], None, kind='capture', note='Sensors: take a photo'),
    check('one_push_white_balance', None, [], None, dict(NEUTRAL_MANUAL, wb_red='256',
          wb_blue='256'), kind='one_push', note='Sensors: one-push white balance, green water'),
]


class Runner:
    def __init__(self, web, save_dir=None):
        self.web = web
        self.host = web.host
        self.save_dir = save_dir
        self.original = web.form()
        self.original.pop('csrf')

    def save(self, changes, restart=False):
        messages = self.web.save(changes, restart)
        text = ' '.join(messages)
        if 'waits until recording stops' in text:
            raise RuntimeError('a recording is running; stop it first')
        if re.search(r'fail|could not|error', text, re.I):
            raise RuntimeError(f'camera refused {changes}: {text}')
        return text

    def restore(self, names, restart=False):
        changes = {n: self.original[n] for n in names}
        if changes:
            self.save(changes, restart)
            if restart:
                self.wait_stream('sub')

    def wait_stream(self, stream, timeout=60):
        deadline = time.time() + timeout
        while True:
            try:
                return grab(self.host, stream, frames=3, keep=1)
            except GrabError:
                if time.time() > deadline:
                    raise
                time.sleep(2)

    def measure_value(self, item):
        kind = item['kind']
        if kind in ('main', 'sub'):
            return probe(self.host, kind)
        if kind == 'sharp':
            return {'sharpness': sharpness(self.host)}
        if kind == 'centre':
            return {'frame': grab(self.host, 'main', frames=8, keep=3, crop=CROSS_CROP, attempts=3)}
        frame = grab(self.host, 'sub', attempts=5)
        return dict(measure(frame), frame=frame)

    def run(self, item):
        if item['kind'] == 'capture':
            return self.capture()
        if item['kind'] == 'one_push':
            return self.one_push(item)
        touched = set(item['context']) | {item['setting']}
        results = []
        try:
            for index, value in enumerate(item['values']):
                changes = dict(item['context']) if index == 0 else {}
                changes[item['setting']] = value
                self.save(changes, item['restart'])
                if item['restart']:
                    self.wait_stream('sub')
                if item['kind'] in ('main', 'sub'):
                    time.sleep(3)
                    self.wait_stream(item['kind'])
                time.sleep(item['settle'])
                results.append(self.measure_value(item))
        finally:
            self.restore(sorted(touched), item['restart'])
        if self.save_dir and results and 'frame' in results[0]:
            save_png(os.path.join(self.save_dir, f'{item["name"]}.png'), [r['frame'] for r in results])
        return item['verdict'](results)

    def photos(self):
        found = set()
        listing = self.web.get('/files?path=' + urllib.parse.quote('/mnt/DCIM/capture'))
        for folder in set(re.findall(r'href="/files\?path=(%2Fmnt%2FDCIM%2Fcapture%2F[^"&]+)"', listing)):
            page = self.web.get('/files?path=' + folder)
            found.update(re.findall(r'href="/file\?path=([^"&]+\.jpg)', page))
        return found

    def capture(self):
        before = self.photos()
        text = ' '.join(notices(self.web.post('/sensors/capture', {'csrf': self.web.csrf('/sensors')})))
        time.sleep(1)
        new = sorted(self.photos() - before)
        if not new:
            return FAIL, f'no new photo ({text})'
        path = urllib.parse.unquote(new[-1])
        data = self.web.opener.open(self.web.url + '/file?path=' + new[-1], timeout=60).read()
        info = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'stream=codec_name,width,height',
                               '-of', 'csv=p=0', '-'], input=data, capture_output=True).stdout.decode().strip()
        ok = info.startswith('mjpeg,') and len(data) > 100000
        return (PASS if ok else FAIL), f'{path}: {len(data)} bytes, {info or "not an image"}'

    def one_push(self, item):
        names = ['white_balance', 'wb_red', 'wb_green', 'wb_blue']
        try:
            self.save(item['context'])
            time.sleep(2.5)
            before = measure(grab(self.host, 'sub', attempts=5))
            text = ' '.join(notices(self.web.post('/sensors/white_balance',
                                                  {'csrf': self.web.csrf('/sensors'), 'water': '1'},
                                                  timeout=60)))
            time.sleep(2.5)
            after = measure(grab(self.host, 'sub', attempts=5))
            form = self.web.form()
        finally:
            self.restore(names)
        gains = '/'.join(form[n] for n in names[1:])
        summary = (f'cast {before["cast"]:.3f} -> {after["cast"]:.3f}, saved {form["white_balance"]} '
                   f'R/G/B {gains} ({text[:80]})')
        ok = form['white_balance'] == 'manual' and after['cast'] < 0.7 * before['cast']
        return (PASS if ok else FAIL), summary

    def verify_restored(self):
        now = self.web.form()
        now.pop('csrf')
        return {k: (v, now.get(k)) for k, v in self.original.items() if now.get(k) != v}


def parse_changes(args):
    changes = {}
    for arg in args:
        name, sep, value = arg.partition('=')
        if not sep:
            sys.exit(f'{arg}: expected NAME=VALUE')
        changes[name] = value
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split('\n\n', 1)[1])
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('list')
    stats = sub.add_parser('stats')
    stats.add_argument('--main', action='store_true', help='also measure main-stream sharpness')
    stats.add_argument('--save')
    change = sub.add_parser('set')
    change.add_argument('changes', nargs='+', metavar='NAME=VALUE')
    change.add_argument('--keep', action='store_true', help='leave the new values in place')
    change.add_argument('--main', action='store_true', help='also measure main-stream sharpness')
    change.add_argument('--restart', action='store_true', help='save and restart camera-app')
    change.add_argument('--settle', type=float, default=3.0)
    change.add_argument('--save')
    run = sub.add_parser('run')
    run.add_argument('checks', nargs='*')
    run.add_argument('--save-dir')
    args = parser.parse_args()

    if args.command == 'list':
        for item in CHECKS:
            values = ', '.join(item['values']) if item['values'] else ''
            print(f'{item["name"]:24} {values:40} {item["note"]}')
        return 0

    web = CameraWeb()
    if args.command == 'stats':
        frame = grab(web.host, 'sub', attempts=3)
        stats = measure(frame)
        if args.main:
            stats['sharpness'] = sharpness(web.host)
        print(format_stats(stats))
        if args.save:
            save_png(args.save, [frame])
        return 0

    if args.command == 'set':
        changes = parse_changes(args.changes)
        runner = Runner(web)
        frames = [grab(web.host, 'sub', attempts=3)]
        before = measure(frames[0])
        if args.main:
            before['sharpness'] = sharpness(web.host)
        try:
            print('camera:', runner.save(changes, args.restart))
            if args.restart:
                runner.wait_stream('sub')
            time.sleep(args.settle)
            frames.append(grab(web.host, 'sub', attempts=5))
            after = measure(frames[1])
            if args.main:
                after['sharpness'] = sharpness(web.host)
        finally:
            if not args.keep:
                runner.restore(changes, args.restart)
        for key in before:
            delta = after[key] - before[key]
            print(f'{key:11} {before[key]:9.3f} -> {after[key]:9.3f}  ({delta:+.3f})')
        if args.save:
            save_png(args.save, frames)
        return 0

    selected = [c for c in CHECKS if not args.checks or c['name'] in args.checks]
    unknown = set(args.checks) - {c['name'] for c in CHECKS}
    if unknown:
        sys.exit(f'unknown checks: {", ".join(sorted(unknown))}; see "list"')
    if args.save_dir:
        os.makedirs(args.save_dir, exist_ok=True)
    runner = Runner(web, args.save_dir)
    counts = {PASS: 0, FAIL: 0, UNSURE: 0}
    try:
        for item in selected:
            start = time.time()
            try:
                state, text = runner.run(item)
            except Exception as error:  # report it and go on to the next check
                state, text = FAIL, f'{type(error).__name__}: {error}'
            counts[state] += 1
            print(f'{state:6} {item["name"]:24} {text}  [{time.time() - start:.0f} s]', flush=True)
    finally:
        left = runner.verify_restored()
        if left:
            print('NOT RESTORED:', left)
            runner.save({k: v for k, (v, _) in left.items()})
    print(f'{counts[PASS]} passed, {counts[FAIL]} failed, {counts[UNSURE]} unsure')
    return 1 if counts[FAIL] else 0


if __name__ == '__main__':
    sys.exit(main())
