#!/usr/bin/env python3
"""Symbolizes tsample output against the Alpine runtime root the musl bundle
was staged from. Prints self time, inclusive time and memcpy callers.
Usage: symbolize.py RUNTIME_ROOT STAGE_DIR SAMPLES"""
import collections
import os
import subprocess
import sys

rt, stage, samples_path = sys.argv[1:4]
samples, maps = [], []
for line in open(samples_path):
    if line.startswith('S '):
        samples.append([int(x, 16) for x in line.split()[1:]])
    elif line.startswith('M '):
        f = line.split()
        start, end = (int(x, 16) for x in f[1].split('-'))
        maps.append((start, end, int(f[3], 16), f[6] if len(f) > 6 else f[-1]))

bias = {}
for start, end, offset, path in maps:
    if offset == 0 and path not in bias:
        bias[path] = start


def local_file(path):
    name = os.path.basename(path)
    for d in ('usr/lib/gstreamer-1.0', 'usr/lib', 'lib', 'usr/bin'):
        candidate = os.path.join(rt, d, name)
        if os.path.exists(candidate):
            debug = os.path.join(rt, 'usr/lib/debug', d, name + '.debug')
            return debug if os.path.exists(debug) else candidate
    for d in ('bin', 'lib'):
        candidate = os.path.join(stage, d, name)
        if os.path.exists(candidate):
            return candidate
    return None


def locate(addr):
    for start, end, offset, path in maps:
        if start <= addr < end and path in bias:
            return path, addr - bias[path]
    return '?', addr


wanted = collections.defaultdict(set)
for s in samples:
    for i, addr in enumerate(s):
        path, rel = locate(addr)
        wanted[path].add(rel if i == 0 else rel - 1)

names = {}
for path, rels in wanted.items():
    obj = local_file(path) if path != '?' else None
    rels = sorted(rels)
    if not obj:
        for r in rels:
            names[(path, r)] = f'{os.path.basename(path)}+{r:#x}'
        continue
    out = subprocess.run(['llvm-symbolizer', '--obj', obj, '--functions=linkage', '--no-inlines',
                          '--demangle', '--output-style=GNU'],
                         input='\n'.join(hex(r) for r in rels), capture_output=True, text=True).stdout
    lines = out.split('\n')
    for i, r in enumerate(rels):
        fn = lines[2 * i] if 2 * i < len(lines) else '??'
        if fn in ('??', ''):
            fn = f'+{r:#x}'
        names[(path, r)] = f'{fn} [{os.path.basename(path)}]'


def frames(s):
    out = []
    for i, addr in enumerate(s):
        path, rel = locate(addr)
        out.append(names[(path, rel if i == 0 else rel - 1)])
    return out


n = len(samples)
self_count = collections.Counter()
incl = collections.Counter()
memcpy_callers = collections.Counter()
lib_self = collections.Counter()
for s in samples:
    f = frames(s)
    self_count[f[0]] += 1
    lib_self[f[0].rsplit('[', 1)[-1].rstrip(']')] += 1
    for name in set(f):
        incl[name] += 1
    if 'memcpy' in f[0] or 'memmove' in f[0]:
        memcpy_callers[' <- '.join(f[1:4])] += 1

print(f'{n} samples of one thread (100% = that thread\'s busy time)\n')
print('self time by library:')
for name, c in lib_self.most_common(12):
    print(f'  {100 * c / n:5.1f}%  {name}')
print('\nself time by function:')
for name, c in self_count.most_common(30):
    print(f'  {100 * c / n:5.1f}%  {name}')
print('\ninclusive (on stack) by function:')
for name, c in incl.most_common(45):
    print(f'  {100 * c / n:5.1f}%  {name}')
print('\nmemcpy/memmove callers (lr <- fp frames):')
for name, c in memcpy_callers.most_common(15):
    print(f'  {100 * c / n:5.1f}%  {name}')
