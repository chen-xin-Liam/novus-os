import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from qemu_smoke import Guest, Rfb, check_log, compare, firmware, main, read_png, receive, write_png


class Wire:
    def __init__(self, data):
        self.data = data
        self.sent = bytearray()

    def recv(self, length):
        result, self.data = self.data[:min(length, 7)], self.data[min(length, 7):]
        return result

    def sendall(self, data):
        self.sent.extend(data)


class SmokeTests(unittest.TestCase):
    def test_rfb_absolute_pointer_raw_frame_and_input(self):
        init = struct.pack("!HH", 2, 1) + bytes(16) + struct.pack("!I", 4) + b"test"
        update = (b"\0\0\0\2" +
                  struct.pack("!HHHHi", 1, 0, 2, 1, -257) +
                  struct.pack("!HHHHi", 0, 0, 2, 1, 0) +
                  b"\x03\x02\x01\0\x06\x05\x04\0")
        wire = Wire(b"RFB 003.008\n\x01\x01" + bytes(4) + init + update)
        with patch("qemu_smoke.socket.create_connection", return_value=wire):
            rfb = Rfb(5901)
        self.assertEqual(rfb.frame(), b"\x01\x02\x03\x04\x05\x06")
        self.assertTrue(rfb.absolute)
        rfb.pointer(123, 456, 1)
        self.assertEqual(wire.sent[-6:], struct.pack("!BBHH", 5, 1, 123, 456))
        with patch("qemu_smoke.time.sleep"):
            rfb.key(0xFF09)
        self.assertEqual(wire.sent[-16:], struct.pack("!BBHI", 4, 1, 0, 0xFF09) +
                         struct.pack("!BBHI", 4, 0, 0, 0xFF09))
        with self.assertRaises(RuntimeError):
            receive(wire, 1)

    def test_png_round_trip_and_corruption(self):
        rgb = bytes(range(18))
        with tempfile.TemporaryDirectory() as work:
            image = Path(work) / "frame.png"
            write_png(image, 3, 2, rgb)
            self.assertEqual(read_png(image), (3, 2, rgb))
            corrupt = bytearray(image.read_bytes())
            corrupt[20] ^= 1
            image.write_bytes(corrupt)
            with self.assertRaises(ValueError):
                read_png(image)

    def test_visual_thresholds_and_dynamic_region_mask(self):
        rgb = bytes(4 * 3)
        changed = bytes([9, 0, 0]) + bytes(9)
        compare(changed, rgb, 2, 2, [(0, 0, 2, 2)], 8, 0.25)
        with self.assertRaises(RuntimeError):
            compare(changed, rgb, 2, 2, [(0, 0, 2, 2)], 8, 0.249)
        compare(changed, rgb, 2, 2, [(1, 0, 1, 2)], 0, 0)
        with self.assertRaises(ValueError):
            compare(changed, rgb, 2, 2, [(0, 0, 3, 2)], 8, 0)
        with self.assertRaises(ValueError):
            compare(changed, bytes(3), 2, 2, [(0, 0, 2, 2)], 8, 0)

    def test_boot_assertions_reject_faults_and_missing_markers(self):
        check_log("[ND] selftest PASS", ("[ND] selftest PASS",))
        check_log("[SCHED] task nxprobe exited with code 0xFFFFFFFF")
        for log in ("[TRAP] kernel fault: halting", "[ND] selftest FAIL",
                    "[ND] FAIL: text width", "[APP] display list over text budget",
                    "[SCHED] task settings exited with code 3", "[FATAL] Boot refused"):
            with self.assertRaises(RuntimeError):
                check_log(log)
        with self.assertRaises(RuntimeError):
            check_log("[ND] selftest PASS", ("[M3] live root mounted",))

    def test_application_frames_exclude_wm_selftest(self):
        guest = Guest(Mock(), Path("unused"), 1)
        guest.deadline = 1
        log = ("[NWM] first frame win=1\n[HALT] entering the scheduler\n"
               "[NWM] first frame win=3\n")
        with patch.object(guest, "log", return_value=log), \
             patch("qemu_smoke.time.monotonic", side_effect=[0, 2]), \
             patch("qemu_smoke.time.sleep"):
            with self.assertRaises(TimeoutError):
                guest.frames(2)
        with patch.object(guest, "log", return_value=log), \
             patch("qemu_smoke.time.monotonic", return_value=0):
            guest.frames(1)

    def test_explicit_missing_firmware_does_not_fall_back(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaises(FileNotFoundError):
                firmware("code", str(Path(work) / "missing.fd"), "qemu")

    def test_repeated_run_discards_stale_boot_log_and_pass_result(self):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            (root / "out").mkdir()
            (root / "out" / "NOVUS.EFI").write_bytes(b"test")
            output = root / "captures"
            output.mkdir()
            serial = output / "serial.log"
            serial.write_text("[FATAL] previous run\n", encoding="ascii")
            result = output / "result.json"
            result.write_text('{"status":"PASS"}', encoding="ascii")
            image = root / "firmware.fd"
            image.write_bytes(bytes(4096))
            process = Mock()
            process.poll.return_value = 0

            def fresh_log(_process, path, _timeout):
                self.assertEqual(path.read_text(encoding="ascii"), "")
                self.assertFalse(result.exists())
                raise RuntimeError("fresh boot state verified")

            with patch.object(sys, "argv", ["qemu-smoke", "--no-root", "--output", str(output)]), \
                 patch("qemu_smoke.ROOT", root), \
                 patch("qemu_smoke.shutil.which", return_value=str(root / "qemu")), \
                 patch("qemu_smoke.firmware", return_value=image), \
                 patch("qemu_smoke.socket.socket"), \
                 patch("qemu_smoke.subprocess.Popen", return_value=process), \
                 patch("qemu_smoke.Guest", side_effect=fresh_log):
                with self.assertRaisesRegex(RuntimeError, "fresh boot state verified"):
                    main()


if __name__ == "__main__":
    unittest.main()
