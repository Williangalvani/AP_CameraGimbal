#!/usr/bin/env python3
"""hisi appfs staging (tools/build_hisi_appfs.py), with and without an MCM bundle.

camera-app, hisi-web and the MPP libraries are stand-in AArch64 ELFs carrying
only the headers build_hisi_appfs.py reads.
"""
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_hisi_appfs as appfs  # noqa: E402

MKSQUASHFS = os.environ.get('MKSQUASHFS', str(ROOT / 'build/deps/squashfs-tools/bin/mksquashfs'))
EHDR_SIZE = 64
PHDR_SIZE = 56


def fake_elf(interpreter=None, needed=None):
    """An AArch64 ELF with PT_INTERP and, when needed is a list, PT_DYNAMIC."""
    count = 1 + (interpreter is not None) + (needed is not None)
    body = b''
    headers = []
    offset = EHDR_SIZE + count * PHDR_SIZE
    if interpreter is not None:
        text = interpreter.encode() + b'\0'
        headers.append((appfs.PT_INTERP, offset + len(body), len(text)))
        body += text
    if needed is not None:
        strtab = offset + len(body)
        names = b'\0'
        positions = []
        for name in needed:
            positions.append(len(names))
            names += name.encode() + b'\0'
        body += names
        entries = [(appfs.DT_NEEDED, position) for position in positions]
        entries += [(appfs.DT_STRTAB, strtab), (appfs.DT_NULL, 0)]
        dynamic = b''.join(struct.pack('<qQ', tag, value) for tag, value in entries)
        headers.append((appfs.PT_DYNAMIC, offset + len(body), len(dynamic)))
        body += dynamic
    size = offset + len(body)
    headers.insert(0, (appfs.PT_LOAD, 0, size))
    ident = b'\x7fELF\x02\x01\x01'.ljust(16, b'\0')
    data = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, appfs.EM_AARCH64, 1, 0, EHDR_SIZE, 0, 0,
                       EHDR_SIZE, PHDR_SIZE, count, 0, 0, 0)
    for kind, position, length in headers:
        data += struct.pack('<IIQQQQQQ', kind, 4, position, position, position, length, length, 8)
    return data + body


def mode(path):
    return stat.S_IMODE(path.lstat().st_mode)


