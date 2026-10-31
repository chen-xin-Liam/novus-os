import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "out/novus.img"
with open(path, "rb") as f:
    sb = f.read(4096)
    bs, = struct.unpack_from("<I", sb, 8)
    itbl_start, = struct.unpack_from("<Q", sb, 24)
    print("bs", bs, "itbl_start", itbl_start)
    f.seek(itbl_start * bs)
    ino = f.read(128)
    mode, = struct.unpack_from("<H", ino, 0)
    size, = struct.unpack_from("<I", ino, 8)
    blocks, = struct.unpack_from("<I", ino, 16)
    # dblock[0] actually lives at offset 28 (blocks@16..19, mtime@20..27)
    dblock = struct.unpack_from("<7Q", ino, 28)
    print("root mode", mode, "size", size, "blocks", blocks, "dblock", dblock)
    db = dblock[0]
    if db:
        f.seek(db * bs)
        blk = f.read(bs)
        off = 0
        while off + 64 <= bs:
            ino_i, rec, namelen = struct.unpack_from("<IHH", blk, off)
            if ino_i == 0:
                break
            name = blk[off+8:off+8+namelen]
            print("  entry ino", ino_i, "name", name.decode("latin1", ""))
            off += rec
