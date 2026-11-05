#!/usr/bin/env python3
"""Generate web build identity, updating the header only when values change.

An untagged checkout reports its version as 'development' rather than
'unknown': the commit is still identified by the hash, and releases are
refused without a tag by tools/build_release.py.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[1]
version = os.environ.get('MT11_VERSION') or 'development'
git_hash = os.environ.get('MT11_GIT_HASH') or 'unknown'
try:
    dirty = subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--'], cwd=repo,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 1
except OSError:
    dirty = False
if dirty:
    git_hash += '-dirty'
text = ('/* Generated at build time; do not edit. */\n'
        '#define CA_FIRMWARE_VERSION ' + json.dumps(version) + '\n'
        '#define CA_FIRMWARE_GIT_HASH ' + json.dumps(git_hash) + '\n')
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    with tempfile.NamedTemporaryFile(mode='w', dir=output.parent, delete=False) as temporary:
        temporary.write(text)
    os.replace(temporary.name, output)
