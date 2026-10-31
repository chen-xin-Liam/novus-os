#!/usr/bin/env python3
import struct, sys, hashlib

def extract(path, name):
    f = open(path, "rb")
    bpb = f.read(512)
    bps = struct.unpack_from("<H", bpb, 11)[0]
    spc = bpb[13]
    res = struct.unpack_from("<H", bpb, 14)[0]
    nfats = bpb[16]
    root_ent = struct.unpack_from("<H", bpb, 17)[0]
    fat_sz16 = struct.unpack_from("<H", bpb, 22)[0]
    fat_sz = fat_sz16
    root_sec = (root_ent * 32 + bps - 1) // bps
    fat_start = res
    data_start = fat_start + nfats * fat_sz + root_sec
    # locate name in root dir
    root_off = (fat_start + nfats * fat_sz) * bps
    up = name.upper()
    if "." in up:
        nm, ex = up.split(".", 1)
    else:
        nm, ex = up, ""
    name8 = nm.ljust(8)[:8].encode() + ex.ljust(3)[:3].encode()
    target = None
    for i in range(root_ent):
        f.seek(root_off + i*32)
        ent = f.read(32)
        if ent[0] == 0: break
        if ent[0] == 0xE5: continue
        if ent[11] & 0x0F: continue  # not a file
        if ent[0:11] == name8:
            target = ent
            break
    if target is None:
        print("NOT FOUND", name); return None
    first = struct.unpack_from("<H", target, 26)[0]
    size = struct.unpack_from("<I", target, 28)[0]
    print("found", name, "first_cluster", first, "size", size)
    # walk FAT
    fat_off = fat_start * bps
    def fat_next(c):
        f.seek(fat_off + c*2)
        v = struct.unpack_from("<H", f.read(2), 0)[0]
        return v
    out = bytearray()
    c = first
    guard = 0
    while c >= 2 and c < 0xFFF0 and guard < 200000:
        cl_off = data_start * bps + (c - 2) * spc * bps
        f.seek(cl_off)
        out += f.read(spc * bps)
        c = fat_next(c)
        guard += 1
    data = bytes(out[:size])
    return data

if __name__ == "__main__":
    img = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else "SYSTEM.IMG"
    data = extract(img, name)
    if data is None:
        sys.exit(1)
    h = hashlib.md5(data).hexdigest()
    print("extracted size", len(data), "md5", h)
    # compare with out/novus.img
    try:
        ref = open("out/novus.img", "rb").read()
        print("novus.img size", len(ref), "md5", hashlib.md5(ref).hexdigest())
        if data == ref:
            print("MATCH: extracted == out/novus.img")
        else:
            # find first diff
            n = min(len(data), len(ref))
            d = next((i for i in range(n) if data[i] != ref[i]), n)
            print("DIFFER at byte", d, "ext", data[d:d+16].hex(), "ref", ref[d:d+16].hex())
    except FileNotFoundError:
        print("no out/novus.img to compare")
