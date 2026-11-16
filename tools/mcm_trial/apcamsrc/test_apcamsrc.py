#!/usr/bin/env python3
"""Host test of apcamsrc against build/test_feed (camera-app's frame socket
code publishing build/test.h264 at 30 fps). Run through `make test`."""
import os
import subprocess
import sys
import threading
import time

import gi

gi.require_version('Gst', '1.0')
from gi.repository import Gst  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, 'build')
SOCKET = f'@apcam-test-{os.getpid()}'
os.environ['GST_PLUGIN_PATH'] = BUILD
Gst.init(None)
failures = 0


def check(ok, what):
    global failures
    print(('PASS ' if ok else 'FAIL ') + what, flush=True)
    failures += 0 if ok else 1


class Feed:
    def __init__(self):
        self.process = subprocess.Popen(
            [os.path.join(BUILD, 'test_feed'), SOCKET, os.path.join(BUILD, 'test.h264'),
             '1280', '720', '30'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.requests = 0
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.process.stdout:
            if line.startswith('keyframe request'):
                self.requests += 1

    def stop(self):
        self.process.terminate()
        self.process.wait()


class Pipeline:
    def __init__(self):
        self.pipeline = Gst.parse_launch(
            f'apcamsrc name=src socket={SOCKET} ! h264parse ! avdec_h264 ! '
            'appsink name=sink emit-signals=true sync=false')
        self.frames = []
        self.errors = []
        self.lock = threading.Lock()
        sink = self.pipeline.get_by_name('sink')
        sink.connect('new-sample', self.on_sample)
        self.sink = sink
        src_pad = self.pipeline.get_by_name('src').get_static_pad('src')
        src_pad.add_probe(Gst.PadProbeType.BUFFER, self.on_buffer)
        self.raw = []
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.enable_sync_message_emission()
        bus.connect('sync-message::error', lambda _, m: self.errors.append(m.parse_error()))

    def running_time(self):
        clock = self.pipeline.get_clock()
        return clock.get_time() - self.pipeline.get_base_time() if clock else 0

    def on_buffer(self, pad, info):
        buffer = info.get_buffer()
        with self.lock:
            self.raw.append((buffer.pts, buffer.get_flags(), self.running_time()))
        return Gst.PadProbeReturn.OK

    def on_sample(self, sink):
        sample = sink.emit('pull-sample')
        with self.lock:
            self.frames.append(sample.get_buffer().pts)
        return Gst.FlowReturn.OK

    def caps(self):
        caps = self.pipeline.get_by_name('src').get_static_pad('src').get_current_caps()
        return caps.to_string() if caps else ''

    def start(self):
        self.pipeline.set_state(Gst.State.PLAYING)

    def stop(self):
        started = time.monotonic()
        self.pipeline.set_state(Gst.State.NULL)
        return time.monotonic() - started

    def key_unit(self):
        structure = Gst.Structure.new_empty('GstForceKeyUnit')
        structure.set_value('all-headers', True)
        self.sink.send_event(Gst.Event.new_custom(Gst.EventType.CUSTOM_UPSTREAM, structure))


def wait(predicate, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


DISCONT = Gst.BufferFlags.DISCONT
DELTA = Gst.BufferFlags.DELTA_UNIT

feed = Feed()
time.sleep(0.2)
p = Pipeline()
p.start()
check(wait(lambda: len(p.frames) >= 60, 5), 'decodes the stream')
check(p.caps().startswith('video/x-h264') and 'width=(int)1280' in p.caps() and
      'framerate=(fraction)30/1' in p.caps(), f'caps come from the socket: {p.caps()}')
with p.lock:
    raw = list(p.raw)
check(raw[0][1] & DISCONT and not raw[0][1] & DELTA, 'the first buffer is a key frame and DISCONT')
gaps = [b[0] - a[0] for a, b in zip(raw, raw[1:])]
check(all(25e6 < g < 42e6 for g in gaps), f'buffers are ~33 ms apart (min {min(gaps)/1e6:.1f}, '
      f'max {max(gaps)/1e6:.1f} ms)')
late = [arrival - pts for pts, _, arrival in raw]
check(all(-1e6 <= x < 20e6 for x in late), f'timestamps track arrival (lateness '
      f'{min(late)/1e6:.1f}..{max(late)/1e6:.1f} ms)')

src = p.pipeline.get_by_name('src')
first_ok, first = src.query_position(Gst.Format.TIME)
time.sleep(0.2)
second_ok, second = src.query_position(Gst.Format.TIME)
check(first_ok and second_ok and second > first, f'position advances ({first} -> {second})')

time.sleep(0.1)
before = feed.requests
p.key_unit()
check(wait(lambda: feed.requests > before, 1), 'a force-key-unit event reaches camera-app')

count = len(p.raw)
feed.stop()
time.sleep(1.0)
check(not p.errors, 'losing camera-app is not an error')
feed = Feed()
check(wait(lambda: len(p.raw) > count + 30, 5), 'reconnects when camera-app comes back')
with p.lock:
    resumed = p.raw[count:]
check(resumed and resumed[0][1] & DISCONT and not resumed[0][1] & DELTA,
      'the first buffer after reconnecting is a key frame and DISCONT')
check(all(b[0] > a[0] for a, b in zip(p.raw, p.raw[1:])), 'timestamps keep increasing')
check(p.stop() < 1.0, 'stops promptly while streaming')
check(not p.errors, f'no errors: {p.errors}')
feed.stop()

p = Pipeline()
p.start()
time.sleep(1.2)
check(p.stop() < 1.0, 'stops promptly while camera-app is absent')
p = Pipeline()
p.start()
time.sleep(0.7)
feed = Feed()
check(wait(lambda: len(p.frames) >= 30, 5), 'starts before camera-app does')
p.stop()
feed.stop()
sys.exit(1 if failures else 0)
