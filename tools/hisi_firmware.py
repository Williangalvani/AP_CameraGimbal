#!/usr/bin/env python3
"""Hi3516DV500 appfs image in the vendor update format.

The rootfs updater (/usr/hiface/update, HTTP :9091 POST /fwUpgrade) and the
vendor web uploader accept a container of raw MTD partition images: a 52-byte
header, 164-byte entries naming each /dev/mtdN with its offset, length and
CRC, the data, and a 28-byte trailer. The updater erases the whole partition,
nandwrites the entry and optionally reboots.

appfs is mtd4: a UBI image holding one dynamic volume with a squashfs (xz,
64 KiB blocks), attached as ubi1 and mounted through ubiblock1_0. The CRC
detects corruption; it is not a signature.
"""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

MAGIC = 0xabcabccb
MAGIC_END = 0xcbcbacba
TYPE_MTD = 0x42
HEADER_SIZE = 0x34
ENTRY_SIZE = 0xa4
TRAILER_SIZE = 0x1c
ENTRY_PATH_SIZE = 0x80
# updatetype.ini [UPDATE_SET] DEV_TYPE / SDK_TYPE / PLATFORM_TYPE on this camera.
DEVICE_CODE = (19, 2, 51)
APPFS_MTD = '/dev/mtd4'
# MX35LF1GE4AB SPI-NAND UBI geometry, as found on the camera.
PEB_SIZE = 0x20000
PAGE_SIZE = 0x800
VID_HDR_OFFSET = PAGE_SIZE
DATA_OFFSET = 2 * PAGE_SIZE
LEB_SIZE = PEB_SIZE - DATA_OFFSET
APPFS_PEBS = 288
# The volume size this camera's UBI and the vendor's own updates give appfs.
APPFS_VOLUME_MAX_PEBS = 265
LAYOUT_VOLUME_ID = 0x7fffefff
VTBL_SLOTS = 128
VTBL_RECORD_SIZE = 172
UBI_VID_DYNAMIC = 1
UBI_COMPAT_REJECT = 5


def _vendor_table():
    table = []
    for index in range(256):
        value = index
        for _ in range(8):
            value = (value >> 1) ^ 0xbdb083e0 if value & 1 else value >> 1
        table.append(value)
    return table


VENDOR_TABLE = _vendor_table()


def vendor_crc(data):
    """Reflected CRC-32 with the vendor's polynomial, seed 0xcb, no final xor."""
    crc = 0xcb
    table = VENDOR_TABLE
    for byte in data:
        crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8)
    return crc


def ubi_crc(data):
    return zlib.crc32(data) ^ 0xffffffff


def _ec_header(image_seq):
    header = struct.pack('>4sB3xQIII32x', b'UBI#', 1, 0, VID_HDR_OFFSET, DATA_OFFSET, image_seq)
    return header + struct.pack('>I', ubi_crc(header))


def _vid_header(volume_id, lnum, compat):
    header = struct.pack('>4sBBBBII4xIIII4xQ12x', b'UBI!', 1, UBI_VID_DYNAMIC, 0, compat,
                         volume_id, lnum, 0, 0, 0, 0, 0)
    return header + struct.pack('>I', ubi_crc(header))


def _peb(image_seq, volume_id, lnum, compat, data):
    peb = bytearray(b'\xff' * PEB_SIZE)
    peb[0:64] = _ec_header(image_seq)
    peb[VID_HDR_OFFSET:VID_HDR_OFFSET + 64] = _vid_header(volume_id, lnum, compat)
    peb[DATA_OFFSET:DATA_OFFSET + len(data)] = data
    return bytes(peb)


def _vtbl_record(reserved_pebs=0, name=b''):
    record = struct.pack('>IIIBBH128sB23x', reserved_pebs, 1 if name else 0, 0,
                         UBI_VID_DYNAMIC if name else 0, 0, len(name), name, 0)
    return record + struct.pack('>I', ubi_crc(record))


