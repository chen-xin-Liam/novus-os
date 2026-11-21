#!/usr/bin/env python3
"""Write a Novus disk image onto a raw device, then prove it landed.

    flash_usb.py <disk_number> <image>

The image is written sector for sector from LBA 0, so it carries its own
GPT, ESP and NovusFS; nothing is partitioned or formatted here. Refusals
come first, because a raw write has no undo: disk 0 is refused outright, and
so is a device smaller than the image.

After the write every byte is read back and compared. A mismatch is reported
with the offset of the first bad byte - "the write returned success" is not
the same as "the bytes are on the medium".

Needs administrator rights (raw device access); run it from an elevated shell.
"""

import ctypes
import hashlib
import os
import sys
import time
from ctypes import wintypes

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
INVALID_HANDLE = ctypes.c_void_p(-1).value
CHUNK = 1 << 20
PREAMBLE_SECTORS = 34  # GPT header plus its entries

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                            ctypes.c_void_p]
k32.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_longlong,
                                 ctypes.POINTER(ctypes.c_longlong),
                                 wintypes.DWORD]
k32.SetFilePointerEx.restype = wintypes.BOOL
k32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k32.ReadFile.restype = wintypes.BOOL
k32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
k32.WriteFile.restype = wintypes.BOOL
k32.GetFileSizeEx.argtypes = [ctypes.c_void_p,
                              ctypes.POINTER(ctypes.c_longlong)]
k32.GetFileSizeEx.restype = wintypes.BOOL
k32.FlushFileBuffers.argtypes = [ctypes.c_void_p]
k32.FlushFileBuffers.restype = wintypes.BOOL
k32.CloseHandle.argtypes = [ctypes.c_void_p]


class Device:
    def __init__(self, number):
        self.path = r'\\.\PhysicalDrive%d' % number
        self.handle = k32.CreateFileW(
            self.path, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
        if not self.handle or self.handle == INVALID_HANDLE:
            raise SystemExit('cannot open %s: error %d (administrator rights?)'
                             % (self.path, ctypes.get_last_error()))
        size = ctypes.c_longlong(0)
        if k32.GetFileSizeEx(self.handle, ctypes.byref(size)) and size.value > 0:
            self.length = size.value
            self.length_error = 0
        else:
            self.length = int(os.environ.get('NOVUS_DISK_BYTES', '0') or 0)
            self.length_error = ctypes.get_last_error()

    def seek(self, offset):
        pos = ctypes.c_longlong(0)
        if not k32.SetFilePointerEx(self.handle, offset, ctypes.byref(pos), 0):
            raise OSError(ctypes.get_last_error(),
                          'seek to %d failed' % offset)

    def read_at(self, offset, length):
        self.seek(offset)
        buf = (ctypes.c_char * length)()
        done = wintypes.DWORD(0)
        if not k32.ReadFile(self.handle, buf, length, ctypes.byref(done), None):
            raise OSError(ctypes.get_last_error(),
                          'read at %d failed' % offset)
        return bytes(buf[:done.value])

    def write_at(self, offset, data):
        # The mount manager re-reads the partition table the moment the new
        # GPT lands, and a volume it decides to open can hold the disk for a
        # few hundred milliseconds - a raw write then fails with
        # ERROR_ACCESS_DENIED (5) in the middle of the image. Retry that
        # error instead of leaving half an image on the stick; every other
        # error is real and is raised at once.
        for attempt in range(30):
            self.seek(offset)
            view = (ctypes.c_char * len(data)).from_buffer_copy(data)
            done = wintypes.DWORD(0)
            ok = k32.WriteFile(self.handle, view, len(data),
                               ctypes.byref(done), None)
            if ok and done.value == len(data):
                return
            err = ctypes.get_last_error()
            if ok and done.value != len(data):
                raise OSError(0, 'short write at %d: %d of %d' %
                              (offset, done.value, len(data)))
            if err != 5 or attempt == 29:
                raise OSError(err, 'write at %d failed' % offset)
            time.sleep(0.5)

    def flush(self):
        k32.FlushFileBuffers(self.handle)

    def close(self):
        k32.CloseHandle(self.handle)


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return None if len(a) == len(b) else min(len(a), len(b))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    number = int(argv[1], 0)
    image = argv[2]
    if not os.path.isfile(image):
        print('no such image:', image)
        return 2
    data = open(image, 'rb').read()
    if len(data) < 1 << 20:
        print('image is suspiciously small: %d bytes' % len(data))
        return 2

    device = Device(number)
    try:
        if device.length:
            print('%s: %d MiB on the device, image is %d MiB' %
                  (device.path, device.length >> 20, len(data) >> 20))
        else:
            print('%s: capacity not reported (error %d), image is %d MiB' %
                  (device.path, device.length_error, len(data) >> 20))
        if number == 0:
            print('REFUSED: disk 0 is the system disk')
            return 1
        if device.length and device.length < len(data):
            print('REFUSED: the device is smaller than the image')
            return 1

        old = device.read_at(0, PREAMBLE_SECTORS * 512)
        if old == data[:len(old)]:
            print('device already starts with this image; rewriting anyway')

        written = 0
        while written < len(data):
            chunk = data[written:written + CHUNK]
            device.write_at(written, chunk)
            written += len(chunk)
        device.flush()
        print('wrote %d bytes and flushed the device' % written)

        bad = None
        done = 0
        while done < len(data):
            chunk = device.read_at(done, min(CHUNK, len(data) - done))
            diff = first_diff(chunk, data[done:done + len(chunk)])
            if diff is not None:
                bad = done + diff
                break
            done += len(chunk)
        if bad is not None:
            print('VERIFY FAILED: first differing byte at %d' % bad)
            return 1
        print('verified: %d bytes read back identical, md5 %s' %
              (len(data), hashlib.md5(data).hexdigest()))
        return 0
    finally:
        device.close()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
