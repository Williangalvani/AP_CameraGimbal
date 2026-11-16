#!/usr/bin/env python3
"""Measure how much later frames arrive over MCM's WebRTC than over direct RTSP.

Usage: sudo path_latency.py [--source IP] [--seconds N] [--direct-port PORT]
                            [--srtp-tag BYTES] [--json PATH]
Sniffs every (S)RTP stream the camera sends to this host. camera-app's RTSP
stream is plain RTP/UDP; MCM's WebRTC stream is SRTP, whose RTP header is in
clear. A frame is one RTP timestamp. Frames of the WebRTC stream are paired
with frames of the direct stream by correlating per-frame payload sizes, then
the arrival of each pair's first and marker packets is compared. Without
--direct-port the SSRC with the lowest UDP source port is taken as direct;
every other SSRC has --srtp-tag bytes per packet removed from its sizes.
"""
import argparse
import json
import math
import socket
import struct
import time

ETH_P_ALL = 0x0003
SOL_PACKET = 263
PACKET_STATISTICS = 6
SO_RCVBUFFORCE = 33
HEADER = 8 + 12 + 60 + 4  # UDP, RTP, 15 CSRCs, extension header
MIN_FRAMES = 30
MAX_OFFSET = 300
MIN_OVERLAP = 60
TICKS_2MS = 180


def camera_mac(address):
    with open("/proc/net/arp") as arp:
        for line in arp.readlines()[1:]:
            fields = line.split()
            if fields[0] == address:
                return bytes.fromhex(fields[3].replace(":", ""))
    raise SystemExit(f"{address} is not in the ARP table; ping it first")


def capture(args):
    source = socket.inet_aton(args.source)
    source_mac = camera_mac(args.source)
    low, high = (96, 127) if args.pt is None else (args.pt, args.pt)
    sniffer = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    for option in (SO_RCVBUFFORCE, socket.SO_RCVBUF):
        try:
            sniffer.setsockopt(socket.SOL_SOCKET, option, 16 << 20)
            break
        except OSError:
            pass
    sniffer.settimeout(0.5)
    try:
        sniffer.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8)  # reading resets the counters
    except OSError:
        pass
    packets = []  # (arrival, ip version, UDP header + start of RTP)
    interface = args.interface
    outgoing = socket.PACKET_OUTGOING
    now = time.monotonic()
    end = now + args.seconds
    while now < end:
        try:
            data, address = sniffer.recvfrom(65535)
        except socket.timeout:
            now = time.monotonic()
            continue
        now = time.monotonic()
        if address[0] != interface or address[2] == outgoing or len(data) < 14 + 20 + 8 + 12:
            continue
        ethertype = data[12] << 8 | data[13]
        if ethertype == 0x0800:
            if data[23] != 17 or data[26:30] != source:
                continue
            udp = 14 + (data[14] & 15) * 4
            version = 4
        elif ethertype == 0x86DD:
            # WebRTC often picks IPv6; no extension headers expected
            if data[20] != 17 or data[6:12] != source_mac:
                continue
            udp = 14 + 40
            version = 6
        else:
            continue
        if len(data) < udp + 8 + 12 or data[udp + 8] >> 6 != 2 \
                or not low <= data[udp + 9] & 0x7F <= high:
            continue
        packets.append((now, version, data[udp:udp + HEADER]))
    try:
        stats = struct.unpack("II", sniffer.getsockopt(SOL_PACKET, PACKET_STATISTICS, 8))
    except OSError:
        stats = None
    return packets, stats


def parse(packets):
    streams = {}
    for arrival, version, header in packets:
        rtp = header[8:]
        start = 12 + 4 * (rtp[0] & 15)
        if rtp[0] & 0x10:
            if len(rtp) < start + 4:
                continue
            start += 4 + 4 * (rtp[start + 2] << 8 | rtp[start + 3])
        payload = (header[4] << 8 | header[5]) - 8 - start
        seq, ts, ssrc = struct.unpack_from("!HII", rtp, 2)
        stream = streams.get(ssrc)
        if stream is None:
            stream = streams[ssrc] = {
                "ssrc": ssrc, "port": header[0] << 8 | header[1], "version": version,
                "pt": rtp[1] & 0x7F, "packets": 0, "lost": 0, "seq": seq - 1, "frames": {}}
        step = (seq - stream["seq"]) & 0xFFFF
        if step < 0x8000:
            stream["lost"] += step - 1
            stream["seq"] = seq
        stream["packets"] += 1
        frame = stream["frames"].get(ts)
        if frame is None:
            frame = stream["frames"][ts] = [ts, arrival, None, 0, 0]
        frame[3] += 1
        frame[4] += payload
        if rtp[1] & 0x80:
            frame[2] = arrival
    for stream in streams.values():
        stream["frames"] = list(stream["frames"].values())  # ts, first, marker, packets, bytes
    return streams


def median(values):
    values = sorted(values)
    return values[len(values) // 2]


def spread(values):
    if not values:
        return "n=0"
    v = sorted(values)
    pick = lambda p: v[min(len(v) - 1, int(len(v) * p))]
    return (f"n={len(v)} median {pick(0.5):6.1f} p10 {pick(0.1):6.1f} p90 {pick(0.9):6.1f} "
            f"p99 {pick(0.99):6.1f} max {v[-1]:6.1f} ms")


def pearson(xs, ys):
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs)
    syy = sum((y - my) ** 2 for y in ys)
    return sxy / math.sqrt(sxx * syy) if sxx and syy else 0.0


