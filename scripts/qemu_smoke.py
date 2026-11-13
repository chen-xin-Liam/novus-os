#!/usr/bin/env python3
"""Boot Novus, exercise the desktop through RFB, and compare reviewed PNGs."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import zlib

ROOT = Path(__file__).absolute().parents[1]
FAILURES = re.compile(r"\[FATAL\]|\[TRAP\] kernel fault|selftest(?:\s*:\s*|\s+)FAIL\b|"
                      r"\[ND\] FAIL:|display list over|app launch failed|"
                      r"\[SCHED\] task (?!selftest\b|nxprobe\b)\w+ exited")


def check_log(text: str, required: tuple[str, ...] = ()) -> None:
    failure = FAILURES.search(text)
    if failure:
        raise RuntimeError(f"guest failure: {failure.group()}")
    missing = [line for line in required if line not in text]
    if missing:
        raise RuntimeError(f"missing boot markers: {missing}")


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    if len(rgb) != width * height * 3:
        raise ValueError("invalid RGB buffer")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack("!I", len(data)) + kind + data +
                struct.pack("!I", zlib.crc32(kind + data)))

    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"not a PNG baseline: {path}")
    offset, width, height = 8, 0, 0
    compressed = bytearray()
    while offset < len(data):
        size = struct.unpack_from("!I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + size]
        if len(payload) != size or offset + 12 + size > len(data):
            raise ValueError("truncated PNG")
        crc = struct.unpack_from("!I", data, offset + 8 + size)[0]
        if zlib.crc32(kind + payload) != crc:
            raise ValueError("PNG checksum mismatch")
        if kind == b"IHDR":
            width, height, depth, color, comp, filt, interlace = struct.unpack("!IIBBBBB", payload)
            if (depth, color, comp, filt, interlace) != (8, 2, 0, 0, 0):
                raise ValueError("baseline must be a smoke-generated RGB PNG")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
        offset += size + 12
    rows = zlib.decompress(compressed)
    stride = width * 3 + 1
    if not width or not height or len(rows) != stride * height:
        raise ValueError("invalid PNG dimensions")
    if any(rows[y * stride] != 0 for y in range(height)):
        raise ValueError("baseline must retain the smoke writer's PNG filters")
    return width, height, b"".join(rows[y * stride + 1:(y + 1) * stride]
                                   for y in range(height))


def compare(actual: bytes, expected: bytes, width: int, height: int,
            regions: list[tuple[int, int, int, int]],
            tolerance: int, max_changed: float) -> None:
    if len(actual) != width * height * 3 or len(expected) != len(actual):
        raise ValueError("baseline dimensions differ")
    changed = total = 0
    for x, y, w, h in regions:
        if min(x, y, w, h) < 0 or x + w > width or y + h > height:
            raise ValueError("comparison region outside screenshot")
        for row in range(y, y + h):
            for col in range(x, x + w):
                p = (row * width + col) * 3
                changed += any(abs(actual[p + c] - expected[p + c]) > tolerance
                               for c in range(3))
                total += 1
    if not total or changed / total > max_changed:
        raise RuntimeError(f"visual mismatch: {changed}/{total} pixels "
                           f"(limit {max_changed:.3%}, channel tolerance {tolerance})")


def receive(sock: socket.socket, length: int) -> bytes:
    result = bytearray()
    while len(result) < length:
        part = sock.recv(length - len(result))
        if not part:
            raise RuntimeError("QEMU closed its control connection")
        result.extend(part)
    return bytes(result)


class Rfb:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        version = receive(self.sock, 12)
        if version != b"RFB 003.008\n":
            raise RuntimeError(f"unsupported RFB version: {version!r}")
        self.sock.sendall(version)
        count = receive(self.sock, 1)[0]
        if 1 not in receive(self.sock, count):
            raise RuntimeError("RFB requires loopback-only, unauthenticated QEMU VNC")
        self.sock.sendall(b"\x01")
        if receive(self.sock, 4) != b"\0\0\0\0":
            raise RuntimeError("RFB security handshake failed")
        self.sock.sendall(b"\x01")
        header = receive(self.sock, 24)
        self.width, self.height = struct.unpack_from("!HH", header)
        receive(self.sock, struct.unpack_from("!I", header, 20)[0])
        self.rgb = bytearray(self.width * self.height * 3)
        self.absolute = False
        self.sock.sendall(struct.pack("!BBBBBBBBHHHBBBxxx",
                                     0, 0, 0, 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
        self.sock.sendall(struct.pack("!BBHiii", 2, 0, 3, 0, -223, -257))

    def frame(self) -> bytes:
        self.sock.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, self.width, self.height))
        while True:
            message = receive(self.sock, 1)[0]
            if message == 2:
                continue
            if message == 3:
                receive(self.sock, struct.unpack_from("!I", receive(self.sock, 7), 3)[0])
                continue
            if message != 0:
                raise RuntimeError(f"unexpected RFB message {message}")
            count = struct.unpack("!xH", receive(self.sock, 3))[0]
            pixels = False
            for _ in range(count):
                x, y, w, h, encoding = struct.unpack("!HHHHi", receive(self.sock, 12))
                if encoding == -257:
                    self.absolute = bool(x)
                elif encoding == -223:
                    self.width, self.height = w, h
                    self.rgb = bytearray(w * h * 3)
                elif encoding == 0:
                    raw = receive(self.sock, w * h * 4)
                    rgb = bytearray(w * h * 3)
                    rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
                    if x + w > self.width or y + h > self.height:
                        raise RuntimeError("RFB rectangle outside framebuffer")
                    for row in range(h):
                        dest = ((y + row) * self.width + x) * 3
                        self.rgb[dest:dest + w * 3] = rgb[row * w * 3:(row + 1) * w * 3]
                    pixels = True
                else:
                    raise RuntimeError(f"unsupported RFB encoding {encoding}")
            if pixels:
                return bytes(self.rgb)
            self.sock.sendall(struct.pack("!BBHHHH", 3, 0, 0, 0, self.width, self.height))

    def pointer(self, x: int, y: int, buttons: int = 0) -> None:
        self.sock.sendall(struct.pack("!BBHH", 5, buttons, x, y))

    def click(self, x: int, y: int) -> None:
        self.pointer(x, y)
        time.sleep(0.2)
        self.pointer(x, y, 1)
        time.sleep(0.2)
        self.pointer(x, y)
        time.sleep(0.8)

    def key(self, keysym: int) -> None:
        for down in (1, 0):
            self.sock.sendall(struct.pack("!BBHI", 4, down, 0, keysym))
            time.sleep(0.03)

    def text(self, text: str) -> None:
        for char in text:
            self.key(ord(char))


class Guest:
    def __init__(self, process: subprocess.Popen, serial: Path, timeout: float):
        self.process, self.serial = process, serial
        self.deadline = time.monotonic() + timeout

    def log(self) -> str:
        text = self.serial.read_text(encoding="utf-8", errors="replace") if self.serial.exists() else ""
        check_log(text)
        if self.process.poll() is not None:
            raise RuntimeError(f"QEMU exited with {self.process.returncode}; see stderr.log")
        return text

    def wait(self, marker: str) -> str:
        while time.monotonic() < self.deadline:
            text = self.log()
            if marker in text:
                return text
            time.sleep(0.1)
        raise TimeoutError(f"missing guest marker: {marker}")

    def frames(self, count: int) -> None:
        while time.monotonic() < self.deadline:
            text = self.log().split("[HALT] entering the scheduler", 1)[-1]
            if text.count("[NWM] first frame") >= count:
                return
            time.sleep(0.1)
        raise TimeoutError(f"only partial UI startup: expected {count} committed windows")


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def firmware(name: str, explicit: str | None, qemu: str) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise FileNotFoundError(f"firmware does not exist: {path}")
        return path.absolute()
    choices = [
        Path("/usr/share/OVMF") / ("OVMF_CODE_4M.fd" if name == "code" else "OVMF_VARS_4M.fd"),
        Path(qemu).absolute().parent / "share" /
        ("edk2-x86_64-code.fd" if name == "code" else "edk2-i386-vars.fd"),
    ]
    for choice in choices:
        if choice.is_file():
            return choice.absolute()
    raise FileNotFoundError(f"set --{name} / OVMF_{name.upper()} to an OVMF {name} image")


def exercise(guest: Guest, rfb: Rfb, args: argparse.Namespace) -> None:
    guest.wait("[HALT] entering the scheduler")
    rfb.frame()
    if (rfb.width, rfb.height) != (1024, 768):
        raise RuntimeError(f"expected 1024x768, got {rfb.width}x{rfb.height}")
    guest.wait("[ND] absolute pointer ready")
    rfb.frame()
    if not rfb.absolute:
        raise RuntimeError("RFB did not advertise an absolute pointer; refusing blind clicks")
    required = ("[ND] selftest PASS", "[HEAP] selftest PASS")
    required += (("[M3] no live root published", "[FONT] no system filesystem")
                 if args.no_root else ("[M3] live root mounted", "[WP] wallpaper ready"))
    check_log(guest.log(), required)
    baseline = args.baseline / ("no-root" if args.no_root else "disk")
    captured = []

    def capture(name: str, regions: list[tuple[int, int, int, int]]) -> bytes:
        rfb.pointer(4, 4)
        time.sleep(0.7)
        rfb.frame()
        rgb = rfb.frame()
        guest.log()
        output = args.output / f"{name}.png"
        write_png(output, rfb.width, rfb.height, rgb)
        reference = baseline / output.name
        if args.update_baseline:
            baseline.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(output, reference)
        else:
            if not reference.is_file():
                raise FileNotFoundError(f"missing reviewed baseline: {reference}; "
                                        "use --update-baseline only after reviewing captures")
            w, h, expected = read_png(reference)
            if (w, h) != (rfb.width, rfb.height):
                raise ValueError(f"baseline resolution changed: {reference}")
            compare(rgb, expected, w, h, regions, args.tolerance, args.max_changed)
        captured.append(name)
        print(f"PASS {name}", flush=True)
        return rgb

    session = "[SESSION] live session: no account required" not in guest.log()
    if session:
        if "[SESSION] account loaded; login required" in guest.log():
            raise RuntimeError("account setup regression requires a fresh disk image")
        guest.wait("[APP] session manager up")
        guest.frames(1)
        capture("session", [(303, 242, 420, 252)])
        rfb.click(450, 318)
        rfb.text("smoke")
        rfb.key(0xFF09)  # Tab
        rfb.text("passw0rd")
        rfb.click(470, 406)
        guest.wait("[SESSION] account created")
    guest.wait("[APP] settings up")
    frame_count = 4 if session else 3
    guest.frames(frame_count)
    # Minimize the three automatically launched apps, topmost first.
    for x, y in ((907, 167), (847, 206), (487, 412)):
        rfb.click(x, y)
    if session:
        rfb.click(609, 226)
    desktop = capture("desktop", [(0, 0, 1024, 690), (16, 690, 162, 78)])
    rfb.click(212, 730)
    start = capture("start", [(182, 134, 480, 560)])
    if sum(a != b for a, b in zip(desktop, start)) < 10000:
        raise RuntimeError("start-menu click did not reveal the menu")
    # A pinned start-menu entry must actually start an application.
    rfb.click(270, 386)
    guest.wait("[APP] netman up")
    frame_count += 1
    guest.frames(frame_count)
    capture("netman", [(381, 192, 520, 66), (391, 380, 500, 30),
                       (391, 498, 500, 30), (391, 586, 500, 30),
                       (405, 662, 374, 28)])
    rfb.click(787, 176)
    # Desktop tiles must restore existing instances as well as start new ones.
    rfb.click(960, 68)
    capture("fileman", [(41, 428, 560, 322)])
    rfb.click(487, 412)
    rfb.click(960, 164)
    capture("taskman", [(321, 222, 640, 105), (321, 592, 640, 28)])
    rfb.click(847, 206)
    rfb.click(960, 260)
    rfb.click(370, 390)  # Settings > About
    capture("settings", [(301, 183, 720, 390), (301, 733, 720, 34)])
    rfb.click(907, 167)
    rfb.click(960, 452)
    guest.wait("[APPS] launching nsh")
    guest.frames(frame_count + 1)
    rfb.pointer(4, 4)
    time.sleep(0.5)
    before = rfb.frame()
    rfb.text("smoke")
    after = capture("keyboard", [(147, 192, 876, 428)])
    changed = sum(a != b for a, b in zip(before, after))
    if changed < 30:
        raise RuntimeError("keyboard input did not change terminal content")
    guest.wait("[ND] keyboard byte from IRQ queue")
    check_log(guest.log(), required)
    (args.output / "result.json").write_text(json.dumps({
        "status": "PASS", "mode": "no-root" if args.no_root else "disk",
        "absolute_pointer": True, "screenshots": captured,
        "baseline_updated": args.update_baseline,
    }, indent=2) + "\n", encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", default=os.environ.get("QEMU_BIN", "qemu-system-x86_64"))
    parser.add_argument("--code", default=os.environ.get("OVMF_CODE"))
    parser.add_argument("--vars", default=os.environ.get("OVMF_VARS"))
    parser.add_argument("--image", type=Path, default=ROOT / "out" / "novus-disk.img")
    parser.add_argument("--no-root", action="store_true")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--vnc-display", type=int, default=1)
    parser.add_argument("--baseline", type=Path, default=ROOT / "scripts" / "baselines" / "qemu")
    parser.add_argument("--update-baseline", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "out" / "qemu-smoke")
    parser.add_argument("--tolerance", type=int, default=8)
    parser.add_argument("--max-changed", type=float, default=0.002)
    args = parser.parse_args()
    if not 0 <= args.tolerance <= 255 or not 0 <= args.max_changed < 1:
        parser.error("comparison thresholds outside their valid range")
    qemu = shutil.which(args.qemu)
    if qemu is None:
        raise FileNotFoundError(f"QEMU executable not found: {args.qemu}")
    code = firmware("code", args.code, qemu)
    variables = firmware("vars", args.vars, qemu)
    args.output = args.output.absolute()
    args.output.mkdir(parents=True, exist_ok=True)
    # Fail before starting a guest rather than attach to someone else's VNC.
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 5900 + args.vnc_display))
    with tempfile.TemporaryDirectory(prefix="novus-smoke-") as work:
        workdir = Path(work)
        varcopy = workdir / "vars.fd"
        shutil.copyfile(variables, varcopy)
        if args.no_root:
            boot = workdir / "esp" / "EFI" / "BOOT"
            boot.mkdir(parents=True)
            shutil.copyfile(ROOT / "out" / "NOVUS.EFI", boot / "BOOTX64.EFI")
            drive = f"format=raw,file=fat:{workdir / 'esp'}"
        else:
            if not args.image.is_file():
                raise FileNotFoundError(f"build the disk image first: {args.image}")
            drive = f"format=raw,file={args.image.absolute()}"
        port = free_port()
        command = [qemu, "-machine", "q35,vmport=on", "-accel", "tcg",
                   "-smp", "2", "-m", "3G", "-device", "VGA,xres=1024,yres=768",
                   "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                   "-drive", f"if=pflash,format=raw,file={varcopy}",
                   "-drive", drive, "-snapshot", "-no-reboot", "-display", "none",
                   "-vnc", f"127.0.0.1:{args.vnc_display}",
                   "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
                   "-serial", f"file:{args.output / 'serial.log'}",
                   "-rtc", "base=2026-01-01T12:00:00,clock=vm",
                   "-netdev", "user,id=net0", "-device", "e1000,netdev=net0"]
        share = Path(qemu).absolute().parent / "share"
        if share.is_dir():
            command += ["-L", str(share)]
        environment = dict(os.environ, TMPDIR=str(workdir), TEMP=str(workdir), TMP=str(workdir))
        serial = args.output / "serial.log"
        serial.write_text("", encoding="ascii")
        (args.output / "result.json").unlink(missing_ok=True)
        with (args.output / "stderr.log").open("w") as stderr:
            process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=stderr,
                                       env=environment)
            rfb = None
            try:
                guest = Guest(process, serial, args.timeout)
                guest.wait("[ND] demo desktop painted")
                rfb = Rfb(5900 + args.vnc_display)
                exercise(guest, rfb, args)
            finally:
                if rfb is not None:
                    rfb.sock.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    print(f"QEMU smoke PASS: {args.output}")


if __name__ == "__main__":
    main()
