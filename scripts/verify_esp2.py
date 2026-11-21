#!/usr/bin/env python3
"""Read the ESP back out of a flashed disk and check what boot will load.

    verify_esp2.py <disk_number|image> [local_file ...]

The disk half of a flash is the GPT; the part that actually decides whether
the machine boots is the ESP: BOOTX64.EFI has to be there, under the name
the firmware looks for, and the live root and install template have to be
beside it. This reads the GPT off the medium (not off the image), cuts the
ESP out at its real offset, parses its FAT, and prints the tree with sizes.

Any local file named on the command line is looked up by name inside that
tree and compared byte for byte, so a stale copy left from an earlier build
shows up as a mismatch instead of as "the file is present".
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from novusimg import FatFS, u32, u64

ESP_GUID = b'\x28\x73\x2a\xc1\x1f\xf8\xd2\x11\xba\x4b\x00\xa0\xc9\x3e\xc9\x3b'
SECTOR = 512


class DiskSource:
    def __init__(self, spec):
        if spec.isdigit():
            self.path = r'\\.\PhysicalDrive%d' % int(spec)
            self.fd = os.open(self.path,
                              os.O_RDONLY | getattr(os, 'O_BINARY', 0))
            self.device = True
        else:
            self.path = spec
            self.fd = os.open(spec, os.O_RDONLY | getattr(os, 'O_BINARY', 0))
            self.device = False

    def read_at(self, offset, length):
        os.lseek(self.fd, offset, os.SEEK_SET)
        out = bytearray()
        while len(out) < length:
            piece = os.read(self.fd, length - len(out))
            if not piece:
                break
            out += piece
        return bytes(out)

    def close(self):
        os.close(self.fd)


def gpt_partitions(src):
    header = src.read_at(SECTOR, SECTOR)
    if header[0:8] != b'EFI PART':
        raise SystemExit('%s: no GPT signature at LBA 1' % src.path)
    entries_lba = u64(header, 72)
    count = u32(header, 80)
    entry_size = u32(header, 84)
    first = src.read_at(entries_lba * SECTOR, count * entry_size)
    out = []
    for i in range(count):
        e = first[i * entry_size:(i + 1) * entry_size]
        if len(e) < 128 or e[0:16] == b'\x00' * 16:
            continue
        out.append({
            'index': i + 1,
            'type': e[0:16],
            'first': u64(e, 32),
            'last': u64(e, 40),
        })
    return out


def find_esp(parts):
    for p in parts:
        if p['type'] == ESP_GUID:
            return p
    return None


def walk(fat, cluster, prefix, rows):
    for nm, first, size, is_dir in fat.entries(cluster):
        name = nm[:8].decode('ascii', 'replace').rstrip()
        ext = nm[8:].decode('ascii', 'replace').rstrip()
        full = name + ('.' + ext if ext else '')
        rows.append((prefix + full + ('/' if is_dir else ''), size, is_dir,
                     first))
        if is_dir and first >= 2:
            walk(fat, first, prefix + full + '/', rows)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src = DiskSource(argv[1])
    try:
        parts = gpt_partitions(src)
        print('%s: %d GPT partitions' % (src.path, len(parts)))
        for p in parts:
            print('  #%d type %s  LBA %d..%d  %d MiB' %
                  (p['index'], p['type'].hex()[:8], p['first'], p['last'],
                   (p['last'] - p['first'] + 1) * SECTOR >> 20))
        esp = find_esp(parts)
        if esp is None:
            print('FAILED: no EFI system partition in the GPT')
            return 1
        size = (esp['last'] - esp['first'] + 1) * SECTOR
        print('ESP: LBA %d, %d bytes' % (esp['first'], size))
        blob = src.read_at(esp['first'] * SECTOR, size)
    finally:
        src.close()

    try:
        fat = FatFS(blob)
    except ValueError as exc:
        print('FAILED: ESP is not a readable FAT: %s' % exc)
        return 1
    print('ESP FAT%d: bps=%d spc=%d fat_sectors=%d' %
          (fat.fat_type, fat.bps, fat.spc, fat.fat_sz))

    rows = []
    walk(fat, 0, '/', rows)
    for name, sz, is_dir, _ in rows:
        if is_dir:
            print('  %s' % name)
        else:
            print('  %-34s %8d bytes' % (name, sz))

    boot_efi = [r for r in rows if r[0].upper().endswith('/BOOTX64.EFI')]
    if not boot_efi:
        print('FAILED: BOOTX64.EFI is not on the ESP')
        return 1
    payload = fat.read_file('EFI/BOOT/BOOTX64.EFI')
    print('BOOTX64.EFI: %d bytes on the medium' % len(payload))

    rc = 0
    for local in argv[2:]:
        # `local[:remote]` names the entry explicitly; a bare path is matched
        # by basename first and then by size, because the ESP renames some
        # build outputs (out/cppcc/hello.nuc is shipped as DEMO.NUC).
        if ':' in local and not local[1:3] == ':\\':
            local, remote = local.split(':', 1)
        else:
            remote = None
        if not os.path.isfile(local):
            print('%s: no such local file' % local)
            rc = 1
            continue
        mine = open(local, 'rb').read()
        if remote is not None:
            hits = [r for r in rows if not r[2] and
                    r[0].lstrip('/').upper() == remote.lstrip('/').upper()]
        else:
            want = os.path.basename(local).upper()
            hits = [r for r in rows if not r[2] and
                    os.path.basename(r[0]).upper() == want]
            if not hits:
                hits = [r for r in rows if not r[2] and r[1] == len(mine)]
        if not hits:
            print('%s: NOT on the ESP and no entry of %d bytes' %
                  (remote or local, len(mine)))
            rc = 1
            continue
        got = fat.read_file(hits[0][0].lstrip('/'))
        if got == mine:
            print('%s: %d bytes, identical to %s' %
                  (hits[0][0], len(got), local))
        else:
            first = next((i for i, (a, b) in enumerate(zip(got, mine))
                          if a != b), min(len(got), len(mine)))
            print('%s: MISMATCH at byte %d (on disk %d bytes, local %d bytes)' %
                  (hits[0][0], first, len(got), len(mine)))
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv))