class Staging(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name)
        self.camera_app = self.path / 'camera-app'
        self.camera_app.write_bytes(fake_elf(appfs.INTERPRETER, ['libss_mpi.so', appfs.LIBC]))
        self.web = self.path / 'hisi-web'
        self.web.write_bytes(fake_elf())
        self.lib_dir = self.path / 'lib'
        self.lib_dir.mkdir()
        (self.lib_dir / 'libss_mpi.so').write_bytes(fake_elf(needed=[appfs.LIBC]))
        self.loader = self.path / 'libc.so'
        self.loader.write_bytes(b'loader')
        bundle = self.bundle = self.path / 'mcm-bundle'
        (bundle / 'bin').mkdir(parents=True)
        (bundle / 'lib/gstreamer-1.0').mkdir(parents=True)
        (bundle / 'run.sh').write_text('#!/bin/sh\n')
        (bundle / 'run.sh').chmod(0o755)
        (bundle / 'bin/mavlink-camera-manager').write_bytes(b'mcm')
        (bundle / 'bin/mavlink-camera-manager').chmod(0o755)
        (bundle / 'lib/ld-musl-aarch64.so.1').write_bytes(b'musl')
        (bundle / 'lib/ld-musl-aarch64.so.1').chmod(0o755)
        (bundle / 'lib/libc.musl-aarch64.so.1').symlink_to('ld-musl-aarch64.so.1')
        (bundle / 'lib/gstreamer-1.0/libgstapcam.so').write_bytes(b'apcam')
        (bundle / 'lib/gstreamer-1.0/libgstapcam.so').chmod(0o644)
        # chmod() through this one would reach outside the staging
        self.outside = self.path / 'outside'
        self.outside.mkdir(mode=0o700)
        (bundle / 'lib/outside').symlink_to(self.outside)

    def tearDown(self):
        self.directory.cleanup()

    def stage(self, *bundle):
        staging = Path(tempfile.mkdtemp(dir=self.path))
        appfs.stage(self.camera_app, self.web, self.lib_dir, self.loader, staging, *bundle)
        return staging

    def test_without_bundle(self):
        staging = self.stage()
        app = staging / 'ap_cameragimbal'
        self.assertTrue((app / 'bin/camera-app').is_file())
        self.assertTrue((app / 'lib/libss_mpi.so').is_file())
        for name in ('sdcard.sh', 'sdbench.sh'):
            self.assertEqual(mode(app / name), 0o755, name)
            self.assertEqual((app / name).read_bytes(), (ROOT / 'packaging/hisi' / name).read_bytes())
        self.assertEqual([path for path in staging.rglob('*') if 'mcm' in path.name], [])

    def test_with_bundle(self):
        staging = self.stage(self.bundle)
        app = staging / 'ap_cameragimbal'
        mcm = app / 'mcm'
        self.assertEqual(mode(app / 'mcm.sh'), 0o755)
        self.assertEqual((app / 'mcm.sh').read_bytes(), (ROOT / 'packaging/hisi/mcm.sh').read_bytes())
        self.assertEqual(mode(app / 'mcm-settings.json'), 0o644)
        self.assertEqual((app / 'mcm-settings.json').read_bytes(),
                         (ROOT / 'packaging/hisi/mcm-settings.json').read_bytes())
        self.assertEqual(mode(mcm / 'run.sh'), 0o755)
        self.assertEqual(mode(mcm / 'bin/mavlink-camera-manager'), 0o755)
        self.assertEqual((mcm / 'bin/mavlink-camera-manager').read_bytes(), b'mcm')
        self.assertEqual(mode(mcm / 'lib/ld-musl-aarch64.so.1'), 0o755)
        self.assertEqual(mode(mcm / 'lib/gstreamer-1.0/libgstapcam.so'), 0o644)
        link = mcm / 'lib/libc.musl-aarch64.so.1'
        self.assertTrue(link.is_symlink())
        self.assertEqual(os.readlink(link), 'ld-musl-aarch64.so.1')
        self.assertTrue((mcm / 'lib/outside').is_symlink())
        for directory in (mcm, mcm / 'bin', mcm / 'lib', mcm / 'lib/gstreamer-1.0'):
            self.assertEqual(mode(directory), 0o755, directory)
        self.assertEqual(mode(self.outside), 0o700, 'chmod followed a symlink out of the staging')

    def test_rejects_incomplete_bundle(self):
        (self.bundle / 'bin/mavlink-camera-manager').unlink()
        with self.assertRaisesRegex(ValueError, 'bin/mavlink-camera-manager'):
            self.stage(self.bundle)
        (self.bundle / 'run.sh').unlink()
        with self.assertRaisesRegex(ValueError, 'run.sh'):
            self.stage(self.bundle)

    @unittest.skipUnless(os.access(MKSQUASHFS, os.X_OK), 'mksquashfs not built (tools/bootstrap_squashfs_tools.sh)')
    def test_command_line(self):
        """--mcm-bundle changes the image; without it the image is unchanged."""
        def build(name, *extra):
            output = self.path / name
            subprocess.run([sys.executable, str(ROOT / 'tools/build_hisi_appfs.py'),
                            '--mksquashfs', MKSQUASHFS, '--lib-dir', str(self.lib_dir),
                            '--loader', str(self.loader), *extra,
                            str(self.camera_app), str(self.web), str(output)],
                           check=True, stdout=subprocess.DEVNULL)
            return output.read_bytes()
        plain = build('plain.img')
        self.assertEqual(build('again.img'), plain)
        with_mcm = build('mcm.img', '--mcm-bundle', str(self.bundle))
        self.assertNotEqual(with_mcm, plain)


class Settings(unittest.TestCase):
    def test_streams(self):
        settings = json.loads((ROOT / 'packaging/hisi/mcm-settings.json').read_text())
        self.assertEqual(settings['mavlink_endpoint'], 'udpin:127.0.0.1:14560')
        self.assertEqual(settings['blocked_sources'], [])
        streams = [(stream['name'],
                    stream['video_source']['Gst']['source'],
                    stream['stream_information']['endpoints'],
                    stream['stream_information']['configuration']['width'],
                    stream['stream_information']['configuration']['height'])
                   for stream in settings['streams']]
        self.assertEqual(streams, [
            ('4K Cam main (on camera)', {'ApCam': 'apcam:main'}, ['rtsp://0.0.0.0:8564/cam_main'], 3840, 2160),
            ('4K Cam sub (on camera)', {'ApCam': 'apcam:sub'}, ['rtsp://0.0.0.0:8564/cam_sub'], 1280, 720),
        ])
        for stream in settings['streams']:
            extended = stream['stream_information']['extended_configuration']
            self.assertTrue(extended['disable_mavlink'] and extended['disable_thumbnails'])
            self.assertIsNone(stream['mavlink'])


if __name__ == '__main__':
    unittest.main()
