#!/usr/bin/env python3
"""Regenerate the wallpaper JPEGs that go into NOVUS/WALLPAPERS.PKG.

The photos ship as one package file on the ESP (see tools/wpkg), so the
size budget is the medium, not the loader: the live ESP carries the package
twice - once for itself and once inside the install template the installer
writes to a target disk - and the whole ESP has to stay under the 32 MB
ceiling of the El Torito boot entry. With the loader, the system image and
the template accounted for, that leaves roughly 4 MB for seven photos.

So each photo is resampled to 2560 px on the long edge at quality 88 with
4:2:0 chroma: QHD, twice the 1280x800 mode QEMU hands out and above most
laptop panels, for about 3.5 MB in total. The originals in assets/backgrounds
stay where they are and stay untouched.

Usage (needs Pillow; not part of the normal build, like tools/themegen):

    python3 tools/wpgen/wpgen.py

The generated files are committed, so a normal build needs no Python.
"""

import os
import sys

SRC = "assets/backgrounds"
DST = "assets/backgrounds-screen"

# Long-edge target. The wallpaper is cover-scaled to the screen at paint
# time, so this only has to cover the largest mode we expect to run in.
MAX_EDGE = 2560
QUALITY = 88
SUBSAMPLING = 2  # 4:2:0, as the sources already are


def main():
    try:
        from PIL import Image
    except ImportError:
        sys.exit("wpgen: Pillow is required (pip install Pillow)")

    if not os.path.isdir(SRC):
        sys.exit("wpgen: run from the repository root (no %s/ here)" % SRC)
    os.makedirs(DST, exist_ok=True)

    names = sorted(n for n in os.listdir(SRC)
                   if n.lower().endswith((".jpg", ".jpeg")))
    if not names:
        sys.exit("wpgen: no JPEGs found in %s" % SRC)

    for name in names:
        src = os.path.join(SRC, name)
        dst = os.path.join(DST, name)
        with Image.open(src) as im:
            im = im.convert("RGB")
            w, h = im.size
            long_edge = max(w, h)
            if long_edge > MAX_EDGE:
                scale = MAX_EDGE / float(long_edge)
                im = im.resize((max(1, round(w * scale)),
                                max(1, round(h * scale))),
                               Image.LANCZOS)
            im.save(dst, "JPEG", quality=QUALITY, subsampling=SUBSAMPLING,
                    optimize=True)
        print("wpgen: %-42s %5dx%-5d %8d bytes -> %s"
              % (name, im.size[0], im.size[1], os.path.getsize(dst), dst))

    total = sum(os.path.getsize(os.path.join(DST, n)) for n in names)
    print("wpgen: %d images, %d bytes total" % (len(names), total))


if __name__ == "__main__":
    main()
