import os, struct, subprocess, sys

MAGICK = r"C:\Program Files\ImageMagick-7.1.2-Q16-HDRI\magick.exe"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "assets", "icons", "src")
OUT = os.path.join(ROOT, "assets", "icons")
SIZES = (32, 24, 20, 16)

def render(svg, px):
    raw = os.path.join(OUT, ".tmp.raw")
    subprocess.run([MAGICK, "-background", "none", "-density", "384", svg,
                    "-resize", "%dx%d" % (px, px), "-alpha", "on", "-depth", "8",
                    "bgra:" + raw], check=True)
    with open(raw, "rb") as f:
        data = f.read()
    os.remove(raw)
    if len(data) != px * px * 4:
        raise SystemExit("unexpected raw size for %s: %d" % (svg, len(data)))
    return data

def ico_image(px, bgra):
    mask_stride = ((px + 31) // 32) * 4
    xored = bytearray()
    for y in range(px - 1, -1, -1):
        xored += bgra[y * px * 4:(y + 1) * px * 4]
    and_mask = bytes(mask_stride * px)
    hdr = struct.pack("<IiiHHIIiiII", 40, px, px * 2, 1, 32, 0,
                      len(xored) + len(and_mask), 0, 0, 0, 0)
    return hdr + bytes(xored) + and_mask

def build(svg, out_path):
    images = [(px, ico_image(px, render(svg, px))) for px in SIZES]
    offset = 6 + 16 * len(images)
    entries = bytearray()
    payload = bytearray()
    for px, blob in images:
        entries += struct.pack("<BBBBHHII", px % 256, px % 256, 0, 0, 1, 32,
                               len(blob), offset + len(payload))
        payload += blob
    with open(out_path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        f.write(entries)
        f.write(payload)

def main():
    made = 0
    for name in sorted(os.listdir(SRC)):
        if not name.endswith(".svg"):
            continue
        svg = os.path.join(SRC, name)
        out = os.path.join(OUT, name[:-4] + ".ico")
        build(svg, out)
        made += 1
        print("%-12s %6d bytes" % (os.path.basename(out), os.path.getsize(out)))
    print("icons:", made)

main()