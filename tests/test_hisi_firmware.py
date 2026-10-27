#!/usr/bin/env python3
"""hisi appfs image: vendor container layout, CRC and UBI structure.

With HISI_VENDOR_ROOTFS set to an extracted camera rootfs, also upload the
image to the camera's own updater (/usr/hiface/update) under qemu-aarch64 in a
private user/net/pid namespace; it stops at the missing MTD device.
"""
import contextlib
import io
import os
from pathlib import Path
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
            fw.ubinize(b'\0' * (fw.APPFS_PEBS - 1) * fw.LEB_SIZE)


@unittest.skipUnless(os.access(MKSQUASHFS, os.X_OK), 'mksquashfs not built (tools/bootstrap_squashfs_tools.sh)')
class Appfs(unittest.TestCase):
    def build(self, directory, *extra):
        staging = Path(directory) / 'stage'
        (staging / 'sh').mkdir(parents=True)
        (staging / 'fconfigs').mkdir()
        (staging / 'sh/start.sh').write_text('#!/bin/sh\n')
        output = Path(directory) / 'appfs.img'
        subprocess.run([sys.executable, str(ROOT / 'tools/hisi_firmware.py'), 'appfs', str(staging),
                        str(output), '--mksquashfs', MKSQUASHFS, *extra],
                       check=True, stdout=subprocess.DEVNULL)
        return output

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


if __name__ == '__main__':
    unittest.main()
