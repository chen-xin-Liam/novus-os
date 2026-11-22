#!/usr/bin/env python3
"""Five-minute QEMU smoke test for the locally built live ISO.

Boots out/novus-live.iso headlessly with OVMF, watches the guest serial log
for the boot/desktop markers and fails immediately on a fatal marker, an early
QEMU exit or the 300 s wall-clock deadline. No network, no Pillow, no VNC and
no interaction with the developer's existing QEMU instances.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from ci_common import ROOT, dump_json, ensure_out

MAX_TIMEOUT = 300
REQUIRED = (
    "[BOOT] UEFI boot started",
    "[BOOT] mounts applied",
    "[ND] demo desktop painted",
)
FATAL = re.compile(
    r"\[FATAL\]|\[TRAP\] kernel fault|\[ND\] FAIL:|\[HALT\] panic|"
    r"selftest(?:\s*:\s*|\s+)FAIL\b|app launch failed"
)
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
OVSMF_CODE_CHOICES = (
    Path("/usr/share/OVMF/OVMF_CODE_4M.fd"),
    Path("/usr/share/OVMF/OVMF_CODE.fd"),
    Path("/home/user/local/usr/share/OVMF/OVMF_CODE.fd"),
)
OVMF_VARS_CHOICES = (
    Path("/usr/share/OVMF/OVMF_VARS_4M.fd"),
    Path("/usr/share/OVMF/OVMF_VARS.fd"),
    Path("/home/user/local/usr/share/OVMF/OVMF_VARS.fd"),
)


def clean(chunk: str) -> str:
    chunk = ANSI.sub("", chunk)
    out: list[str] = []
    for char in chunk:
        if char == "\b":
            if out:
                out.pop()
        elif char != "\r":
            out.append(char)
    return "".join(out)


def pick_firmware(explicit: str | None, choices, label: str, flag: str, env: str) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise RuntimeError(f"{label} not found: {path}")
        return path.resolve()
    for path in choices:
        if path.is_file():
            return path
    raise RuntimeError(f"no {label} found; pass --{flag} or set {env}")


def tail_lines(path: Path, count: int = 20) -> list[str]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []
    return [clean(line) for line in lines[-count:]]


def stop_qemu(process: subprocess.Popen) -> int:
    if process.poll() is not None:
        return process.returncode
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        process.terminate()
    try:
        return process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            process.kill()
        return process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default=str(ROOT / "out" / "novus-live.iso"))
    parser.add_argument("--timeout", type=int, default=MAX_TIMEOUT)
    parser.add_argument("--settle", type=int, default=15,
                        help="seconds to keep observing the guest after boot markers")
    parser.add_argument("--qemu", default=os.environ.get("QEMU_BIN", "qemu-system-x86_64"))
    parser.add_argument("--ovmf-code", default=os.environ.get("OVMF_CODE"))
    parser.add_argument("--ovmf-vars", default=os.environ.get("OVMF_VARS"))
    parser.add_argument("--output", default=str(ROOT / "out" / "ci" / "smoke"))
    args = parser.parse_args()

    if args.timeout < 10 or args.timeout > MAX_TIMEOUT:
        parser.error(f"--timeout must be between 10 and {MAX_TIMEOUT} seconds")
    if args.settle < 1 or args.settle > 60:
        parser.error("--settle must be between 1 and 60 seconds")
    image = Path(args.image)
    if not image.is_file():
        print(f"ci-smoke: missing image {image}; run make iso first", file=sys.stderr)
        return 2
    qemu = shutil.which(args.qemu)
    if qemu is None:
        print(f"ci-smoke: QEMU not found: {args.qemu}", file=sys.stderr)
        return 2
    code = pick_firmware(args.ovmf_code, OVSMF_CODE_CHOICES, "OVMF code image",
                         "ovmf-code", "OVMF_CODE")
    variables = pick_firmware(args.ovmf_vars, OVMF_VARS_CHOICES, "OVMF variable store",
                              "ovmf-vars", "OVMF_VARS")

    output = Path(args.output)
    if not output.is_absolute():
        output = ROOT / output
    output.mkdir(parents=True, exist_ok=True)
    serial = output / "serial.log"
    qemu_log = output / "qemu.log"
    result_path = output / "result.json"
    serial.write_text("", encoding="utf-8")

    marker_times: dict[str, float] = {}
    fatal_hits: list[str] = []
    status = "FAIL"
    reason = ""
    qemu_rc: int | None = None
    started = time.monotonic()

    with tempfile.TemporaryDirectory(prefix="novus-ci-smoke-") as work:
        varcopy = Path(work) / "vars.fd"
        shutil.copyfile(variables, varcopy)
        command = [
            qemu, "-machine", "q35", "-smp", "2", "-m", "3G", "-accel", "tcg",
            "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,file={varcopy}",
            "-cdrom", str(image.resolve()),
            "-serial", f"file:{serial}",
            "-display", "none", "-no-reboot", "-nic", "none",
        ]
        share = Path(qemu).resolve().parent / "share"
        if share.is_dir():
            command += ["-L", str(share)]

        process = None
        try:
            with qemu_log.open("wb") as log:
                process = subprocess.Popen(
                    command, stdout=log, stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
            deadline = started + args.timeout
            offset = 0
            window = ""
            ready_since: float | None = None

            while time.monotonic() < deadline:
                with serial.open("rb") as stream:
                    stream.seek(offset)
                    chunk = clean(stream.read().decode("utf-8", errors="replace"))
                    offset = stream.tell()
                window = (window + chunk)[-8192:]

                for marker in REQUIRED:
                    if marker not in marker_times and marker in window:
                        marker_times[marker] = round(time.monotonic() - started, 2)
                        print(f"  marker: {marker} (+{marker_times[marker]}s)", flush=True)
                match = FATAL.search(window)
                if match:
                    fatal_hits.append(match.group())
                    reason = f"fatal guest marker: {match.group()}"
                    break
                if process.poll() is not None:
                    reason = f"QEMU exited early with code {process.returncode}"
                    break
                if len(marker_times) == len(REQUIRED):
                    if ready_since is None:
                        ready_since = time.monotonic()
                    elif time.monotonic() - ready_since >= args.settle:
                        status = "PASS"
                        break
                time.sleep(0.25)
            else:
                missing = [marker for marker in REQUIRED if marker not in marker_times]
                reason = "timeout after {}s; missing: {}".format(
                    args.timeout, ", ".join(missing) if missing else "settle"
                )
        finally:
            if process is not None:
                qemu_rc = stop_qemu(process)

    duration = round(time.monotonic() - started, 2)
    result = {
        "status": status,
        "reason": reason,
        "duration_s": duration,
        "timeout_s": args.timeout,
        "settle_s": args.settle,
        "image": str(image),
        "qemu": qemu,
        "qemu_rc": qemu_rc,
        "markers_s": marker_times,
        "fatal_hits": fatal_hits,
        "serial_log": str(serial),
        "qemu_log": str(qemu_log),
    }
    dump_json(result_path, result)
    (output / "summary.txt").write_text(
        f"{status}: {reason or 'all boot markers seen; guest alive; no fatal markers'}\n"
        f"duration {duration}s / limit {args.timeout}s\n",
        encoding="utf-8",
    )

    if status == "PASS":
        print(f"smoke PASS: desktop painted in {marker_times[REQUIRED[-1]]}s; "
              f"no fatal markers; guest alive")
        return 0
    print(f"smoke FAIL: {reason}")
    for line in tail_lines(serial):
        print(f"  | {line}")
    print(f"artifacts: {output}")
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"ci-smoke: {exc}", file=sys.stderr)
        raise SystemExit(2)
