#!/usr/bin/env python3
"""Verify the standard root tree in every image the build produces.

Usage: python3 scripts/check_rootfs.py [out-dir]

novus.img   - writable NovusFS, original names
esp*.img    - FAT ESP, 8.3 aliases
novus-live.iso - primary tree (aliases) + Joliet tree (original names)
"""
import hashlib
import os
import re
import sys

from novusimg import FatFS, IsoFS, NovusFS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADER = os.path.join(ROOT, 'fs', 'novusfs', 'rootfs_layout.h')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'out')

with open(HEADER, 'r', encoding='utf-8') as fh:
    text = fh.read()

dirs_block = text.split('#define NOVUS_LAYOUT_DIRS(X)')[1].split(
    '/* Number of directory entries')[0]
DIRS = re.findall(r'X\("([^"]+)",\s*"([^"]+)",\s*(\d+)\)', dirs_block)
assert len(DIRS) == 77, 'layout directory count changed: %d' % len(DIRS)

meta_block = text.split('#define NOVUS_LAYOUT_META(X)')[1].split(
    '#define NOVUS_LAYOUT_MOUNTS(X)')[0]
METAS = re.findall(r'X\("([^"]+)",\s*"([^"]+)",\s*\w+\)', meta_block)

mount_block = text.split('#define NOVUS_LAYOUT_MOUNTS(X)')[1].split('#endif')[0]
MOUNTS = re.findall(r'X\("([^"]+)"', mount_block)

ALIAS = {path: alias for path, alias, _ in DIRS}


def alias_path(path):
    parts = path.split('/')
    prefix = []
    out = []
    for part in parts:
        prefix.append(part)
        out.append(ALIAS['/'.join(prefix)])
    return '/'.join(out)


errors = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        errors.append(what)


def load(name):
    path = os.path.join(OUT, name)
    if not os.path.exists(path):
        errors.append('%s is missing' % name)
        return None
    with open(path, 'rb') as fh:
        return fh.read()


def check_meta(content, label, expect_mounts=True):
    check(content is not None, '%s: metadata file missing' % label)
    if content is None:
        return
    body = content.decode('ascii', 'replace')
    check('fstype=novusfs' in body or '"fstype"' in body,
          '%s: bad metadata content' % label)
    if expect_mounts:
        for path in MOUNTS:
            if path != '/':
                check(path in body, '%s: missing mount path %s' % (label, path))


# ---- writable system partition -----------------------------------------
img = load('novus.img')
if img is not None:
    nv = NovusFS(img)
    for path, _, _ in DIRS:
        check(nv.is_dir(path), 'novus.img: missing directory /%s' % path)
    for path, _ in METAS:
        check(nv.lookup(path) != 0, 'novus.img: missing file /%s' % path)
    check_meta(nv.read_file('.novusfs/fsinfo'), 'novus.img fsinfo', False)
    check_meta(nv.read_file('.novusfs/mounts.json'), 'novus.img mounts.json')
    font = nv.read_file('Novus/fonts/font.bin')
    with open(os.path.join(ROOT, 'graphics', 'nd_font_data.bin'), 'rb') as fh:
        ref = fh.read()
    check(font is not None and hashlib.md5(font).hexdigest() ==
          hashlib.md5(ref).hexdigest(), 'novus.img: font payload mismatch')

# ---- FAT ESP template and live ESP --------------------------------------
for name in ('esp-tpl.img', 'esp.img'):
    data = load(name)
    if data is None:
        continue
    fat = FatFS(data)
    for path, _, _ in DIRS:
        check(fat.is_dir(alias_path(path)),
              '%s: missing FAT directory /%s' % (name, alias_path(path)))
    for path, alias in METAS:
        apath = alias_path(path.rsplit('/', 1)[0]) + '/' + alias
        check(fat.read_file(apath) is not None,
              '%s: missing FAT file /%s' % (name, apath))
    check(fat.read_file('NOVUSFS/FSINFO') is not None,
          '%s: fsinfo unreadable' % name)
    check_meta(fat.read_file('NOVUSFS/MOUNTS.JSO'), '%s mounts' % name)
    check(fat.read_file('EFI/BOOT/BOOTX64.EFI') is not None,
          '%s: UEFI boot file missing' % name)
    check(fat.read_file('SYSTEM.IMG') is not None,
          '%s: SYSTEM.IMG missing' % name)

# ---- ISO: primary aliases + Joliet original names ----------------------
iso = load('novus-live.iso')
if iso is not None:
    iso_fs = IsoFS(iso)
    check('joliet' in iso_fs.trees, 'ISO: Joliet SVD missing')
    check('primary' in iso_fs.trees, 'ISO: primary PVD missing')
    for path, _, _ in DIRS:
        check(iso_fs.is_dir(path, 'joliet'),
              'ISO Joliet: missing directory /%s' % path)
    for path, _, _ in DIRS:
        check(iso_fs.is_dir(alias_path(path), 'primary'),
              'ISO primary: missing directory /%s' % alias_path(path))
    for path, _ in METAS:
        check(iso_fs.lookup(path, 'joliet') is not None,
              'ISO Joliet: missing file /%s' % path)
    check_meta(iso_fs.read_file('.novusfs/fsinfo', 'joliet'),
               'ISO fsinfo', False)
    check_meta(iso_fs.read_file('.novusfs/mounts.json', 'joliet'),
               'ISO mounts.json')
    check(iso_fs.lookup('EFI/BOOT/BOOTX64.EFI', 'primary') is not None,
          'ISO primary: UEFI boot file missing')

print('check_rootfs: %d checks, %d errors' % (checks, len(errors)))
for err in errors[:30]:
    print('  ERROR', err)
if len(errors) > 30:
    print('  ... %d more' % (len(errors) - 30))
sys.exit(1 if errors else 0)
