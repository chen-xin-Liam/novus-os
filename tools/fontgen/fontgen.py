#!/usr/bin/env python3
"""Novus OS - full-Unicode 16x16 bitmap font generator.

The old generator produced one glyph per character that happened to appear in
the UI sources. That made every user file name, terminal output or pasted
string a hollow box. This version renders the whole of Unicode instead:

  unifont.hex         Basic Multilingual Plane (U+0000..U+FFFF)
  unifont_upper.hex   Supplementary planes (U+010000..U+10FFFF)

Both are the GNU Unifont "hex" builds, which are 16x16 or 8x16 1bpp bitmaps
released under the GNU GPL v2+ with the font embedding exception. Download:

  https://unifoundry.com/pub/unifont/unifont-16.0.04/font-builds/

Usage:
    python3 tools/fontgen/fontgen.py --bmp unifont.hex --upper unifont_upper.hex
    python3 tools/fontgen/fontgen.py --preview 中文 A

Outputs (all committed; the build never runs this script, never needs the
Unifont files and never needs a host font):

  graphics/nd_font_data.bin   packed 32-byte glyphs (incbin'd by nd_font_blob.S)
  graphics/nd_font_uni.c      sparse page index + per-codepoint advance table
  graphics/nd_font_uni.h
  apps/src/unicode_data.rs    the same advance table for the Rust side

Storage: pages of 256 codepoints. `g_uni_page[cp >> 8]` is a slot number or
0xFFFF; each slot has a 256-bit "has a glyph" map, a 256-bit "is 16 pixels
wide" map and a dense block of 32-byte glyphs. Lookup is: page -> slot ->
bit -> rank (popcount) -> glyph. No codepoint is special-cased.

Advance model (1x pixels): printable ASCII keeps the original 5x7 face and a
6 pixel advance. Every other codepoint advances by its Unifont cell width
(8 or 16). Codepoints with no glyph fall back to East Asian Width: wide and
fullwidth are 16, combining marks are 0, everything else is 8.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT_BIN = os.path.join(ROOT, "graphics", "nd_font_data.bin")
OUT_C = os.path.join(ROOT, "graphics", "nd_font_uni.c")
OUT_H = os.path.join(ROOT, "graphics", "nd_font_uni.h")
OUT_RS = os.path.join(ROOT, "apps", "src", "unicode_data.rs")

PAGES = 0x1100          # 0x110000 codepoints / 256
EMPTY = 0xFFFF
GLYPH_BYTES = 32        # 16 rows, two bytes each (MSB = leftmost column)
ASCII_ADV = 6

ZERO_WIDTH_CATEGORIES = ("Mn", "Me", "Cf")


def parse_hex(path):
    """codepoint -> list of 16 row values (16-bit, MSB leftmost)."""
    out = {}
    with open(path, "r", encoding="ascii", errors="ignore") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" not in line:
                continue
            code, bits = line.split(":", 1)
            try:
                cp = int(code, 16)
            except ValueError:
                continue
            if cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
                continue
            bits = bits.strip()
            if len(bits) == 64:            # 16x16: four hex digits a row
                wide = 16
                rows = [int(bits[i:i + 4], 16) for i in range(0, 64, 4)]
            elif len(bits) == 32:          # 8x16, left half of the cell
                wide = 8
                rows = [int(bits[i:i + 2], 16) << 8 for i in range(0, 32, 2)]
            else:
                continue
            out[cp] = (wide, rows)
    return out


def fallback_adv(cp):
    import unicodedata

    if 0x20 <= cp <= 0x7E:
        return ASCII_ADV
    ch = chr(cp)
    cat = unicodedata.category(ch)
    if cat in ZERO_WIDTH_CATEGORIES:
        return 0
    if unicodedata.east_asian_width(ch) in ("W", "F"):
        return 16
    return 8


def advance_table(glyphs):
    """Sorted (start, end, advance) ranges for every codepoint >= 0x80."""
    ranges = []

    def add(cp, adv):
        if ranges and ranges[-1][1] + 1 == cp and ranges[-1][2] == adv:
            ranges[-1] = (ranges[-1][0], cp, adv)
        else:
            ranges.append((cp, cp, adv))

    for cp in range(0x80, 0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            cp_adv = 0
        elif cp in glyphs:
            cp_adv = glyphs[cp][0]
        else:
            cp_adv = fallback_adv(cp)
        add(cp, cp_adv)
    return ranges


def pack(glyphs):
    """Sparse page layout + blob. Returns (pages, slots, bases, valid, wide,
    blob)."""
    pages = [EMPTY] * PAGES
    bases = []
    valid = bytearray()
    wide = bytearray()
    blob = bytearray()

    for page in range(PAGES):
        first, last = page * 256, page * 256 + 255
        members = [cp for cp in range(first, last + 1) if cp in glyphs]
        if not members:
            continue
        pages[page] = len(bases)
        bases.append(len(blob) // GLYPH_BYTES)
        vb = bytearray(32)
        wb = bytearray(32)
        for cp in members:
            idx = cp - first
            vb[idx >> 3] |= 1 << (idx & 7)
            if glyphs[cp][0] == 16:
                wb[idx >> 3] |= 1 << (idx & 7)
            for row in glyphs[cp][1]:
                blob.append((row >> 8) & 0xFF)
                blob.append(row & 0xFF)
        valid += vb
        wide += wb
    return pages, bases, valid, wide, blob


def write_bin(blob):
    with open(OUT_BIN, "wb") as fh:
        fh.write(blob)


def write_c(pages, bases, valid, wide, ranges):
    out = []
    out.append("/**")
    out.append(" * Novus OS - full-Unicode font index (generated, do not edit).")
    out.append(" *")
    out.append(" * Sparse pages: g_uni_page[cp >> 8] is a slot or 0xFFFF. Within a")
    out.append(" * slot g_uni_valid/g_uni_wide hold 256 bits each and g_uni_base")
    out.append(" * points at the start of the slot's dense glyph block in")
    out.append(" * nd_font_data.bin (32 bytes per glyph).")
    out.append(" *")
    out.append(" * Generated by tools/fontgen/fontgen.py from GNU Unifont")
    out.append(" * (16x16 and 8x16, GPLv2+ with the font embedding exception).")
    out.append(" */")
    out.append("")
    out.append('#include "nd_internal.h"')
    out.append('#include "nd_font_uni.h"')
    out.append("")

    out.append(f"const UINT16 g_uni_page_count = {PAGES}u;")
    out.append("")
    out.append("const UINT16 g_uni_page[] = {")
    for i in range(0, PAGES, 16):
        out.append("    " + ", ".join(f"0x{v:04X}" for v in pages[i:i + 16]) + ",")
    out.append("};")
    out.append("")

    slots = len(bases)
    out.append(f"const UINT16 g_uni_slot_count = {slots}u;")
    out.append("")
    out.append("const UINT32 g_uni_base[] = {")
    for i in range(0, slots, 12):
        out.append("    " + ", ".join(f"{v}u" for v in bases[i:i + 12]) + ",")
    out.append("};")
    out.append("")

    for name, data in (("g_uni_valid", valid), ("g_uni_wide", wide)):
        out.append(f"const UINT8 {name}[] = {{")
        for i in range(0, len(data), 24):
            out.append("    " + ", ".join(f"0x{v:02X}" for v in data[i:i + 24]) + ",")
        out.append("};")
        out.append("")

    out.append(f"const UINT16 g_uni_width_count = {len(ranges)}u;")
    out.append("")
    out.append("const nd_uni_range_t g_uni_width[] = {")
    for start, end, adv in ranges:
        out.append(f"    {{0x{start:04X}u, 0x{end:04X}u, {adv}u}},")
    out.append("};")
    out.append("")

    with open(OUT_C, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))


def write_h():
    with open(OUT_H, "w", encoding="utf-8") as fh:
        fh.write("""/**
 * Novus OS - full-Unicode font index (generated).
 *
 * Data lives in nd_font_uni.c (index) and nd_font_data.bin (glyphs).
 */

