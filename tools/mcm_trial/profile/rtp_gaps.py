#!/usr/bin/env python3
"""Sniff (S)RTP video from the camera and report frame arrival gaps.

Usage: sudo rtp_gaps.py [--source IP] [--seconds N] [--threshold MS]
SRTP leaves the RTP header in clear, so this works on WebRTC too. A frame is
one RTP timestamp; its arrival is its marker-bit (last) packet. Gaps above
the threshold are printed with the RTP timestamp gap, which says whether the
sender skipped frames (RTP gap large too) or delivered late (RTP gap 33 ms).
"""
import argparse
import socket
import struct
import time

ETH_P_ALL = 0x0003


def camera_mac(address):
    with open("/proc/net/arp") as arp:
        for line in arp.readlines()[1:]:
            fields = line.split()
            if fields[0] == address:
                return bytes.fromhex(fields[3].replace(":", ""))
    raise SystemExit(f"{address} is not in the ARP table; ping it first")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="192.168.2.10")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--threshold", type=float, default=100)
    parser.add_argument("--pt", type=int, default=96)
    parser.add_argument("--interface", default="br0")
    args = parser.parse_args()
    source = socket.inet_aton(args.source)
    source_mac = camera_mac(args.source)
    sniffer = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    sniffer.settimeout(0.5)
    frames = []  # (arrival of marker packet, rtp ts, packets, bytes, first arrival)
    current = None
    packets = 0
    lost = 0
    last_seq = None
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        try:
            data, address = sniffer.recvfrom(65535)
        except socket.timeout:
            continue
        if address[0] != args.interface or address[2] == socket.PACKET_OUTGOING \
                or len(data) < 14 + 20 + 8 + 12:
            continue
        ethertype = struct.unpack_from("!H", data, 12)[0]
        ip = data[14:]
        if ethertype == 0x0800:
            if ip[9] != 17 or ip[12:16] != source:
                continue
            rtp = ip[(ip[0] & 15) * 4 + 8:]
        elif ethertype == 0x86DD:
            # WebRTC often picks IPv6; no extension headers expected
            if ip[6] != 17 or data[6:12] != source_mac:
                continue
            rtp = ip[40 + 8:]
        else:
            continue
        if len(rtp) < 12 or rtp[0] >> 6 != 2 or rtp[1] & 0x7F != args.pt:
            continue
        now = time.monotonic()
        seq, ts = struct.unpack_from("!HI", rtp, 2)
        if last_seq is not None and (seq - last_seq) & 0xFFFF != 1:
            lost += ((seq - last_seq) & 0xFFFF) - 1
        last_seq = seq
        packets += 1
        if current is None or current[1] != ts:
            current = [None, ts, 0, 0, now]
        current[2] += 1
        current[3] += len(rtp)
        if rtp[1] & 0x80:
            current[0] = now
            frames.append(tuple(current))
            current = None
    if len(frames) < 2:
        print(f"only {len(frames)} frames, {packets} packets")
        return 1
    gaps = []
    for previous, frame in zip(frames, frames[1:]):
        arrival = 1000 * (frame[0] - previous[0])
        rtp_gap = ((frame[1] - previous[1]) & 0xFFFFFFFF) / 90
        gaps.append(arrival)
        if arrival > args.threshold:
            print(f"t={frame[0] - frames[0][0]:6.2f}s arrival gap {arrival:6.1f} ms, "
                  f"rtp gap {rtp_gap:5.1f} ms, frame {frame[2]} packets {frame[3]} bytes, "
                  f"spread {1000 * (frame[0] - frame[4]):5.1f} ms")
    gaps.sort()
    big = sorted(frames, key=lambda f: f[3])[-1]
    print(f"{len(frames)} frames, {packets} packets, {lost} sequence gaps; arrival gap "
          f"median {gaps[len(gaps) // 2]:.1f} ms, p99 {gaps[int(len(gaps) * 0.99)]:.1f} ms, "
          f"max {gaps[-1]:.1f} ms; largest frame {big[2]} packets, spread "
          f"{1000 * (big[0] - big[4]):.1f} ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
