import struct
BLK = 4096
INO_TBL = 130
INO_SIZE = 128
with open('out/novus.img','rb') as f:
    f.seek((INO_TBL)*BLK)
    tbl = f.read(BLK)
# inode 2 at offset (2-1)*INO_SIZE
off = (2-1)*INO_SIZE
mode, pad0, ino, size, nlinks, blocks = struct.unpack_from('<HHIIII', tbl, off)
print('font.bin inode: ino=%d mode=%d size=%d blocks=%d' % (ino, mode, size, blocks))
dblock = struct.unpack_from('<7I', tbl, off+32)
print('dblock=', dblock)
if size != 0:
    # read first data block and check it is non-zero
    blk = dblock[0]
    f.seek(blk*BLK)
    b0 = f.read(16)
    print('first data block %d head bytes:' % blk, b0.hex())