def align(a, b):
    """Offset k so that b[i + k] is a[i], best log-size correlation."""
    la = [math.log(max(f[4], 1)) for f in a]
    lb = [math.log(max(f[4], 1)) for f in b]
    best = None
    for k in range(-MAX_OFFSET, MAX_OFFSET + 1):
        i0 = max(0, -k)
        i1 = min(len(la), len(lb) - k)
        if i1 - i0 < MIN_OVERLAP:
            continue
        r = pearson(la[i0:i1], lb[i0 + k:i1 + k])
        if best is None or r > best[0]:
            best = (r, k, i1 - i0)
    return best


def gap_line(stream):
    markers = [f[2] for f in stream["frames"] if f[2] is not None]
    gaps = sorted(1000 * (b - a) for a, b in zip(markers, markers[1:]))
    if not gaps:
        return "no marker packets"
    return (f"{len(stream['frames'])} frames, {stream['packets']} packets, "
            f"{stream['lost']} sequence gaps; arrival gap median {gaps[len(gaps) // 2]:.1f} ms, "
            f"p99 {gaps[int(len(gaps) * 0.99)]:.1f} ms, max {gaps[-1]:.1f} ms")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="192.168.2.10")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--pt", type=int, default=None, help="default: any of 96-127")
    parser.add_argument("--interface", default="br0")
    parser.add_argument("--direct-port", type=int, help="camera-app's RTP source port")
    parser.add_argument("--srtp-tag", type=int, default=10,
                        help="SRTP auth tag bytes per packet of non-direct SSRCs")
    parser.add_argument("--json", help="write the matched frame pairs here")
    args = parser.parse_args()

    packets, stats = capture(args)
    if stats:
        print(f"kernel: {stats[0]} packets, {stats[1]} dropped")
    streams = parse(packets)
    eligible = [s for s in streams.values() if len(s["frames"]) >= MIN_FRAMES]
    if args.direct_port is not None:
        candidates = [s for s in eligible if s["port"] == args.direct_port]
    else:
        candidates = sorted(eligible, key=lambda s: s["port"])[:1]
    direct = max(candidates, key=lambda s: len(s["frames"])) if candidates else None
    for stream in streams.values():
        if stream is not direct:
            for frame in stream["frames"]:
                frame[4] -= args.srtp_tag * frame[3]

    print("ssrc        port  ip  pt  packets  frames  bytes/frame  pkts/frame")
    for s in sorted(streams.values(), key=lambda s: s["port"]):
        frames = s["frames"]
        role = "direct" if s is direct else "webrtc" if len(frames) >= MIN_FRAMES else "ignored"
        print(f"0x{s['ssrc']:08x} {s['port']:5d}  v{s['version']} {s['pt']:3d} {s['packets']:8d} "
              f"{len(frames):7d} {sum(f[4] for f in frames) / len(frames):12.1f} "
              f"{median(f[3] for f in frames):11d}  {role}")
    if direct is None:
        print(f"no direct stream with >= {MIN_FRAMES} frames"
              + (f" from port {args.direct_port}" if args.direct_port is not None else ""))
        return 1
    others = [s for s in eligible if s is not direct]
    for stream in [direct] + others:
        print(f"0x{stream['ssrc']:08x}: {gap_line(stream)}")
    if not others:
        print("no WebRTC stream to compare with")
        return 1

    a = direct["frames"]
    key_bytes = 4 * median(f[4] for f in a)
    rows = []
    for stream in others:
        b = stream["frames"]
        best = align(a, b)
        print(f"\nwebrtc 0x{stream['ssrc']:08x} vs direct 0x{direct['ssrc']:08x}:")
        if best is None:
            print(f"  fewer than {MIN_OVERLAP} overlapping frames at every offset")
            continue
        r, k, n = best
        pairs = [(a[i], b[i + k]) for i in range(max(0, -k), min(len(a), len(b) - k))]
        same = sum(abs(((fa[0] - pa[0]) & 0xFFFFFFFF) - ((fb[0] - pb[0]) & 0xFFFFFFFF)) <= TICKS_2MS
                   for (pa, pb), (fa, fb) in zip(pairs, pairs[1:]))
        print(f"  offset {k} frames, log-size correlation {r:.3f} over {n} frames, "
              f"rtp deltas equal within 2 ms for {100 * same / (n - 1):.1f}%")
        deltas = {True: ([], []), False: ([], [])}
        for fa, fb in pairs:
            key = fa[4] > key_bytes
            first = 1000 * (fb[1] - fa[1])
            last = 1000 * (fb[2] - fa[2]) if fa[2] is not None and fb[2] is not None else None
            deltas[key][0].append(first)
            if last is not None:
                deltas[key][1].append(last)
            rows.append({"ssrc": f"0x{stream['ssrc']:08x}", "rtp_ts_direct": fa[0],
                         "rtp_ts_webrtc": fb[0], "bytes_direct": fa[4], "bytes_webrtc": fb[4],
                         "delta_first_ms": round(first, 3),
                         "delta_last_ms": None if last is None else round(last, 3), "key": key})
        print(f"  matched {len(pairs)} frames (webrtc minus direct)")
        for key, name in ((True, "keyframes"), (False, "other    ")):
            print(f"  {name} first packet:  {spread(deltas[key][0])}")
            print(f"  {name} marker packet: {spread(deltas[key][1])}")
    if args.json:
        with open(args.json, "w") as out:
            json.dump(rows, out)
        print(f"wrote {len(rows)} rows to {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
