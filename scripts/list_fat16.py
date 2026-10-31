#!/usr/bin/env python3
import struct, sys
f = open(sys.argv[1], "rb")
bpb = f.read(512)
bps = struct.unpack_from("<H", bpb, 11)[0]
spc = bpb[13]
res = struct.unpack_from("<H", bpb, 14)[0]
nfats = bpb[16]
root_ent = struct.unpack_from("<H", bpb, 17)[0]
fat_sz = struct.unpack_from("<H", bpb, 22)[0]
print("bps", bps, "spc", spc, "res", res, "nfats", nfats, "root_ent", root_ent, "fat_sz", fat_sz)
root_off = (res + nfats * fat_sz) * bps
print("root_off(byte)", root_off)
for i in range(root_ent):
    f.seek(root_off + i * 32)
    ent = f.read(32)
    if ent[0] == 0:
        break
    if ent[0] == 0xE5:
        continue
    if ent[11] & 0x08:
        continue
    nm = ent[0:11]
    first = struct.unpack_from("<H", ent, 26)[0]
    size = struct.unpack_from("<I", ent, 28)[0]
    print(i, repr(nm), "first", first, "size", size)
