#!/usr/bin/env python3
"""Run shell commands on a HiSilicon camera over telnet.

Usage: hisi_shell.py [-t SECONDS] COMMAND...

Each COMMAND runs in one root shell session and prints its output and exit
status. CAM_HOST selects the camera (default 192.168.2.10); CAM_PW is a
comma-separated list of root passwords to try (default ardupilot). -t sets the
per-command timeout (default 20 s); run longer jobs with nohup and poll them,
because a command that times out is killed when the session closes.
"""
import os
import socket
import sys
import time

HOST = os.environ.get('CAM_HOST', '192.168.2.10')
PASSWORDS = os.environ.get('CAM_PW', 'ardupilot').split(',')
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240
MARKER = '__APCAM_DONE__'


def strip_iac(sock, data):
    """Refuse every telnet option and return the remaining text."""
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b != IAC:
            out.append(b)
            i += 1
            continue
        if i + 1 >= len(data):
            break
        cmd = data[i + 1]
        if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(data):
            sock.sendall(bytes([IAC, WONT if cmd in (DO, DONT) else DONT, data[i + 2]]))
            i += 3
        elif cmd == SB:
            end = data.find(bytes([IAC, SE]), i)
            i = len(data) if end < 0 else end + 2
        else:
            i += 2
    return out.decode('utf-8', 'replace')


def read_until(sock, markers, timeout):
    buf = ''
    deadline = time.time() + timeout
    while time.time() < deadline:
        sock.settimeout(max(0.1, deadline - time.time()))
        try:
            data = sock.recv(65536)
        except socket.timeout:
            break
        if not data:
            break
        buf += strip_iac(sock, data)
        if any(m in buf for m in markers):
            return buf, True
    return buf, False


def login():
    for password in PASSWORDS:
        sock = socket.create_connection((HOST, 23), timeout=5)
        out, ok = read_until(sock, ['login:'], 10)
        if not ok:
            sock.close()
            sys.exit(f'no login prompt from {HOST}: {out!r}')
        sock.sendall(b'root\r\n')
        out, _ = read_until(sock, ['assword:', '# '], 10)
        if 'assword:' in out:
            sock.sendall(password.encode() + b'\r\n')
            out, _ = read_until(sock, ['# ', 'incorrect', 'login:'], 10)
        if '# ' in out:
            return sock
        sock.close()
    sys.exit(f'{HOST}: login failed with every password in CAM_PW')


def main():
    args = sys.argv[1:]
    timeout = 20.0
    if args[:1] == ['-t'] and len(args) >= 2:
        timeout = float(args[1])
        args = args[2:]
    if not args:
        sys.exit(__doc__)
    sock = login()
    sock.sendall(b'PS1="#\\$ "; stty -echo 2>/dev/null\r\n')
    read_until(sock, ['#$ '], 5)
    status = '0'
    for cmd in args:
        # The quotes keep the echoed command line from matching the marker.
        sock.sendall(f'{cmd}\r\necho __APCAM_""DONE__$?\r\n'.encode())
        out, ok = read_until(sock, [MARKER], timeout)
        body, _, rest = out.partition(MARKER)
        status = rest.strip().split()[0] if rest.strip() else '?'
        print(f'$ {cmd}')
        print(body.replace('\r', '').replace('#$ ', '').strip())
        print(f'[exit {status}{"" if ok else ", timed out"}]')
    sock.sendall(b'exit\r\n')
    sock.close()
    sys.exit(0 if status == '0' else 1)


if __name__ == '__main__':
    main()
