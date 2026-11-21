#!/usr/bin/env python3
# Novus OS - build the two images the loader has to refuse.
#
# The mingw toolchain on this machine has no 32-bit target (gcc -m32 fails
# without a message, mingw32/ is empty), so a real PE32 cannot be compiled
# here. Rather than skip the test, this writes the two smallest images that
# are exactly what the compatibility list contains and what the loader must
# answer honestly about:
#
#   out/win32/hello32.exe  - PE32, machine i386, subsystem 6.01 (Windows 7),
#                            one .text section holding `jmp $`
#   out/win32/dosdemo.com  - 16-bit DOS .COM: plain code, no MZ header
#
# Both are header-accurate, not "fake" in the sense of lying about anything:
# the PE32 one really is a 32-bit x86 PE image, whose entry point really is
# that infinite loop. It only does nothing because nothing can run it yet.

import struct
import sys

PE32_TEXT_RVA = 0x1000
SECTION_ALIGN = 0x1000
FILE_ALIGN = 0x200
IMAGE_BASE = 0x400000
SUBSYS_WIN7 = (6, 1)


def align(n, a):
    return (n + a - 1) & ~(a - 1)


def pe32(code: bytes, subsystem=(6, 1)) -> bytes:
    """A PE32 image: DOS header, NT headers, one .text section."""
    hdr = bytearray(0x200)          # DOS header + stub + PE headers, one page
    # DOS header: only e_magic and e_lfanew matter to a loader.
    hdr[0:2] = b"MZ"
    struct.pack_into("<H", hdr, 0x3C, 0x80)
    # The "stub" at 0x40 is the classic 14-byte message + int 21h, printed by
    # DOS if the image is run there. Same text Windows puts in its own EXEs.
    stub = (b"\x0e\x1f\xba\x0e\x00\xb4\x09\xcd\x21\xb8\x01\x4c\xcd\x21"
            b"This program cannot be run in DOS mode.\r\r\n$")
    hdr[0x40:0x40 + len(stub)] = stub
    off = 0x80
    hdr[off:off + 4] = b"PE\0\0"
    coff = off + 4
    # machine, sections, timestamp, ptr/num symbols, optional header size,
    # characteristics (executable image | 32-bit)
    struct.pack_into("<HHIIIHH", hdr, coff, 0x14C, 1, 0, 0, 0, 0xE0, 0x0102)
    opt = coff + 20
    p = opt
    struct.pack_into("<H", hdr, p, 0x10B)                    # PE32 magic
    hdr[p + 2] = 11                                          # linker version
    size_of_code = align(len(code), FILE_ALIGN)
    struct.pack_into("<I", hdr, p + 4, size_of_code)         # SizeOfCode
    struct.pack_into("<I", hdr, p + 16, PE32_TEXT_RVA)       # AddressOfEntryPoint
    struct.pack_into("<I", hdr, p + 20, PE32_TEXT_RVA)       # BaseOfCode
    struct.pack_into("<I", hdr, p + 24, PE32_TEXT_RVA)       # BaseOfData
    struct.pack_into("<I", hdr, p + 28, IMAGE_BASE)          # ImageBase
    struct.pack_into("<I", hdr, p + 32, SECTION_ALIGN)       # SectionAlignment
    struct.pack_into("<I", hdr, p + 36, FILE_ALIGN)          # FileAlignment
    struct.pack_into("<HH", hdr, p + 40, *subsystem)         # OS version
    struct.pack_into("<HH", hdr, p + 48, *subsystem)         # subsystem version
    struct.pack_into("<I", hdr, p + 56, align(PE32_TEXT_RVA + size_of_code,
                                              SECTION_ALIGN))  # SizeOfImage
    struct.pack_into("<I", hdr, p + 60, 0x200)               # SizeOfHeaders
    struct.pack_into("<H", hdr, p + 68, 3)                   # subsystem: console
    struct.pack_into("<I", hdr, p + 92, 16)                  # NumberOfRvaAndSizes
    sec = opt + 0xE0
    hdr[sec:sec + 8] = b".text\0\0\0"
    struct.pack_into("<I", hdr, sec + 8, align(len(code), FILE_ALIGN))
    struct.pack_into("<I", hdr, sec + 12, PE32_TEXT_RVA)
    struct.pack_into("<I", hdr, sec + 16, align(len(code), FILE_ALIGN))
    struct.pack_into("<I", hdr, sec + 20, 0x200)             # PointerToRawData
    struct.pack_into("<I", hdr, sec + 36, 0x60000020)        # code|execute|read
    body = code + b"\0" * (size_of_code - len(code))
    return bytes(hdr) + body


def com16(code: bytes) -> bytes:
    """A .COM file is raw 16-bit code at 0x100 with no container at all."""
    return code


def main(argv: list[str]) -> int:
    out = argv[1] if len(argv) > 1 else "out/win32"
    # `jmp $` is EB FE in 32-bit and also in 16-bit; each image gets its own
    # copy so the two files are not byte-identical in a diff.
    with open(f"{out}/hello32.exe", "wb") as f:
        f.write(pe32(b"\xeb\xfe", SUBSYS_WIN7))
    with open(f"{out}/dosdemo.com", "wb") as f:
        f.write(com16(b"\xeb\xfe\x90"))
    print(f"{out}/hello32.exe and {out}/dosdemo.com written")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