#ifndef NOVUS_ND_FONT_UNI_H
#define NOVUS_ND_FONT_UNI_H

#include "uefi.h"

#define ND_UNI_EMPTY 0xFFFFu
#define ND_UNI_GLYPH_BYTES 32u

typedef struct {
  UINT32 start;
  UINT32 end;
  UINT8  adv;
} nd_uni_range_t;

extern const UINT16 g_uni_page_count;
extern const UINT16 g_uni_page[];
extern const UINT16 g_uni_slot_count;
extern const UINT32 g_uni_base[];
extern const UINT8  g_uni_valid[];
extern const UINT8  g_uni_wide[];
extern const UINT16 g_uni_width_count;
extern const nd_uni_range_t g_uni_width[];

/* Packed glyph blob (nd_font_data.bin, incbin'd by nd_font_blob.S). */
extern const UINT8 g_font_data[];

/* 32 glyph bytes (16 rows, big-endian) or NULL. *wide is 16 or 8. */
const UINT8 *nd_glyph_find(UINT32 cp, UINT32 *wide);

/* Advance in pixels at 1x: 0 for combining marks, 6 for printable ASCII,
 * 8 or 16 for everything else. */
UINT32 nd_uni_adv(UINT32 cp);

#endif /* NOVUS_ND_FONT_UNI_H */
""")


def write_rs(ranges):
    out = []
    out.append("//! Full-Unicode advance table (generated by tools/fontgen/fontgen.py)")
    out.append("//!")
    out.append("//! Sorted inclusive ranges of codepoints >= U+0080 and their advance")
    out.append("//! at 1x, in pixels. Printable ASCII keeps the 5x7 face (6 px).")
    out.append("//! 0 = combining mark, 8 = half width, 16 = full width.")
    out.append("")
    out.append("pub static RANGES: [(u32, u32, u8); %d] = [" % len(ranges))
    for start, end, adv in ranges:
        out.append(f"    (0x{start:04X}, 0x{end:04X}, {adv}),")
    out.append("];")
    out.append("")
    out.append("pub fn adv(c: char) -> i16 {")
    out.append("    let cp = c as u32;")
    out.append("    if cp < 0x80 {")
    out.append("        return if cp >= 0x20 { 6 } else { 0 };")
    out.append("    }")
    out.append("    let mut lo = 0usize;")
    out.append("    let mut hi = RANGES.len();")
    out.append("    while lo < hi {")
    out.append("        let mid = (lo + hi) / 2;")
    out.append("        let (start, end, w) = RANGES[mid];")
    out.append("        if cp < start {")
    out.append("            hi = mid;")
    out.append("        } else if cp > end {")
    out.append("            lo = mid + 1;")
    out.append("        } else {")
    out.append("            return w as i16;")
    out.append("        }")
    out.append("    }")
    out.append("    8")
    out.append("}")
    out.append("")
    out.append("/// Advance at an arbitrary integer scale (0 is treated as 1).")
    out.append("pub fn adv_scaled(c: char, scale: i16) -> i16 {")
    out.append("    let s = if scale <= 0 { 1 } else { scale };")
    out.append("    adv(c) * s")
    out.append("}")
    with open(OUT_RS, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out))


def preview(chars):
    font = os.path.join(HERE, "unifont.hex")
    glyphs = parse_hex(font)
    for ch in chars:
        cp = ord(ch)
        if cp not in glyphs:
            print(f"--- {ch} U+{cp:04X}: missing")
            continue
        wide, rows = glyphs[cp]
        print(f"--- {ch} U+{cp:04X} ({wide}px)")
        for bits in rows:
            print("".join("#" if bits & (1 << (15 - x)) else "." for x in range(16)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bmp", default=os.path.join(HERE, "unifont.hex"))
    ap.add_argument("--upper", default=os.path.join(HERE, "unifont_upper.hex"))
    ap.add_argument("--preview", nargs="*")
    args = ap.parse_args()

    if args.preview:
        preview(args.preview)
        return

    if not os.path.exists(args.bmp):
        sys.exit(f"missing {args.bmp}; download the GNU Unifont hex build")

    glyphs = parse_hex(args.bmp)
    if os.path.exists(args.upper):
        glyphs.update(parse_hex(args.upper))

    ranges = advance_table(glyphs)
    pages, bases, valid, wide, blob = pack(glyphs)
    write_bin(blob)
    write_c(pages, bases, valid, wide, ranges)
    write_h()
    write_rs(ranges)
    print(f"fontgen: {len(glyphs)} glyphs, {len(bases)} pages, "
          f"{len(blob) // 1024} KiB glyphs, {len(ranges)} advance ranges")


main()
