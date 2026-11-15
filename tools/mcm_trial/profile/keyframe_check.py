#!/usr/bin/env python3
"""Measure how fast an RTSP server answers keyframe requests.

For each run: connect, PLAY, time until the first IDR; then, mid-GOP, send an
RTCP PLI (or FIR) and time until the next IDR. Without request support the
next IDR arrives at the GOP boundary; with it, a frame or two later.

Usage: keyframe_check.py [--udp] [--fir] [--requests N] rtsp://host:port/path
"""
import argparse
import random
import re
import select
import socket
import statistics
import struct
import time
from urllib.parse import urlparse


class Rtsp:
    def __init__(self, url, udp):
        self.url = url
        parsed = urlparse(url)
        self.host = parsed.hostname
        self.sock = socket.create_connection((parsed.hostname, parsed.port or 554), timeout=5)
        self.cseq = 0
        self.session = None
        self.buffer = b''
        self.udp = udp
        self.codec = None
        self.rtp = self.rtcp = None
        self.server_rtcp = None

    def request(self, method, url, headers=()):
        self.cseq += 1
        lines = [f'{method} {url} RTSP/1.0', f'CSeq: {self.cseq}', *headers]
        if self.session:
            lines.append(f'Session: {self.session}')
        self.sock.sendall(('\r\n'.join(lines) + '\r\n\r\n').encode())
        while b'\r\n\r\n' not in self.buffer:
            self.buffer += self.sock.recv(65536)
        head, self.buffer = self.buffer.split(b'\r\n\r\n', 1)
        text = head.decode(errors='replace')
        length = re.search(r'Content-Length:\s*(\d+)', text, re.I)
        body = b''
        if length:
            need = int(length.group(1))
            while len(self.buffer) < need:
                self.buffer += self.sock.recv(65536)
            body, self.buffer = self.buffer[:need], self.buffer[need:]
        if ' 200 ' not in text.split('\r\n')[0]:
            raise RuntimeError(f'{method}: {text.splitlines()[0]}')
        return text, body.decode(errors='replace')

    def start(self):
        _, sdp = self.request('DESCRIBE', self.url, ['Accept: application/sdp'])
        self.codec = 'h265' if re.search(r'rtpmap:\d+ H265', sdp) else 'h264'
        control = re.search(r'a=control:(\S+)', sdp.split('m=video', 1)[1])
        track = control.group(1) if control else ''
        track = track if track.startswith('rtsp:') else self.url.rstrip('/') + '/' + track
        if self.udp:
            self.rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.rtp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
            self.rtp.bind(('', 0))
            self.rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.rtcp.bind(('', self.rtp.getsockname()[1] + 1))
            port = self.rtp.getsockname()[1]
            transport = f'RTP/AVP;unicast;client_port={port}-{port + 1}'
        else:
            transport = 'RTP/AVP/TCP;unicast;interleaved=0-1'
        head, _ = self.request('SETUP', track, [f'Transport: {transport}'])
        self.session = re.search(r'Session:\s*([^;\r\n]+)', head).group(1).strip()
        if self.udp:
            server = re.search(r'server_port=(\d+)-(\d+)', head)
            self.server_rtcp = (self.host, int(server.group(2)))
            self.rtcp.sendto(b'', self.server_rtcp)
        self.request('PLAY', self.url, ['Range: npt=0.000-'])
        return time.monotonic()

    def packets(self, until):
        """Yield (arrival, ssrc, payload) until the monotonic deadline."""
        while time.monotonic() < until:
            if self.udp:
                ready, _, _ = select.select([self.rtp], [], [], 0.2)
                if not ready:
                    continue
                data = self.rtp.recv(65536)
                yield time.monotonic(), data
                continue
            while len(self.buffer) < 4 or (self.buffer[0] == 0x24 and
                                           len(self.buffer) < 4 + struct.unpack('>H', self.buffer[2:4])[0]):
                ready, _, _ = select.select([self.sock], [], [], 0.2)
                if ready:
                    self.buffer += self.sock.recv(262144)
                if time.monotonic() >= until:
                    return
            if self.buffer[0] != 0x24:
                raise RuntimeError('lost interleaved framing')
            channel = self.buffer[1]
            size = struct.unpack('>H', self.buffer[2:4])[0]
            data, self.buffer = self.buffer[4:4 + size], self.buffer[4 + size:]
            if channel == 0:
                yield time.monotonic(), data

    def is_idr_start(self, rtp):
        offset = 12 + 4 * (rtp[0] & 15)
        if rtp[0] & 0x10:
            offset += 4 + 4 * struct.unpack('>H', rtp[offset + 2:offset + 4])[0]
        payload = rtp[offset:]
        if len(payload) < 3:
            return False
        if self.codec == 'h264':
            kind = payload[0] & 31
            if kind == 5:
                return True
            if kind == 28:
                return payload[1] & 0x80 and payload[1] & 31 == 5
            if kind == 24:
                pos = 1
                while pos + 2 < len(payload):
                    size = struct.unpack('>H', payload[pos:pos + 2])[0]
                    if payload[pos + 2] & 31 == 5:
                        return True
                    pos += 2 + size
            return False
        kind = (payload[0] >> 1) & 63
        if kind in (19, 20, 21):
            return True
        if kind == 49:
            return payload[2] & 0x80 and (payload[2] & 63) in (19, 20, 21)
        if kind == 48:
            pos = 2
            while pos + 3 < len(payload):
                size = struct.unpack('>H', payload[pos:pos + 2])[0]
                if ((payload[pos + 2] >> 1) & 63) in (19, 20, 21):
                    return True
                pos += 2 + size
        return False

    def send_request(self, media_ssrc, fir_seq):
        sender = 0x4b465243
        if fir_seq is None:
            packet = struct.pack('>BBHII', 0x81, 206, 2, sender, media_ssrc)
        else:
            packet = struct.pack('>BBHIIIB3x', 0x84, 206, 4, sender, 0, media_ssrc, fir_seq & 255)
        if self.udp:
            self.rtcp.sendto(packet, self.server_rtcp)
        else:
            self.sock.sendall(struct.pack('>BBH', 0x24, 1, len(packet)) + packet)

    def close(self):
        try:
            self.request('TEARDOWN', self.url)
        except Exception:
            pass
        self.sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('url')
    parser.add_argument('--udp', action='store_true')
    parser.add_argument('--fir', action='store_true')
    parser.add_argument('--connects', type=int, default=5)
    parser.add_argument('--requests', type=int, default=8)
    args = parser.parse_args()

    first = []
    for _ in range(args.connects):
        client = Rtsp(args.url, args.udp)
        played = client.start()
        for arrival, rtp in client.packets(played + 5):
            if client.is_idr_start(rtp):
                first.append((arrival - played) * 1000)
                break
        client.close()
        time.sleep(0.8)

    client = Rtsp(args.url, args.udp)
    played = client.start()
    gop, answers = [], []
    last_idr = pending = next_request = pending_sent = None
    sent = 0
    for arrival, rtp in client.packets(played + 6 + args.requests * 3):
        if pending is None and next_request is not None and arrival >= next_request:
            client.send_request(struct.unpack('>I', rtp[8:12])[0], sent if args.fir else None)
            pending, next_request = time.monotonic(), None
            pending_sent = pending
            sent += 1
        if not client.is_idr_start(rtp) or (last_idr and arrival - last_idr < 0.010):
            continue
        if pending is not None:
            answers.append((arrival - pending) * 1000)
            pending = None
        elif last_idr is not None:
            gop.append((arrival - last_idr) * 1000)
        last_idr = arrival
        if len(gop) < 3:
            continue
        if sent >= args.requests:
            break
        # The server answers at most one request every 500 ms per stream.
        next_request = max(arrival + random.uniform(0.25, 0.6), (pending_sent or 0) + 0.6)
    client.close()

    def summary(values):
        if not values:
            return 'none'
        return (f'n={len(values)} median={statistics.median(values):.0f} ms '
                f'min={min(values):.0f} max={max(values):.0f}')
    kind = 'FIR' if args.fir else 'PLI'
    print(f'{client.codec} {"udp" if args.udp else "tcp"}: PLAY to first IDR: {summary(first)}')
    print(f'  natural IDR interval: {summary(gop)}')
    print(f'  {kind} sent {sent}, answered {len(answers)}: {summary(answers)}')


if __name__ == '__main__':
    main()
