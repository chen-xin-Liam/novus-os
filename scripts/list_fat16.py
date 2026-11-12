#!/usr/bin/env python3
"""List a FAT ESP image, including the mirrored root directory tree."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novusimg import FatFS

path = sys.argv[1] if len(sys.argv) > 1 else 'out/esp.img'
with open(path, 'rb') as fh:
    fat = FatFS(fh.read())
print('%s: bps=%d spc=%d reserved=%d fats=%d fat_sectors=%d' %
      (path, fat.bps, fat.spc, fat.res, fat.nf, fat.fat_sz))

count = 0


def walk(cluster, prefix):
    global count
    for nm, first, size, is_dir in fat.entries(cluster):
        name = nm[:8].decode('ascii', 'replace').rstrip()
        ext = nm[8:].decode('ascii', 'replace').rstrip()
        full = name + ('.' + ext if ext else '')
        print('  %s%s%s' % (prefix, full, '/' if is_dir else ''))
        count += 1
        if is_dir and first >= 2:
            walk(first, prefix + full + '/')


walk(0, '/')
print('  %d entries' % count)
