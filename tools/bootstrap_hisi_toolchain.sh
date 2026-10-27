#!/bin/sh
# Install local build inputs; no system installation and no camera access.
# The B040 MPP libraries are musl builds, so the hisi backend links with musl.
set -eu
cd "$(dirname "$0")/.."
deps="$PWD/build/hisi-deps"
name=aarch64--musl--stable-2025.08-1
url="https://toolchains.bootlin.com/downloads/releases/toolchains/aarch64/tarballs/$name.tar.xz"
mkdir -p "$deps"
if [ ! -f "$deps/toolchain.tar.xz" ]; then
    curl -fL --retry 2 "$url" -o "$deps/toolchain.tar.xz.part"
    mv "$deps/toolchain.tar.xz.part" "$deps/toolchain.tar.xz"
fi
python3 - "$deps" "$PWD" "$name" <<'PY'
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile
root = Path(sys.argv[1])
repo = Path(sys.argv[2])
archive = root / "toolchain.tar.xz"
expected = "defba831ffa1175236f137069333e21ed46d4d19feb5080a90cf248b6fc2cb08"
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("toolchain SHA-256 mismatch")
target = root / sys.argv[3]
stamp = target / ".apcam-sha256"
def remove_path(path):
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)
if not target.is_dir() or not stamp.is_file() or stamp.read_text().strip() != expected:
    sys.path.insert(0, str(repo / "tools"))
    from safe_tar import safe_extract
    backup = root / (target.name + ".old")
    remove_path(backup)
    with tempfile.TemporaryDirectory(prefix="hisi-toolchain-", dir=root) as temporary:
        temporary = Path(temporary)
        safe_extract(archive, temporary)
        staged = temporary / target.name
        if not staged.is_dir():
            raise SystemExit("toolchain archive has no expected top-level directory")
        (staged / ".apcam-sha256").write_text(expected + "\n")
        had_target = target.exists()
        if had_target:
            os.replace(target, backup)
        try:
            os.replace(staged, target)
        except Exception:
            if had_target and backup.exists():
                os.replace(backup, target)
            raise
    remove_path(backup)
print(target)
PY
