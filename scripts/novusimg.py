#!/usr/bin/env python3
"""Readers for the three image formats Novus OS ships.

Shared by inspect_img.py, check_rootfs.py and the font/ISO debug helpers.
All of them used to assume "inode 2 is font.bin"; the standard root tree
moved the font to /Novus/fonts/font.bin and added ~80 inodes, so path
resolution is now the only correct way to find anything.
"""
import struct

NOVS_MAGIC = 0x4E4F5653
BLOCK = 4096


def u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def u64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


class NovusFS:
    def __init__(self, data):
        self.data = data
        if u32(data, 0) != NOVS_MAGIC:
            raise ValueError('not a NovusFS image')
        self.bs = u32(data, 8)
        self.itbl = u64(data, 24)
        self.root = 1

    def block(self, n):
        return self.data[n * self.bs:(n + 1) * self.bs]

    def inode(self, ino):
        off = self.itbl * self.bs + (ino - 1) * 128
        raw = self.data[off:off + 128]
        if len(raw) < 100:
            raise ValueError('inode out of range')
        return {
            'mode': u16(raw, 0),
            'ino': u32(raw, 4),
            'size': u32(raw, 8),
            'blocks': u32(raw, 16),
            'direct': list(struct.unpack_from('<7Q', raw, 28)),
            'iblk': u64(raw, 84),
            'diblk': u64(raw, 92),
        }

    def _ids(self, node):
        for b in node['direct']:
            if b:
                yield b
        if node['iblk']:
            ind = self.block(node['iblk'])
            for i in range(0, len(ind), 8):
                b = u64(ind, i)
                if b:
                    yield b
        if node['diblk']:
            l2 = self.block(node['diblk'])
            for i in range(0, len(l2), 8):
                l1 = u64(l2, i)
                if not l1:
                    continue
                ind = self.block(l1)
                for j in range(0, len(ind), 8):
                    b = u64(ind, j)
                    if b:
                        yield b

    def _bytes(self, ino):
        node = self.inode(ino)
        out = bytearray()
        for b in self._ids(node):
            out += self.block(b)
        return bytes(out[:node['size']]), node

    def dirents(self, ino):
        buf, node = self._bytes(ino)
        if node['mode'] != 2:
            return []
        out = []
        for off in range(0, len(buf), 64):
            if off + 8 > len(buf):
                break
            child, rec, nlen = struct.unpack_from('<IHH', buf, off)
            if child == 0:
                continue
            if rec < 8 or off + 8 + nlen > len(buf):
                break
            out.append((child, buf[off + 8:off + 8 + nlen].decode('utf-8', 'replace')))
        return out

    def lookup(self, path):
        cur = self.root
        for comp in [c for c in path.split('/') if c]:
            found = 0
            for ino, name in self.dirents(cur):
                if name == comp:
                    found = ino
                    break
            if not found:
                return 0
            cur = found
        return cur

    def is_dir(self, path):
        ino = self.lookup(path)
        return ino != 0 and self.inode(ino)['mode'] == 2

    def read_file(self, path):
        ino = self.lookup(path)
        if not ino:
            return None
        data, node = self._bytes(ino)
        return data if node['mode'] == 1 else None


def short_name(name):
    if '.' in name:
        base, ext = name.split('.', 1)
    else:
        base, ext = name, ''
    return (base.upper().ljust(8) + ext.upper().ljust(3)).encode('ascii')


