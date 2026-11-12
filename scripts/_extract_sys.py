#!/usr/bin/env python3
"""Extract SYSTEM.IMG from the ISO and locate the font by path."""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novusimg import IsoFS, NovusFS

ISO = sys.argv[1] if len(sys.argv) > 1 else 'out/novus-live.iso'
OUT = 'out/sys_extracted.img'

with open(ISO, 'rb') as fh:
    iso = IsoFS(fh.read())
sysimg = iso.read_file('SYSTEM.IMG', 'primary')
assert sysimg is not None, 'SYSTEM.IMG not found in the primary ISO tree'
with open(OUT, 'wb') as fh:
    fh.write(sysimg)
print('extracted SYSTEM.IMG size=%d md5=%s' %
      (len(sysimg), hashlib.md5(sysimg).hexdigest()))

nv = NovusFS(sysimg)
font = nv.read_file('Novus/fonts/font.bin')
assert font is not None, '/Novus/fonts/font.bin missing'
print('font.bin(path): size=%d md5=%s' %
      (len(font), hashlib.md5(font).hexdigest()))
