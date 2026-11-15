"""Unit tests for the host-side FAT12/16/32 image reader.

Run with: python3 -m unittest discover -s scripts -p novusimg_test.py
"""

import struct
import unittest

from novusimg import FatFS


SECTOR = 512


def synthetic_fat(fat_type):
    """Construct standard-volume-sized images with root, subdir and 2-cluster file."""
    clusters = {12: 100, 16: 5000, 32: 65526}[fat_type]
    reserved = 32 if fat_type == 32 else 1
    root_entries = 0 if fat_type == 32 else 16
    root_sectors = (root_entries * 32 + SECTOR - 1) // SECTOR
    fat_bytes = (clusters + 2) * {12: 3 / 2, 16: 2, 32: 4}[fat_type]
    fat_sectors = (int(fat_bytes + 1) + SECTOR - 1) // SECTOR
    total_sectors = reserved + 2 * fat_sectors + root_sectors + clusters
    data = bytearray(total_sectors * SECTOR)
    struct.pack_into('<H', data, 11, SECTOR)
    data[13] = 1
    struct.pack_into('<H', data, 14, reserved)
    data[16] = 2
    struct.pack_into('<H', data, 17, root_entries)
    if total_sectors < 65536:
        struct.pack_into('<H', data, 19, total_sectors)
    else:
        struct.pack_into('<I', data, 32, total_sectors)
    if fat_type != 32:
        struct.pack_into('<H', data, 22, fat_sectors)
    else:
        struct.pack_into('<I', data, 36, fat_sectors)
        struct.pack_into('<I', data, 44, 2)

    fat_off = reserved * SECTOR
    root_off = (reserved + 2 * fat_sectors) * SECTOR
    data_off = root_off + root_sectors * SECTOR

    def set_fat(cluster, value):
        for table in range(2):
            start = fat_off + table * fat_sectors * SECTOR
            if fat_type == 12:
                off = start + cluster * 3 // 2
                current = struct.unpack_from('<H', data, off)[0]
                if cluster & 1:
                    current = (current & 0x000F) | ((value & 0xFFF) << 4)
                else:
                    current = (current & 0xF000) | (value & 0xFFF)
                struct.pack_into('<H', data, off, current)
            elif fat_type == 16:
                struct.pack_into('<H', data, start + cluster * 2, value)
            else:
                struct.pack_into('<I', data, start + cluster * 4, value)

    eoc = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[fat_type]
    set_fat(0, eoc)
    set_fat(1, eoc)
    for c in (2, 3, 5, 6):
        set_fat(c, eoc)
    set_fat(4, 5)

    def cluster_off(c):
        return data_off + (c - 2) * SECTOR

    def entry(pos, name, first, size=0, directory=False):
        data[pos:pos + 11] = name.encode('ascii')
        data[pos + 11] = 0x10 if directory else 0x20
        struct.pack_into('<H', data, pos + 26, first & 0xFFFF)
        if fat_type == 32:
            struct.pack_into('<H', data, pos + 20, first >> 16)
        struct.pack_into('<I', data, pos + 28, size)

    root = cluster_off(2) if fat_type == 32 else root_off
    entry(root, 'SUBDIR     ', 3, directory=True)
    entry(root + 32, 'HELLO   TXT', 4, 517)
    entry(cluster_off(3), 'CHILD   BIN', 6, 3)
    data[cluster_off(4):cluster_off(4) + SECTOR] = b'A' * SECTOR
    data[cluster_off(5):cluster_off(5) + 5] = b'world'
    data[cluster_off(6):cluster_off(6) + 3] = b'xyz'
    return data, set_fat


class FatFsTests(unittest.TestCase):
    def test_all_fat_variants_with_nested_files(self):
        for variant in (12, 16, 32):
            with self.subTest(fat=variant):
                image, _ = synthetic_fat(variant)
                fat = FatFS(image)
                self.assertEqual(fat.fat_type, variant)
                self.assertTrue(fat.is_dir('SUBDIR'))
                self.assertEqual(fat.read_file('HELLO.TXT'), b'A' * SECTOR + b'world')
                self.assertEqual(fat.read_file('SUBDIR/CHILD.BIN'), b'xyz')
                self.assertIsNone(fat.read_file('NOTFOUND'))
                self.assertIsNone(fat.lookup('HELLO.TXT/BAD'))

    def test_bad_geometry_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'truncated'):
            FatFS(b'')
        for variant in (12, 16, 32):
            with self.subTest(fat=variant):
                image, _ = synthetic_fat(variant)
                with self.assertRaisesRegex(ValueError, 'geometry'):
                    FatFS(image[:4096])

    def test_cycle_and_truncated_chain_are_rejected(self):
        for variant in (12, 16, 32):
            with self.subTest(fat=variant):
                image, set_fat = synthetic_fat(variant)
                set_fat(4, 4)
                with self.assertRaisesRegex(ValueError, 'cyclic'):
                    FatFS(image).read_file('HELLO.TXT')
                set_fat(4, {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}[variant])
                with self.assertRaisesRegex(ValueError, 'truncated'):
                    FatFS(image).read_file('HELLO.TXT')

    def test_empty_file(self):
        image, _ = synthetic_fat(16)
        root_off = (1 + 2 * (struct.unpack_from('<H', image, 22)[0])) * SECTOR
        image[root_off + 32 + 28:root_off + 32 + 32] = b'\0' * 4
        self.assertEqual(FatFS(image).read_file('HELLO.TXT'), b'')


if __name__ == '__main__':
    unittest.main()
