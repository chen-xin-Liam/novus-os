"""Regression tests for the ISO primary boot entry's file type."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class IsoBootFileTypeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        scripts = root / 'scripts'
        scripts.mkdir()
        shutil.copyfile(HERE / 'check_rootfs.py', scripts / 'check_rootfs.py')
        (scripts / 'novusimg.py').write_text('''\
class NovusFS:
    def __init__(self, data):
        pass
    def is_dir(self, path):
        return True
    def lookup(self, path):
        return 1
    def read_file(self, path):
        return b'font' if path == 'Novus/fonts/font.bin' else b'fstype=novusfs'

class FatFS:
    def __init__(self, data):
        pass
    def is_dir(self, path):
        return True
    def read_file(self, path):
        return b'fstype=novusfs'

class IsoFS:
    def __init__(self, data):
        self.kind = data
        self.trees = {'joliet': object(), 'primary': object()}
    def is_dir(self, path, tree):
        return True
    def lookup(self, path, tree):
        if tree == 'primary' and path == 'EFI/BOOT/BOOTX64.EFI':
            if self.kind == b'missing':
                return None
            return (1, 10, self.kind == b'directory')
        return (1, 10, False)
    def read_file(self, path, tree):
        return b'fstype=novusfs'
''', encoding='utf-8')
        layout = root / 'fs' / 'novusfs'
        layout.mkdir(parents=True)
        (layout / 'rootfs_layout.h').write_text(
            '#define NOVUS_LAYOUT_DIRS(X)\\\n' +
            ''.join('X("Dir%02d", "D%02d", 0)\\\n' % (n, n) for n in range(77)) +
            '/* Number of directory entries */\n' +
            '#define NOVUS_LAYOUT_META(X)\n' +
            '#define NOVUS_LAYOUT_MOUNTS(X)\n' +
            '#endif\n', encoding='utf-8')
        graphics = root / 'graphics'
        graphics.mkdir()
        (graphics / 'nd_font_data.bin').write_bytes(b'font')
        self.out = root / 'out'
        self.out.mkdir()
        for name in ('novus.img', 'esp-tpl.img', 'esp.img'):
            (self.out / name).write_bytes(b'fixture')
        self.checker = scripts / 'check_rootfs.py'

    def run_checker(self, kind):
        (self.out / 'novus-live.iso').write_bytes(kind)
        return subprocess.run(
            [sys.executable, str(self.checker), str(self.out)],
            cwd=self.temp.name, capture_output=True, text=True, check=False)

    def test_boot_entry_is_regular_file(self):
        result = self.run_checker(b'file')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_boot_entry_is_directory(self):
        result = self.run_checker(b'directory')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('ISO primary: UEFI boot file missing or not a file', result.stdout)

    def test_boot_entry_is_missing(self):
        result = self.run_checker(b'missing')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('ISO primary: UEFI boot file missing or not a file', result.stdout)


if __name__ == '__main__':
    unittest.main()
