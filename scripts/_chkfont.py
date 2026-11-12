#!/usr/bin/env python3
"""Inspect the font inode in a NovusFS image by its canonical path."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novusimg import NovusFS

path = sys.argv[1] if len(sys.argv) > 1 else 'out/novus.img'
with open(path, 'rb') as fh:
    nv = NovusFS(fh.read())
ino = nv.lookup('Novus/fonts/font.bin')
assert ino, '/Novus/fonts/font.bin not found'
node = nv.inode(ino)
print('font.bin inode=%d mode=%d size=%d blocks=%d' %
      (node['ino'], node['mode'], node['size'], node['blocks']))
print('direct=%s iblk=%d diblk=%d' %
      (node['direct'], node['iblk'], node['diblk']))
font = nv.read_file('Novus/fonts/font.bin')
print('first data bytes: %s' % font[:16].hex())
