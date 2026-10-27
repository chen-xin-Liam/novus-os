#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# tools/embed-icon.py -- embed nuomi.ico into a Windows .exe (RT_GROUP_ICON / RT_ICON).
# Zero extra dependencies: uses ctypes + kernel32 UpdateResourceW.
# Works on TCC-built exes that have no .rsrc section (UpdateResource creates it).
#
# Usage:
#   python tools/embed-icon.py <target.exe> [icon.ico]
# Default icon: <repo root>/nuomi.ico
import os, sys, struct, ctypes

RT_ICON = 3
RT_GROUP_ICON = 14
LANG_NEUTRAL = 0

def fail(msg):
    print("embed-icon:", msg, file=sys.stderr)
    sys.exit(1)

def parse_ico(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x00\x00\x01\x00":
        fail("not a .ico file: " + path)
    (n,) = struct.unpack_from("<H", d, 4)
    entries = []
    for i in range(n):                       # ICONDIRENTRY = 16 bytes
        o = 6 + i * 16
        w, h, c, r, planes, bpp, size, dataoff = struct.unpack_from("<BBBBHHII", d, o)
        entries.append(dict(w=w or 256, h=h or 256, c=c, planes=planes, bpp=bpp,
                            size=size, data=d[dataoff:dataoff + size]))
    return entries

def build_group(entries):
    # NEWHEADER: reserved(0) + type(1=icon) + count
    out = struct.pack("<HHH", 0, 1, len(entries))
    # GRPICONDIRENTRY = 14 bytes (id replaces imageOffset)
    for i, e in enumerate(entries):
        out += struct.pack("<BBBBHHIH",
                           e["w"] & 0xFF, e["h"] & 0xFF, e["c"], 0,
                           e["planes"], e["bpp"], e["size"], i + 1)
    return out

def embed(exe_path, ico_path):
    entries = parse_ico(ico_path)
    group = build_group(entries)

    k = ctypes.windll.kernel32
    k.BeginUpdateResourceW.argtypes = [ctypes.c_wchar_p, ctypes.c_int]
    k.BeginUpdateResourceW.restype = ctypes.c_void_p
    k.UpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_ushort, ctypes.c_void_p, ctypes.c_uint]
    k.UpdateResourceW.restype = ctypes.c_int
    k.EndUpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_int]
    k.EndUpdateResourceW.restype = ctypes.c_int

    h = k.BeginUpdateResourceW(exe_path, 0)
    if not h:
        fail("BeginUpdateResourceW failed (err=%d)" % ctypes.GetLastError())
    for i, e in enumerate(entries):
        buf = ctypes.create_string_buffer(e["size"])
        ctypes.memmove(buf, e["data"], e["size"])
        if not k.UpdateResourceW(h, RT_ICON, i + 1, LANG_NEUTRAL, buf, e["size"]):
            fail("UpdateResourceW(RT_ICON #%d) failed" % (i + 1))
    g = ctypes.create_string_buffer(group, len(group))
    if not k.UpdateResourceW(h, RT_GROUP_ICON, 1, LANG_NEUTRAL, g, len(group)):
        fail("UpdateResourceW(RT_GROUP_ICON) failed")
    if not k.EndUpdateResourceW(h, 0):
        fail("EndUpdateResourceW failed")
    print("embed-icon: OK -> %s (%d icon images)" % (exe_path, len(entries)))

if __name__ == "__main__":
    if len(sys.argv) < 2:
        fail("usage: embed-icon.py <target.exe> [icon.ico]")
    exe = sys.argv[1]
    ico = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "nuomi.ico")
    if not os.path.exists(exe):
        fail("exe not found: " + exe)
    if not os.path.exists(ico):
        fail("ico not found: " + ico)
    embed(exe, ico)
