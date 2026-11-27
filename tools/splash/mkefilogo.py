#!/usr/bin/env python3
"""Bake assets/splash/boot.png into an EDK II GOP pixel array.

Unlike mkbootlogo.py, this generator keeps the PNG alpha channel: the EFI
splash composites the mark over whatever the firmware already painted instead
of covering it with an opaque white rectangle.

Usage: python3 tools/splash/mkefilogo.py [output header]
"""

import os
import sys

WIDTH = 320


def main():
    try:
        from PIL import Image
    except ImportError:
        sys.exit("mkefilogo: Pillow is required (pip install Pillow)")

    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    src = os.path.join(root, "assets", "splash", "boot.png")
    dst = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        root, "edk2", "NovusSplash", "LogoPixels.h")

    with Image.open(src) as image:
        image = image.convert("RGBA")
        bbox = image.getchannel("A").getbbox()
        if bbox:
            image = image.crop(bbox)
        w, h = image.size
        height = max(1, round(h * WIDTH / float(w)))
        image = image.resize((WIDTH, height), Image.LANCZOS)
        px = list(image.getdata())

    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".tmp"
    with open(tmp, "w") as out:
        out.write("/* Generated from assets/splash/boot.png - do not edit.\n")
        out.write(" * Regenerate with tools/splash/mkefilogo.py.\n")
        out.write(" * Pixels are EFI_GRAPHICS_OUTPUT_BLT_PIXEL {Blue,Green,Red,Alpha}.\n")
        out.write(" */\n")
        out.write("#ifndef NOVUS_EFI_LOGO_PIXELS_H\n")
        out.write("#define NOVUS_EFI_LOGO_PIXELS_H\n\n")
        out.write("#define NOVUS_EFI_LOGO_WIDTH  %d\n" % WIDTH)
        out.write("#define NOVUS_EFI_LOGO_HEIGHT %d\n\n" % height)
        out.write("STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL mNovusEfiLogo[\n")
        out.write("    NOVUS_EFI_LOGO_WIDTH * NOVUS_EFI_LOGO_HEIGHT\n")
        out.write("] = {\n")
        for i in range(0, len(px), 4):
            chunk = []
            for r, g, b, a in px[i:i + 4]:
                chunk.append("{%d,%d,%d,%d}" % (b, g, r, a))
            out.write("    " + ", ".join(chunk) + ",\n")
        out.write("};\n\n#endif /* NOVUS_EFI_LOGO_PIXELS_H */\n")
    os.replace(tmp, dst)
    print("efi logo: %dx%d -> %s (%d bytes)" % (
        WIDTH, height, dst, os.path.getsize(dst)))


if __name__ == "__main__":
    main()
