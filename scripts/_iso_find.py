#!/usr/bin/env python3
import sys, struct, hashlib

BLOCK = 4096
def r32(b,o): return struct.unpack_from('<I', b, o)[0]

iso_path = sys.argv[1] if len(sys.argv)>1 else 'out/novus-live.iso'
ref_path = sys.argv[2] if len(sys.argv)>2 else 'out/novus.img'

with open(ref_path,'rb') as f: ref = f.read()
with open(iso_path,'rb') as f: iso = f.read()

print('iso size=%d ref(novus.img) size=%d' % (len(iso), len(ref)))
ref_md5 = hashlib.md5(ref).hexdigest()
print('ref md5=%s' % ref_md5)

# search for ref as a contiguous substring in iso (both big, do it in chunks)
found = []
ref_len = len(ref)
step = 1024*1024
pos = 0
print('scanning for embedded novus.img ...')
while pos <= len(iso) - ref_len:
    idx = iso.find(ref[:4096], pos)  # find first block match first (cheaper)
    if idx < 0: break
    # verify full match
    if iso[idx:idx+ref_len] == ref:
        found.append(idx)
        print('  MATCH at iso offset %d (0x%x), sector %d' % (idx, idx, idx//2048))
        pos = idx + ref_len
    else:
        pos = idx + 1
print('matches: %d' % len(found))

# Also scan for NovusFS superblock magic at 2048-aligned offsets
print('scanning for NOVS superblocks at 2048-aligned offsets:')
seen = {}
for off in range(0, len(iso)-BLOCK, 2048):
    if r32(iso, off) == 0x4E4F5653:
        # read inode_tbl_start (offset 24) and inode2 size (block (2-1)//32+itbl)*4096 + 256 + 8
        itbl = struct.unpack_from('<Q', iso, off+24)[0]
        b2 = ( (2-1)//32 + itbl ) * BLOCK
        boff = b2 + ((2-1)%32)*128
        size = struct.unpack_from('<I', iso, boff+8)[0]
        key = (itbl, size)
        seen.setdefault(key, []).append(off)
for k,v in seen.items():
    print('  itbl=%d inode2_size=%d  occurrences=%d (first off=0x%x)' % (k[0], k[1], len(v), v[0]))
