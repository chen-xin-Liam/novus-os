#!/usr/bin/env python3
import sys, struct

BLOCK_SIZE = 4096
INODE_SIZE = 128
NDIR = 7

def r16(b, o): return struct.unpack_from('<H', b, o)[0]
def r32(b, o): return struct.unpack_from('<I', b, o)[0]
def r64(b, o): return struct.unpack_from('<Q', b, o)[0]

def analyze(path, label):
    print('=== %s (%s) ===' % (label, path))
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except FileNotFoundError:
        print('  MISSING')
        return None
    print('  file size = %d bytes (%d blocks)' % (len(data), len(data)//BLOCK_SIZE))
    if r32(data, 0) != 0x4E4F5653:
        print('  NO MAGIC: %08X' % r32(data, 0)); return None
    # superblock
    sb = data[:BLOCK_SIZE]
    magic = r32(sb, 0)
    version = r32(sb, 4)
    block_size = r32(sb, 8)
    total_blocks = r64(sb, 12)
    inode_tbl_start = r64(sb, 24)   # offset of inode_tbl_start in superblock
    print('  magic=NOVS version=0x%08X block_size=%d total_blocks=%d inode_tbl_start=%d'
          % (version, block_size, total_blocks, inode_tbl_start))
    # read inode 2
    ino = 2
    b = (ino - 1)//32 + inode_tbl_start
    off = ((ino - 1) % 32) * INODE_SIZE
    boff = b*block_size + off
    inb = data[boff:boff+INODE_SIZE]
    mode = r16(inb, 0)
    inum = r32(inb, 4)
    size = r32(inb, 8)
    blocks = r32(inb, 16)
    dblock = [r64(inb, 32 + i*8) for i in range(NDIR)]
    iblk = r64(inb, 32 + NDIR*8)
    diblk = r64(inb, 32 + (NDIR+1)*8)
    print('  inode2: ino=%d mode=%d(%s) size=%d blocks=%d'
          % (inum, mode, 'file' if mode==1 else ('dir' if mode==2 else 'free'), size, blocks))
    print('  inode2 dblock=%s iblk=%d diblk=%d' % (dblock, iblk, diblk))
    # Try to read first 16 bytes of the file data
    def read_block(blk):
        o = blk*block_size
        return data[o:o+block_size]
    first = b''
    if size > 0:
        # gather first block: prefer dblock[0]; if 0, try indirect
        if dblock[0] != 0:
            first = read_block(dblock[0])[:16]
        elif iblk != 0:
            ind = read_block(iblk)
            b0 = r64(ind, 0)
            if b0 != 0:
                first = read_block(b0)[:16]
        print('  first16 = %s' % first[:16].hex())
    return {'size': size, 'blocks': blocks, 'dblock': dblock, 'iblk': iblk}

if __name__ == '__main__':
    base = sys.argv[1] if len(sys.argv) > 1 else 'out'
    a = analyze(base + '/novus.img', 'out/novus.img')
    # reference font data
    try:
        with open('graphics/nd_font_data.bin', 'rb') as f:
            ref = f.read(16)
        print('=== graphics/nd_font_data.bin first16 = %s (%d bytes) ===' % (ref.hex(),
              len(open('graphics/nd_font_data.bin','rb').read())))
    except Exception as e:
        print('ref read error', e)