class FatFS:
    """FAT12/16/32 reader (8.3 short names only)."""

    def __init__(self, data):
        self.data = data
        self.bps = u16(data, 11)
        self.spc = data[13]
        self.res = u16(data, 14)
        self.nf = data[16]
        self.root_ent = u16(data, 17)
        self.fat_sz = u16(data, 22) or u32(data, 36)
        self.rds = (self.root_ent * 32 + self.bps - 1) // self.bps
        self.fat_off = self.res * self.bps
        self.root_off = (self.res + self.nf * self.fat_sz) * self.bps
        self.data_off = self.root_off + self.rds * self.bps
        self.eoc = 0xFFF8

    def _raw_dir(self, cluster):
        if cluster == 0:
            return self.data[self.root_off:self.root_off + self.root_ent * 32]
        out = bytearray()
        seen = set()
        c = cluster
        while 2 <= c < self.eoc and c not in seen:
            seen.add(c)
            off = self.data_off + (c - 2) * self.spc * self.bps
            out += self.data[off:off + self.spc * self.bps]
            c = u16(self.data, self.fat_off + c * 2)
        return bytes(out)

    def entries(self, cluster=0):
        raw = self._raw_dir(cluster)
        out = []
        for off in range(0, len(raw), 32):
            e = raw[off:off + 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5 or e[0] == ord('.'):
                continue
            if e[11] & 0x08 or (e[11] & 0x0F) == 0x0F:
                continue
            first = u16(e, 26) | (u16(e, 20) << 16)
            out.append((e[:11], first, u32(e, 28), bool(e[11] & 0x10)))
        return out

    def lookup(self, path):
        cluster = 0
        result = None
        for comp in [c for c in path.split('/') if c]:
            want = short_name(comp)
            result = None
            for nm, first, size, is_dir in self.entries(cluster):
                if nm == want:
                    result = (first, size, is_dir)
                    break
            if result is None:
                return None
            cluster = result[0]
        return result

    def is_dir(self, path):
        if not path:
            return True
        found = self.lookup(path)
        return found is not None and found[2]

    def read_file(self, path):
        found = self.lookup(path)
        if found is None or found[2]:
            return None
        first, size, _ = found
        out = bytearray()
        c = first
        seen = set()
        while 2 <= c < self.eoc and c not in seen and len(out) < size:
            seen.add(c)
            off = self.data_off + (c - 2) * self.spc * self.bps
            out += self.data[off:off + self.spc * self.bps]
            c = u16(self.data, self.fat_off + c * 2)
        return bytes(out[:size])


class IsoFS:
    """ISO 9660 primary (8.3) or Joliet (UCS-2BE) directory reader."""

    def __init__(self, data):
        self.data = data
        self.trees = {}
        for lba in range(16, 48):
            vd = data[lba * 2048:(lba + 1) * 2048]
            if len(vd) < 190 or vd[1:6] != b'CD001':
                break
            if vd[0] == 255:
                break
            if vd[0] == 1 and 'primary' not in self.trees:
                self.trees['primary'] = (u32(vd, 158), u32(vd, 166))
            if vd[0] == 2 and vd[88:91] == b'%/E' and 'joliet' not in self.trees:
                self.trees['joliet'] = (u32(vd, 158), u32(vd, 166))
        if not self.trees:
            raise ValueError('no ISO 9660 volume descriptor')

    def entries(self, lba, size, name_set='joliet'):
        out = []
        for s in range(0, (size + 2047) // 2048):
            sec = self.data[(lba + s) * 2048:(lba + s + 1) * 2048]
            off = 0
            while off + 33 <= len(sec):
                rec = sec[off]
                if rec == 0:
                    break
                if rec < 33 or off + rec > len(sec):
                    break
                nlen = sec[off + 32]
                ident = sec[off + 33:off + 33 + nlen]
                if not (nlen == 1 and ident[0] in (0, 1)):
                    if name_set == 'joliet':
                        name = ident.decode('utf-16-be', 'replace')
                    else:
                        name = ident.decode('ascii', 'replace')
                    if name.endswith(';1'):
                        name = name[:-2]
                    out.append((name, u32(sec, off + 2), u32(sec, off + 10),
                                bool(sec[off + 25] & 0x02)))
                off += rec
        return out

    def lookup(self, path, name_set='joliet'):
        lba, size = self.trees.get(name_set, self.trees['primary'])
        result = None
        for comp in [c for c in path.split('/') if c]:
            result = None
            for name, extent, length, is_dir in self.entries(lba, size, name_set):
                if name == comp:
                    result = (extent, length, is_dir)
                    break
            if result is None:
                return None
            lba, size = result[0], result[1]
        return result

    def is_dir(self, path, name_set='joliet'):
        if not path:
            return True
        found = self.lookup(path, name_set)
        return found is not None and found[2]

    def read_file(self, path, name_set='joliet'):
        found = self.lookup(path, name_set)
        if found is None or found[2]:
            return None
        lba, size, _ = found
        return self.data[lba * 2048:lba * 2048 + size]
