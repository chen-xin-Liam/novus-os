import struct, sys
data = open('out/novus.img', 'rb').read()
itbl = struct.unpack_from('<Q', data, 24)[0]
print('itbl', itbl)
b = (2 - 1) // 32 + itbl
off = ((2 - 1) % 32) * 128
bo = b * 4096 + off
inb = data[bo:bo + 128]
print('inode2 bytes:', inb.hex())
for i in range(0, 128, 8):
    print(i, struct.unpack_from('<Q', inb, i)[0])