def ubinize(volume, name=b'appfs'):
    """A ubinize-equivalent image: layout volume in PEBs 0-1, volume 0 after."""
    lebs = max(1, -(-len(volume) // LEB_SIZE))
    if lebs > APPFS_VOLUME_MAX_PEBS:
        raise ValueError(f'{len(volume)} bytes do not fit the appfs partition')
    image_seq = int.from_bytes(hashlib.sha256(volume).digest()[:4], 'big') or 1
    vtbl = _vtbl_record(lebs, name) + b''.join(_vtbl_record() for _ in range(VTBL_SLOTS - 1))
    pebs = [_peb(image_seq, LAYOUT_VOLUME_ID, lnum, UBI_COMPAT_REJECT, vtbl) for lnum in (0, 1)]
    for lnum in range(lebs):
        pebs.append(_peb(image_seq, 0, lnum, 0, volume[lnum * LEB_SIZE:(lnum + 1) * LEB_SIZE]))
    return b''.join(pebs)


def pack(entries, device_code=DEVICE_CODE, reboot=True):
    """entries: [(mtd path, data)]. The CRC covers each entry's data."""
    table_end = HEADER_SIZE + ENTRY_SIZE * len(entries)
    offset = table_end
    table = b''
    for path, data in entries:
        encoded = path.encode()
        if not encoded.startswith(b'/dev/mtd') or len(encoded) >= ENTRY_PATH_SIZE:
            raise ValueError(f'Invalid MTD path {path!r}')
        table += struct.pack('<128sII8xI16x', encoded, len(data), offset, vendor_crc(data))
        offset += len(data)
    total = offset + TRAILER_SIZE
    dev_type, sdk_type, platform_type = device_code
    header = struct.pack('<IIIIIIIB19xI', MAGIC, TYPE_MTD, dev_type, sdk_type, platform_type,
                         len(entries), total, 1 if reboot else 0, MAGIC_END)
    trailer = struct.pack('<I20xI', MAGIC, MAGIC_END)
    return header + table + b''.join(data for _, data in entries) + trailer


def unpack(contents, device_code=DEVICE_CODE):
    """The rootfs updater's checks; returns (reboot, [(path, data)])."""
    if len(contents) < HEADER_SIZE + TRAILER_SIZE:
        raise ValueError('Image is too short')
    magic, kind, dev_type, sdk_type, platform_type, count, total, reboot = \
        struct.unpack_from('<IIIIIIIB', contents)
    magic_end, = struct.unpack_from('<I', contents, 0x30)
    trailer, trailer_end = struct.unpack_from('<I20xI', contents, len(contents) - TRAILER_SIZE)
    if (magic, magic_end, trailer, trailer_end) != (MAGIC, MAGIC_END, MAGIC, MAGIC_END):
        raise ValueError('Bad header or trailer magic')
    if total != len(contents):
        raise ValueError('File length does not match the header')
    if kind != TYPE_MTD:
        raise ValueError(f'Unsupported update type {kind:#x}')
    if (dev_type, sdk_type, platform_type) != tuple(device_code):
        raise ValueError(f'Device code {dev_type}/{sdk_type}/{platform_type} does not match')
    entries = []
    table_end = HEADER_SIZE + count * ENTRY_SIZE
    for index in range(count):
        base = HEADER_SIZE + index * ENTRY_SIZE
        if base + ENTRY_SIZE > len(contents) - TRAILER_SIZE:
            raise ValueError('Entry table overruns the image')
        raw_path, length, offset, crc = struct.unpack_from('<128sII8xI', contents, base)
        path = raw_path.split(b'\0', 1)[0].decode(errors='replace')
        if not path.startswith('/dev/mtd') or '\\' in path:
            raise ValueError(f'Invalid MTD path {path!r}')
        if offset < table_end:
            raise ValueError(f'{path} data overlaps the entry table')
        if offset + length > len(contents) - TRAILER_SIZE:
            raise ValueError(f'{path} data overruns the image')
        data = contents[offset:offset + length]
        if vendor_crc(data) != crc:
            raise ValueError(f'{path} CRC mismatch')
        entries.append((path, data))
    return bool(reboot), entries


def check_ubi(image):
    """Header CRCs, geometry and the volume table of a ubinize() image."""
    if len(image) % PEB_SIZE or len(image) // PEB_SIZE < 3:
        raise ValueError('UBI image is not a whole number of PEBs')
    seqs = set()
    for index in range(len(image) // PEB_SIZE):
        peb = image[index * PEB_SIZE:(index + 1) * PEB_SIZE]
        ec, vid = peb[:64], peb[VID_HDR_OFFSET:VID_HDR_OFFSET + 64]
        if ec[:4] != b'UBI#' or ubi_crc(ec[:60]) != struct.unpack('>I', ec[60:])[0]:
            raise ValueError(f'PEB {index}: bad EC header')
        if struct.unpack_from('>II', ec, 16) != (VID_HDR_OFFSET, DATA_OFFSET):
            raise ValueError(f'PEB {index}: wrong header offsets')
        seqs.add(struct.unpack_from('>I', ec, 24)[0])
        if vid[:4] != b'UBI!' or ubi_crc(vid[:60]) != struct.unpack('>I', vid[60:])[0]:
            raise ValueError(f'PEB {index}: bad VID header')
    if len(seqs) != 1:
        raise ValueError('Mixed UBI image sequence numbers')
    vtbl = image[DATA_OFFSET:DATA_OFFSET + VTBL_SLOTS * VTBL_RECORD_SIZE]
    for slot in range(VTBL_SLOTS):
        record = vtbl[slot * VTBL_RECORD_SIZE:(slot + 1) * VTBL_RECORD_SIZE]
        if ubi_crc(record[:168]) != struct.unpack('>I', record[168:])[0]:
            raise ValueError(f'Volume table record {slot}: bad CRC')
    reserved, = struct.unpack_from('>I', vtbl)
    if not len(image) // PEB_SIZE - 2 <= reserved <= APPFS_VOLUME_MAX_PEBS:
        raise ValueError('Volume 0 size does not fit the image and the appfs partition')


MAX_IMAGE_SIZE = HEADER_SIZE + ENTRY_SIZE + APPFS_PEBS * PEB_SIZE + TRAILER_SIZE


def check_appfs_image(contents, device_code=DEVICE_CODE):
    """What hisi-web accepts (include/apcam/hisi_firmware.h check_appfs_image()):
    unpack()'s checks plus a single rebooting /dev/mtd4 entry holding a squashfs
    volume, so the image cannot rewrite the rootfs that carries the updater."""
    if len(contents) < HEADER_SIZE + ENTRY_SIZE + TRAILER_SIZE:
        raise ValueError('Image is too short')
    if len(contents) > MAX_IMAGE_SIZE:
        raise ValueError('Image is larger than the appfs partition')
    count, = struct.unpack_from('<I', contents, 0x14)
    if count != 1:
        raise ValueError('Image must contain exactly one partition (appfs)')
    if contents[0x1c] != 1:
        raise ValueError('Image does not reboot after writing')
    if contents[0x1e] != 0:
        raise ValueError('Unsupported header flags')
    raw_path = contents[HEADER_SIZE:HEADER_SIZE + ENTRY_PATH_SIZE]
    if b'\0' not in raw_path or raw_path.split(b'\0', 1)[0] != APPFS_MTD.encode():
        raise ValueError(f'Image does not target the appfs partition ({APPFS_MTD})')
    length, offset = struct.unpack_from('<II', contents, HEADER_SIZE + ENTRY_PATH_SIZE)
    if offset != HEADER_SIZE + ENTRY_SIZE or offset + length != len(contents) - TRAILER_SIZE:
        raise ValueError('Partition data does not fill the image')
    _, ((_, image),) = unpack(contents, device_code)
    check_ubi(image)
    if image[2 * PEB_SIZE + DATA_OFFSET:2 * PEB_SIZE + DATA_OFFSET + 4] != b'hsqs':
        raise ValueError('appfs volume is not a little-endian squashfs')
    return image


def squashfs(staging, output, mksquashfs):
    subprocess.run([mksquashfs, str(staging), str(output), '-noappend', '-comp', 'xz',
                    '-b', '65536', '-all-root', '-no-xattrs', '-mkfs-time', '0',
                    '-all-time', '0', '-quiet'], check=True)


def build_appfs(staging, output, mksquashfs='mksquashfs', reboot=True):
    with tempfile.TemporaryDirectory() as temporary:
        volume = Path(temporary) / 'appfs.squashfs'
        squashfs(staging, volume, mksquashfs)
        build_volume(volume.read_bytes(), output, reboot)


def build_volume(volume, output, reboot=True):
    if volume[:4] != b'hsqs':
        raise ValueError('appfs volume is not a little-endian squashfs')
    image = ubinize(volume)
    check_ubi(image)
    contents = pack([(APPFS_MTD, image)], reboot=reboot)
    unpack(contents)
    output.write_bytes(contents)
    print(f'{output}: {APPFS_MTD} {len(image) // PEB_SIZE} PEBs, {len(contents)} bytes')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    appfs = sub.add_parser('appfs', help='package a staged appfs tree')
    appfs.add_argument('staging', type=Path)
    appfs.add_argument('output', type=Path)
    appfs.add_argument('--mksquashfs', default='mksquashfs')
    appfs.add_argument('--no-reboot', action='store_true')
    volume = sub.add_parser('volume', help='package an existing appfs squashfs, '
                            'for example the camera\'s original one to restore it')
    volume.add_argument('squashfs', type=Path)
    volume.add_argument('output', type=Path)
    volume.add_argument('--no-reboot', action='store_true')
    check = sub.add_parser('check', help='validate an image as the web UI does: appfs only, '
                           'so it is also safe to send to the rootfs updater on :9091')
    check.add_argument('image', type=Path)
    args = parser.parse_args()
    if args.command == 'appfs':
        build_appfs(args.staging, args.output, args.mksquashfs, reboot=not args.no_reboot)
    elif args.command == 'volume':
        build_volume(args.squashfs.read_bytes(), args.output, reboot=not args.no_reboot)
    else:
        try:
            image = check_appfs_image(args.image.read_bytes())
        except ValueError as error:
            raise SystemExit(f'{args.image}: rejected: {error}')
        print(f'{args.image}: {APPFS_MTD} {len(image) // PEB_SIZE} PEBs, reboots after update')


if __name__ == '__main__':
    main()
