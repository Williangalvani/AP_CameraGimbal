#!/usr/bin/env python3
"""hisi appfs image: vendor container layout, CRC and UBI structure.

With HISI_VENDOR_ROOTFS set to an extracted camera rootfs, also upload the
image to the camera's own updater (/usr/hiface/update) under qemu-aarch64 in a
private user/net/pid namespace; it stops at the missing MTD device.
"""
import contextlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import hisi_firmware as fw

MKSQUASHFS = os.environ.get('MKSQUASHFS', str(ROOT / 'build/deps/squashfs-tools/bin/mksquashfs'))


def sample_ubi():
    return fw.ubinize(bytes(range(256)) * 900)


def with_reserved(ubi, reserved):
    """ubi with volume 0 reserving `reserved` PEBs in both volume table copies."""
    image = bytearray(ubi)
    for copy in range(2):
        record = copy * fw.PEB_SIZE + fw.DATA_OFFSET
        struct.pack_into('>I', image, record, reserved)
        struct.pack_into('>I', image, record + 168, fw.ubi_crc(image[record:record + 168]))
    return bytes(image)


class Container(unittest.TestCase):
    def test_layout(self):
        image = fw.pack([(fw.APPFS_MTD, b'payload')], reboot=False)
        self.assertEqual(image[:4], bytes.fromhex('cbbccaab'))
        self.assertEqual(struct.unpack_from('<IIIII', image, 4), (0x42, 19, 2, 51, 1))
        self.assertEqual(struct.unpack_from('<I', image, 0x18)[0], len(image))
        self.assertEqual(image[0x1c], 0)
        self.assertEqual(struct.unpack_from('<I', image, 0x30)[0], fw.MAGIC_END)
        self.assertEqual(image[0x34:0x34 + 10], b'/dev/mtd4\0')
        self.assertEqual(struct.unpack_from('<II', image, 0x34 + 0x80),
                         (7, fw.HEADER_SIZE + fw.ENTRY_SIZE))
        self.assertEqual(struct.unpack_from('<I', image, 0xc4)[0], fw.vendor_crc(b'payload'))
        self.assertEqual(len(image), fw.HEADER_SIZE + fw.ENTRY_SIZE + 7 + fw.TRAILER_SIZE)
        self.assertEqual(fw.unpack(image), (False, [(fw.APPFS_MTD, b'payload')]))

    def test_vendor_crc(self):
        self.assertEqual(fw.vendor_crc(b''), 0xcb)
        self.assertEqual(fw.vendor_crc(b'123456789'), 0xfe570723)

    def test_rejects(self):
        image = bytearray(fw.pack([(fw.APPFS_MTD, b'payload')]))
        corrupt = bytearray(image)
        corrupt[fw.HEADER_SIZE + fw.ENTRY_SIZE] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC'):
            fw.unpack(bytes(corrupt))
        wrong_length = bytearray(image)
        struct.pack_into('<I', wrong_length, 0x18, len(image) + 1)
        with self.assertRaisesRegex(ValueError, 'length'):
            fw.unpack(bytes(wrong_length))
        with self.assertRaisesRegex(ValueError, 'Device code'):
            fw.unpack(bytes(image), device_code=(19, 2, 52))
        with self.assertRaisesRegex(ValueError, 'magic'):
            fw.unpack(b'\0' * 4 + bytes(image[4:]))
        with self.assertRaisesRegex(ValueError, 'MTD path'):
            fw.pack([('/tmp/x', b'')])


