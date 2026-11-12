import struct
data = open('out/novus.img', 'rb').read()
# block 130 == inode table block (itbl_start=130). inode 2 is at offset 128 in that block.
bo = 130 * 4096
print('file block130[0:16]:', data[bo:bo+16].hex())
print('file block130[128:144] (inode2 word0/size):', data[bo+128:bo+144].hex())
# smoke captured tmp-b130 = 48DBBF0F48DA29F2 BF0F48C8394CD829 (as big-endian-printed u64 pairs)
print('smoke tmp-b130 p0=0x48DBBF0F48DA29F2 p1=0xBF0F48C8394CD829')
# decode smoke p0 as LE u64
sm0 = 0x48DBBF0F48DA29F2
sm1 = 0xBF0F48C8394CD829
import struct
sb = struct.pack('<QQ', sm0, sm1)
print('smoke tmp-b130 bytes LE:', sb.hex())
# compare
print('MATCH block130[0:16]?', data[bo:bo+16].hex() == sb.hex())
# also dump first 3 inodes of file block130 as u64
for i in range(0, 128, 8):
    print('ino off', i, struct.unpack_from('<Q', data, bo+i)[0])
