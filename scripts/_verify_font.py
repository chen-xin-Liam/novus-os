#!/usr/bin/env python3
"""Compare the font stored in a NovusFS image with the build input."""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novusimg import NovusFS

BASE = sys.argv[1] if len(sys.argv) > 1 else 'out'
FONT_PATH = 'Novus/fonts/font.bin'


def analyze(path, label):
    print('=== %s (%s) ===' % (label, path))
    if not os.path.exists(path):
        print('  MISSING')
        return None
    with open(path, 'rb') as fh:
        data = fh.read()
    nv = NovusFS(data)
    ino = nv.lookup(FONT_PATH)
    if not ino:
        print('  MISSING /%s' % FONT_PATH)
        return None
    node = nv.inode(ino)
    print('  inode=%d size=%d blocks=%d direct=%s iblk=%d diblk=%d' %
          (ino, node['size'], node['blocks'], node['direct'], node['iblk'],
           node['diblk']))
    font = nv.read_file(FONT_PATH)
    print('  first16=%s md5=%s' %
          (font[:16].hex(), hashlib.md5(font).hexdigest()))
    return font


image_font = analyze(os.path.join(BASE, 'novus.img'), 'novus.img')
with open('graphics/nd_font_data.bin', 'rb') as fh:
    ref = fh.read()
print('=== graphics/nd_font_data.bin: %d bytes md5=%s ===' %
      (len(ref), hashlib.md5(ref).hexdigest()))
if image_font is None:
    sys.exit(1)
print('MATCH' if image_font == ref else 'MISMATCH')
sys.exit(0 if image_font == ref else 1)
