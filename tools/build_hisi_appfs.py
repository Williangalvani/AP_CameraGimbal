#!/usr/bin/env python3
"""Stage the Hi3516DV500 appfs and write it as a vendor update image.

The image replaces the whole vendor appfs (mtd4). rcS runs sh/start.sh from
it, and S05Ubifs_Mount mounts the vendor parameter volume on fconfigs/, so
both must exist. Everything else lives in ap_cameragimbal/.

camera-app links the B040 MPP shared libraries, which are musl builds; the
rootfs has only glibc, so ap_cameragimbal/lib carries musl's loader and every
library camera-app needs. hisi-web stays static.
"""
import argparse
from pathlib import Path
import shutil
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import hisi_firmware  # noqa: E402

EM_AARCH64 = 183
PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
LIB_DIR = 'ap_cameragimbal/lib'
LOADER = 'ld-musl-aarch64.so.1'
INTERPRETER = f'/appfs/{LIB_DIR}/{LOADER}'
# musl's loader is also its libc and answers for this name itself
LIBC = 'libc.so'


def elf_info(path):
    """Return (interpreter or None, [DT_NEEDED names]) of an AArch64 ELF."""
    data = path.read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
        raise ValueError(f'{path}: not a 64-bit little-endian ELF')
    machine, = struct.unpack_from('<H', data, 18)
    if machine != EM_AARCH64:
        raise ValueError(f'{path}: not an AArch64 ELF')
    phoff, = struct.unpack_from('<Q', data, 32)
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    interpreter = None
    dynamic = None
    loads = []
    for index in range(phnum):
        kind, _, offset, vaddr, _, filesz, _, _ = struct.unpack_from(
            '<IIQQQQQQ', data, phoff + index * phentsize)
        if kind == PT_INTERP:
            interpreter = data[offset:offset + filesz].rstrip(b'\0').decode()
        elif kind == PT_DYNAMIC:
            dynamic = offset, filesz
        elif kind == PT_LOAD:
            loads.append((vaddr, offset, filesz))
    if dynamic is None:
        return interpreter, []
    entries = []
    for position in range(dynamic[0], dynamic[0] + dynamic[1], 16):
        tag, value = struct.unpack_from('<qQ', data, position)
        if tag == DT_NULL:
            break
        entries.append((tag, value))
    strtab = next(value for tag, value in entries if tag == DT_STRTAB)
    base = next(offset + strtab - vaddr for vaddr, offset, filesz in loads
                if vaddr <= strtab < vaddr + filesz)

    def string(offset):
        return data[base + offset:data.index(b'\0', base + offset)].decode()
    return interpreter, [string(value) for tag, value in entries if tag == DT_NEEDED]


def install(source, destination, mode):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)


def stage_libraries(camera_app, lib_dir, loader, staging):
    interpreter, needed = elf_info(camera_app)
    if interpreter != INTERPRETER:
        raise ValueError(f'{camera_app}: interpreter {interpreter}, expected {INTERPRETER}')
    install(loader, staging / LIB_DIR / LOADER, 0o755)
    staged = set()
    pending = [(camera_app, name) for name in needed]
    while pending:
        user, name = pending.pop()
        if name == LIBC or name in staged:
            continue
        source = lib_dir / name
        if not source.is_file():
            raise ValueError(f'{user}: needs {name}, which {lib_dir} lacks')
        interpreter, needed = elf_info(source)
        if interpreter is not None:
            raise ValueError(f'{source}: not a shared library')
        install(source, staging / LIB_DIR / name, 0o644)
        staged.add(name)
        pending.extend((source, dependency) for dependency in needed)


def stage(camera_app, web, lib_dir, loader, staging):
    if elf_info(web)[0] is not None:
        raise ValueError(f'{web}: dynamically linked; hisi-web must be static')
    packaging = ROOT / 'packaging/hisi'
    app = staging / 'ap_cameragimbal'
    install(packaging / 'start.sh', staging / 'sh/start.sh', 0o755)
    (staging / 'fconfigs').mkdir()
    install(packaging / 'service.sh', app / 'service.sh', 0o755)
    install(packaging / 'camera.ini', app / 'camera.ini.default', 0o644)
    (app / 'web.pass.default').write_bytes(b'ardupilot\n')
    stage_libraries(camera_app, lib_dir, loader, staging)
    install(camera_app, app / 'bin/camera-app', 0o755)
    install(web, app / 'bin/hisi-web', 0o755)
    webroot = ROOT / 'web/webroot'
    assets = sorted(path for path in webroot.rglob('*') if path.is_file())
    if not assets:
        raise ValueError(f'{webroot}: missing web assets')
    for path in assets:
        install(path, app / 'webroot' / path.relative_to(webroot), 0o644)
    for path in [staging, *staging.rglob('*')]:
        if path.is_dir():
            path.chmod(0o755)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('camera_app', type=Path)
    parser.add_argument('web', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--lib-dir', type=Path, required=True,
                        help='directory with the MPP shared libraries')
    parser.add_argument('--loader', type=Path, required=True,
                        help="the musl toolchain's libc.so, installed as " + LOADER)
    parser.add_argument('--mksquashfs', default='mksquashfs')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        staging = Path(temporary) / 'appfs'
        staging.mkdir()
        stage(args.camera_app, args.web, args.lib_dir, args.loader, staging)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        hisi_firmware.build_appfs(staging, args.output, args.mksquashfs)


if __name__ == '__main__':
    main()
