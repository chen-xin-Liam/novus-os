#!/usr/bin/env python3
"""Print the standard root tree of a NovusFS image (recursively)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novusimg import NovusFS, u32, u64

path = sys.argv[1] if len(sys.argv) > 1 else 'out/novus.img'
with open(path, 'rb') as fh:
    data = fh.read()
nv = NovusFS(data)
print('%s: block=%d inode_table@%d' % (path, nv.bs, nv.itbl))

count = 0


def walk(ino, prefix='/'):
    global count
    for child, name in nv.dirents(ino):
        if name in ('.', '..'):
            continue
        mode = nv.inode(child)['mode']
        print('  %s%s%s' % (prefix, name, '/' if mode == 2 else ''))
        count += 1
        if mode == 2:
            walk(child, prefix + name + '/')


walk(nv.root)
print('  %d entries' % count)
