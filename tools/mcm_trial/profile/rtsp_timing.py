#!/usr/bin/env python3
"""Per-frame RTP timestamp and arrival gaps of an RTSP H.264 stream, read
over TCP interleaved so no packets are lost on the way.
Usage: rtsp_timing.py rtsp://HOST:PORT/PATH [SECONDS]"""
import socket
import struct
import sys
import time
from urllib.parse import urlparse

url = sys.argv[1]
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20
u = urlparse(url)
sock = socket.create_connection((u.hostname, u.port or 554))
buf = b''
cseq = 0


def request(method, target, headers=''):
    global buf, cseq
    cseq += 1
    sock.sendall(f'{method} {target} RTSP/1.0\r\nCSeq: {cseq}\r\n{headers}\r\n'.encode())
    while b'\r\n\r\n' not in buf:
        buf += sock.recv(65536)
    head, buf = buf.split(b'\r\n\r\n', 1)
    text = head.decode(errors='replace')
    length = next((int(l.split(':')[1]) for l in text.split('\r\n')
                   if l.lower().startswith('content-length')), 0)
    while len(buf) < length:
        buf += sock.recv(65536)
    body, buf = buf[:length].decode(errors='replace'), buf[length:]
    return text, body


_, sdp = request('DESCRIBE', url, 'Accept: application/sdp\r\n')
control = next((l.split(':', 1)[1].strip() for l in sdp.splitlines()
                if l.startswith('a=control:') and l.strip() != 'a=control:*'), '')
track = control if control.startswith('rtsp://') else url.rstrip('/') + '/' + control
head, _ = request('SETUP', track, 'Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n')
session = next(l.split(':', 1)[1].split(';')[0].strip() for l in head.split('\r\n')
               if l.lower().startswith('session'))
request('PLAY', url, f'Session: {session}\r\nRange: npt=0.000-\r\n')

frames = []
sizes = []
frame_bytes = 0
end = time.monotonic() + seconds
while time.monotonic() < end:
    while len(buf) < 4:
        buf += sock.recv(262144)
    if buf[0] != 0x24:
        buf = buf[buf.find(b'$'):] if b'$' in buf else b''
        continue
    channel, size = buf[1], struct.unpack('>H', buf[2:4])[0]
    while len(buf) < 4 + size:
        buf += sock.recv(262144)
    packet, buf = buf[4:4 + size], buf[4 + size:]
    if channel != 0 or len(packet) <= 12:
        continue
    frame_bytes += len(packet)
    nal = packet[12] & 0x1f
    if nal == 5 or (nal == 28 and len(packet) > 13 and packet[13] & 0x1f == 5) or nal in (7, 8):
        frame_idr = True
    if packet[1] & 0x80:
        idr = 'frame_idr' in globals() and frame_idr
        frames.append((struct.unpack('>I', packet[4:8])[0], time.monotonic(), idr))
        sizes.append((frame_bytes, idr))
        frame_bytes, frame_idr = 0, False

rtp = [(b[0] - a[0]) / 90 for a, b in zip(frames, frames[1:])]
arrival = [(b[1] - a[1]) * 1000 for a, b in zip(frames, frames[1:])]
print(f'{len(frames)} frames in {seconds:.0f} s ({len(frames) / seconds:.2f} fps)')
for name, gaps in (('rtp', rtp), ('arrival', arrival)):
    s = sorted(gaps)
    print(f'{name:8s} gap ms: p50 {s[len(s) // 2]:.0f}  p99 {s[int(len(s) * 0.99)]:.0f}  '
          f'max {s[-1]:.0f}  >150 ms: {sum(g > 150 for g in gaps)}  >66 ms: {sum(g > 66 for g in gaps)}')
idr_sizes = [s for s, k in sizes if k]
p_sizes = sorted(s for s, k in sizes if not k)
print(f'keyframes: {len(idr_sizes)}, size KB avg {sum(idr_sizes) / max(len(idr_sizes), 1) / 1024:.0f} '
      f'max {max(idr_sizes, default=0) / 1024:.0f}; other frames KB p50 '
      f'{p_sizes[len(p_sizes) // 2] / 1024 if p_sizes else 0:.0f} max {p_sizes[-1] / 1024 if p_sizes else 0:.0f}; '
      f'{sum(s for s, _ in sizes) * 8 / seconds / 1e6:.1f} Mbit/s')
big = [(r, a, frames[i + 1][2]) for i, (r, a) in enumerate(zip(rtp, arrival)) if a > 100 or r > 66]
print('large gaps (rtp ms, arrival ms, next frame is IDR):', [(round(r), round(a), k) for r, a, k in big[:12]])
