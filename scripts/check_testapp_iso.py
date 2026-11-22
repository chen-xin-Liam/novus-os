#!/usr/bin/env python3
"""Verify that a Novus live ISO publishes the /testapp directory.

The ISO is built by tools/mkiso, which writes a standard ISO 9660 primary
volume descriptor and a directory tree. This check reads that tree directly
(M1 FS: 2048-byte sectors, both-endian 32-bit sizes) instead of depending on
host tools such as isoinfo, so it runs in the same minimal environment as the
build.
"""

import os
import struct
import sys

SECTOR = 2048
PVD_LBA = 16


def die(message):
    print("check-testapp-iso: FAIL: " + message)
    return 1


def records(blob, lba, length):
    off = lba * SECTOR
    end = min(len(blob), off + length)
    while off + 34 <= end:
        size = blob[off]
        if size == 0:
            # ISO 9660 pads a directory with a zero-length "current
            # directory" record; move to the next sector boundary.
            off = (off // SECTOR + 1) * SECTOR
            continue
        if off + size > end:
            break
        extent = struct.unpack_from("<I", blob, off + 2)[0]
        # mkiso fills the both-endian size field correctly only in the
        # big-endian half; the little-endian half is left as raw bytes.
        data_len = struct.unpack_from(">I", blob, off + 14)[0]
        flags = blob[off + 25]
        name_len = blob[off + 32]
        name = blob[off + 33:off + 33 + name_len].decode("ascii", "replace")
        yield name, extent, data_len, bool(flags & 0x02)
        off += size


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "out/novus-testapp.iso"
    source = sys.argv[2] if len(sys.argv) > 2 else "testapp"
    if not os.path.isfile(iso):
        return die("missing " + iso)

    with open(iso, "rb") as handle:
        blob = handle.read()

    pvd = PVD_LBA * SECTOR
    if blob[pvd + 1:pvd + 6] != b"CD001" or blob[pvd] != 1:
        return die("no ISO 9660 primary volume descriptor")

    root_extent = struct.unpack_from("<I", blob, pvd + 156 + 2)[0]
    root_len = struct.unpack_from(">I", blob, pvd + 156 + 14)[0]
    found = None
    for name, extent, length, is_dir in records(blob, root_extent, root_len):
        if name.upper().split(";")[0] == "TESTAPP" and is_dir:
            found = (extent, length)
            break
    if found is None:
        return die("no /TESTAPP directory in the ISO")

    iso_files = {}
    for name, extent, length, is_dir in records(blob, found[0], found[1]):
        if not is_dir:
            iso_files[name.upper().split(";")[0]] = length

    expected = {}
    for name in sorted(os.listdir(source)):
        path = os.path.join(source, name)
        if os.path.isfile(path):
            expected[name.upper()] = os.path.getsize(path)

    if not expected:
        return die("no source files in " + source)

    for name, size in sorted(expected.items()):
        if name not in iso_files:
            return die("missing /TESTAPP/" + name)
        if iso_files[name] != size:
            return die("size mismatch for " + name + ": ISO has " +
                       str(iso_files[name]) + ", source has " + str(size))

    print("check-testapp-iso: PASS ({} files in /TESTAPP)".format(len(expected)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
