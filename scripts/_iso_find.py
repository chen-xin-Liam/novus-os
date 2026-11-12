#!/usr/bin/env python3
"""Find embedded NovusFS images inside the ISO."""
import hashlib
import struct
import sys

BLOCK = 4096


def r32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def r64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


iso_path = sys.argv[1] if len(sys.argv) > 1 else 'out/novus-live.iso'
ref_path = sys.argv[2] if len(sys.argv) > 2 else 'out/novus.img'
with open(ref_path, 'rb') as fh:
    ref = fh.read()
with open(iso_path, 'rb') as fh:
    iso = fh.read()

print('iso size=%d ref(novus.img) size=%d' % (len(iso), len(ref)))
print('ref md5=%s' % hashlib.md5(ref).hexdigest())

found = []
pos = 0
while pos <= len(iso) - len(ref):
    idx = iso.find(ref[:BLOCK], pos)
    if idx < 0:
        break
    if iso[idx:idx + len(ref)] == ref:
        found.append(idx)
        print('  MATCH at iso offset %d (0x%x), sector %d' %
              (idx, idx, idx // 2048))
        pos = idx + len(ref)
    else:
        pos = idx + 1
print('matches: %d' % len(found))

# Group superblocks by geometry, not by "inode 2 is font.bin": the standard
# root tree occupies the first ~80 inodes.
print('scanning for NOVS superblocks at 2048-aligned offsets:')
seen = {}
for off in range(0, len(iso) - BLOCK, 2048):
    if r32(iso, off) == 0x4E4F5653:
        itbl = r64(iso, off + 24)
        total = r64(iso, off + 12)
        # Random data can contain the magic; only plausible geometry is a
        # real superblock (an 8 MiB image has 2048 4 KiB blocks).
        if 0 < itbl < 100000 and 0 < total < 100000000:
            seen.setdefault((itbl, total), []).append(off)
for (itbl, total), offsets in seen.items():
    print('  itbl=%d total_blocks=%d occurrences=%d (first off=0x%x)' %
          (itbl, total, len(offsets), offsets[0]))