class Ubi(unittest.TestCase):
    def test_structure(self):
        image = sample_ubi()
        fw.check_ubi(image)
        self.assertEqual(len(image), 4 * fw.PEB_SIZE)
        peb = image[2 * fw.PEB_SIZE:3 * fw.PEB_SIZE]
        self.assertEqual(peb[fw.DATA_OFFSET:fw.DATA_OFFSET + 256], bytes(range(256)))
        vid = struct.unpack_from('>4sBBBBII', peb, fw.VID_HDR_OFFSET)
        self.assertEqual(vid, (b'UBI!', 1, 1, 0, 0, 0, 0))
        record = image[fw.DATA_OFFSET:fw.DATA_OFFSET + fw.VTBL_RECORD_SIZE]
        self.assertEqual(struct.unpack_from('>IIIBBH', record), (2, 1, 0, 1, 0, 5))
        self.assertEqual(record[16:21], b'appfs')

    def test_check_rejects_corruption(self):
        image = bytearray(sample_ubi())
        image[fw.PEB_SIZE + 8] ^= 1
        with self.assertRaisesRegex(ValueError, 'EC header'):
            fw.check_ubi(bytes(image))

    def test_too_large(self):
        with self.assertRaisesRegex(ValueError, 'do not fit'):
            fw.ubinize(b'\0' * (fw.APPFS_VOLUME_MAX_PEBS * fw.LEB_SIZE + 1))
        fw.check_ubi(fw.ubinize(b'\0' * fw.APPFS_VOLUME_MAX_PEBS * fw.LEB_SIZE))

    def test_volume_may_reserve_the_partition(self):
        image = sample_ubi()
        fw.check_ubi(with_reserved(image, fw.APPFS_VOLUME_MAX_PEBS))
        with self.assertRaisesRegex(ValueError, 'Volume 0'):
            fw.check_ubi(with_reserved(image, fw.APPFS_VOLUME_MAX_PEBS + 1))
        with self.assertRaisesRegex(ValueError, 'Volume 0'):
            fw.check_ubi(with_reserved(image, len(image) // fw.PEB_SIZE - 3))


def build_appfs(directory, *extra):
    staging = Path(directory) / 'stage'
    (staging / 'sh').mkdir(parents=True)
    (staging / 'fconfigs').mkdir()
    (staging / 'sh/start.sh').write_text('#!/bin/sh\n')
    output = Path(directory) / 'appfs.img'
    subprocess.run([sys.executable, str(ROOT / 'tools/hisi_firmware.py'), 'appfs', str(staging),
                    str(output), '--mksquashfs', MKSQUASHFS, *extra],
                   check=True, stdout=subprocess.DEVNULL)
    return output


@unittest.skipUnless(os.access(MKSQUASHFS, os.X_OK), 'mksquashfs not built (tools/bootstrap_squashfs_tools.sh)')
class Appfs(unittest.TestCase):
    def build(self, directory, *extra):
        return build_appfs(directory, *extra)

    def test_appfs_squashfs(self):
        with tempfile.TemporaryDirectory() as directory:
            reboot, entries = fw.unpack(self.build(directory).read_bytes())
            self.assertTrue(reboot)
            (path, ubi), = entries
            fw.check_ubi(ubi)
            superblock = ubi[2 * fw.PEB_SIZE + fw.DATA_OFFSET:][:96]
            self.assertEqual(superblock[:4], b'hsqs')
            compression, = struct.unpack_from('<H', superblock, 20)
            block_size, = struct.unpack_from('<I', superblock, 12)
            self.assertEqual((compression, block_size), (4, 65536))

    def test_existing_volume(self):
        with tempfile.TemporaryDirectory() as directory:
            (_, ubi), = fw.unpack(self.build(directory).read_bytes())[1]
            volume = ubi[2 * fw.PEB_SIZE + fw.DATA_OFFSET:][:fw.LEB_SIZE]
            output = Path(directory) / 'volume.img'
            with contextlib.redirect_stdout(io.StringIO()):
                fw.build_volume(volume, output)
            (_, again), = fw.unpack(output.read_bytes())[1]
            self.assertEqual(again[2 * fw.PEB_SIZE + fw.DATA_OFFSET:][:fw.LEB_SIZE], volume)
            with self.assertRaisesRegex(ValueError, 'not a little-endian squashfs'):
                fw.build_volume(b'UBI#' + volume[4:], output)

    @unittest.skipUnless(os.environ.get('HISI_VENDOR_ROOTFS'), 'HISI_VENDOR_ROOTFS not set')
    def test_vendor_updater_accepts(self):
        if not shutil.which('qemu-aarch64-static') or not shutil.which('unshare'):
            self.skipTest('needs qemu-aarch64-static and unshare')
        with tempfile.TemporaryDirectory() as directory:
            image = self.build(directory, '--no-reboot')
            sysroot = Path(directory) / 'sysroot'
            shutil.copytree(os.environ['HISI_VENDOR_ROOTFS'], sysroot, symlinks=True)
            param = sysroot / 'appfs/fconfigs/param'
            param.mkdir(parents=True, exist_ok=True)
            (param / 'updatetype.ini').write_text(
                '[UPDATE_SET]\nDEV_TYPE = 19\nSDK_TYPE = 2\nPLATFORM_TYPE = 51\n')
            log = Path(directory) / 'updater.log'
            script = textwrap.dedent(f'''\
                ip link set lo up
                script -qfc "qemu-aarch64-static -L {sysroot} {sysroot}/usr/hiface/update" {log} >/dev/null 2>&1 &
                for i in $(seq 1 80); do (exec 3<>/dev/tcp/127.0.0.1/9091) 2>/dev/null && break; sleep 0.25; done
                curl -s -m 60 -H 'Expect:' --data-binary @{image} http://127.0.0.1:9091/fwUpgrade >/dev/null
                sleep 6
                ''')
            subprocess.run(['unshare', '-r', '-n', '-p', '-f', '-m', '--mount-proc', 'bash', '-c', script],
                           check=True, timeout=120)
            text = log.read_bytes().decode(errors='replace')
            self.assertIn('Version Code : 0x13020033', text)
            self.assertIn('dest_path=/dev/mtd4', text)
            self.assertNotIn('CRC32 Chack Failed', text)


CXX = os.environ.get('CXX', 'c++')
VENDOR_APPFS = ROOT.parents[1] / 'restore/vendor_appfs_restore.bin'
# An original vendor update, e.g. IPC_18.010.U35.5_0054_V16(260305).img.
VENDOR_IMAGE = Path(os.environ.get('HISI_VENDOR_IMAGE', '/nonexistent'))
NAMES_ACCEPTED = ['HISI_AP_b321e2_appfs.img', 'HISI_AP_b321e2_appfs.bin',
                  'IPC_18.010.U35.5_0054_V16(260305).img', 'IPC_18.010 (1).img', 'a+b.img', 'x' * 196 + '.img']
NAMES_REJECTED = ['', '.img', 'appfs.tar', 'appfs.img.txt', 'APPFS.IMG', '../appfs.img', 'dir/appfs.img',
                  'dir\\appfs.img', 'a"b.img', 'a<b>.img', 'a&b.img', "a'b.img", 'a\tb.img', 'x' * 197 + '.img']


@unittest.skipUnless(shutil.which(CXX), 'no host C++ compiler')
@unittest.skipUnless(os.access(MKSQUASHFS, os.X_OK), 'mksquashfs not built (tools/bootstrap_squashfs_tools.sh)')
class WebCheck(unittest.TestCase):
    """include/apcam/hisi_firmware.h, which hisi-web runs before forwarding to :9091."""

    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.path = Path(cls.directory.name)
        cls.checker = cls.path / 'check'
        subprocess.run([CXX, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'include'),
                        str(ROOT / 'tests/hisi_firmware_check.cpp'), '-o', str(cls.checker)], check=True)
        cls.good = build_appfs(cls.path).read_bytes()
        cls.ubi = fw.unpack(cls.good)[1][0][1]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def check(self, image, *extra):
        path = self.path / 'candidate.bin'
        path.write_bytes(image)
        result = subprocess.run([str(self.checker), str(path), *extra], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, result.stdout.strip() == 'ok', result.stdout)
        return result.stdout.strip()

    def test_accepts_built_appfs(self):
        self.assertEqual(self.check(self.good), 'ok')

    @unittest.skipUnless(VENDOR_APPFS.is_file(), 'no vendor appfs restore image')
    def test_accepts_vendor_restore(self):
        self.assertEqual(self.check(VENDOR_APPFS.read_bytes()), 'ok')

    @unittest.skipUnless(VENDOR_IMAGE.is_file(), 'HISI_VENDOR_IMAGE not set')
    def test_accepts_vendor_image(self):
        self.assertEqual(self.check(VENDOR_IMAGE.read_bytes()), 'ok')

    def test_volume_may_reserve_the_partition(self):
        pebs = len(self.ubi) // fw.PEB_SIZE
        self.assertEqual(self.check(fw.pack([(fw.APPFS_MTD, with_reserved(self.ubi, fw.APPFS_VOLUME_MAX_PEBS))])), 'ok')
        for reserved in (fw.APPFS_VOLUME_MAX_PEBS + 1, pebs - 3):
            self.assertIn('Volume 0', self.check(fw.pack([(fw.APPFS_MTD, with_reserved(self.ubi, reserved))])))

    def name_verdict(self, name):
        result = subprocess.run([str(self.checker), '--name', name], capture_output=True, text=True)
        return result.stdout.strip()

    def test_names(self):
        for name in NAMES_ACCEPTED:
            self.assertEqual(self.name_verdict(name), 'ok', name)
        for name in NAMES_REJECTED:
            self.assertNotEqual(self.name_verdict(name), 'ok', name)

    def test_name_regex_matches_the_check(self):
        """upgrade.js tests names with NAME_REGEX before uploading."""
        regex = subprocess.run([str(self.checker), '--name-regex'], capture_output=True, text=True,
                               check=True).stdout.rstrip('\n')
        for name in NAMES_ACCEPTED + NAMES_REJECTED:
            self.assertEqual(bool(re.fullmatch(regex, name)), name in NAMES_ACCEPTED, name)
        node = shutil.which('node')
        if node:
            script = 'const r = new RegExp(process.argv[1]); console.log(JSON.stringify(JSON.parse(process.argv[2]).map(n => r.test(n))))'
            names = NAMES_ACCEPTED + NAMES_REJECTED
            verdicts = subprocess.run([node, '-e', script, regex, json.dumps(names)], capture_output=True,
                                      text=True, check=True).stdout
            self.assertEqual(json.loads(verdicts), [name in NAMES_ACCEPTED for name in names])

    def test_device_code_from_updatetype(self):
        ini = self.path / 'updatetype.ini'
        ini.write_text('[UPDATE_SET]\nVER = 1\nDEV_TYPE = 19\nSDK_TYPE = 2\nPLATFORM_TYPE = 52\n')
        self.assertIn('Device code 19/2/51', self.check(self.good, str(ini)))
        self.assertIn('Device code', self.check(fw.pack([(fw.APPFS_MTD, self.ubi)], device_code=(19, 2, 52))))

    def test_rejects_container(self):
        self.assertIn('reboot', self.check(fw.pack([(fw.APPFS_MTD, self.ubi)], reboot=False)))
        self.assertIn('/dev/mtd4', self.check(fw.pack([('/dev/mtd2', self.ubi)])))
        self.assertIn('exactly one', self.check(fw.pack([(fw.APPFS_MTD, self.ubi), ('/dev/mtd5', b'x')])))
        self.assertIn('magic', self.check(self.good[:-1]))
        self.assertIn('too short', self.check(b''))
        corrupt = bytearray(self.good)
        corrupt[fw.HEADER_SIZE + fw.ENTRY_SIZE + 2 * fw.PEB_SIZE + fw.DATA_OFFSET + 77] ^= 1
        self.assertIn('CRC mismatch', self.check(bytes(corrupt)))

    def test_rejects_ubi(self):
        ubi = bytearray(self.ubi)
        ubi[fw.PEB_SIZE + 8] ^= 1
        self.assertIn('EC header', self.check(fw.pack([(fw.APPFS_MTD, bytes(ubi))])))
        self.assertIn('squashfs', self.check(fw.pack([(fw.APPFS_MTD, sample_ubi())])))
        self.assertIn('whole number', self.check(fw.pack([(fw.APPFS_MTD, self.ubi[:-1])])))


if __name__ == '__main__':
    unittest.main()
