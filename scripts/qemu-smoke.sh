#!/usr/bin/env bash
#
# Novus OS - off-hardware QEMU smoke test (i440fx + OVMF + serial + HMP).
#
# Boots a published disk image, waits for the desktop, drives the guest through
# the QEMU monitor only, and checks the serial log plus PPM screendumps.
# Nothing is built and no source file is touched. No VNC, socat or vncdotool.
#
# Usage:
#   bash scripts/qemu-smoke.sh
#   bash scripts/qemu-smoke.sh --url https://.../novus-disk.img
#   bash scripts/qemu-smoke.sh --image out/novus-disk.img --scenario ps2
#   bash scripts/qemu-smoke.sh --scenario xhci --keep
#
# Exit codes: 0 every check passed, 1 at least one check failed, 2 setup error.
#
# Defaults (all overridable):
#   image URL   --url / NOVUS_IMAGE_URL / out/latest-image-url.txt / built-in
#   QEMU        --qemu / QEMU_BIN (/home/user/local/bin/qemu-novus if present)
#   OVMF        --ovmf-dir  (default /home/user/local/usr/share/OVMF)
#   QEMU data   --qemu-share (default /home/user/local/usr/share/qemu)
#   artifacts   --output    (default out/qemu-smoke)
#
set -uo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
OUT="$ROOT/out/qemu-smoke"
SETTLE=75
BOOT_TIMEOUT=90
SCENARIOS=all
IMAGE=""
URL="${NOVUS_IMAGE_URL:-}"
QEMU_BIN="${QEMU_BIN:-}"
OVMF_DIR="${OVMF_DIR:-/home/user/local/usr/share/OVMF}"
QEMU_SHARE="${QEMU_SHARE:-/home/user/local/usr/share/qemu}"
KEEP=0

# Latest build artifact published on the project CDN. Change this one line (or
# use --url / NOVUS_IMAGE_URL / out/latest-image-url.txt) when the upload key
# changes; the script never edits it at runtime.
DEFAULT_IMAGE_URL="https://cdn.tracy.ink/novus/novus-disk.img"

QPID=""
SOCK=""
LOG=""
HMPLOG=""
NAME=""
QEMU_START=0
WORK=""
HELPER=""

die() {
  printf 'qemu-smoke: %s\n' "$*" >&2
  exit 2
}

usage() {
  cat <<'USAGE'
Novus OS off-hardware QEMU smoke test.

  bash scripts/qemu-smoke.sh [options]

Options:
  --url URL            disk image URL (default: NOVUS_IMAGE_URL, then
                       out/latest-image-url.txt, then the published CDN link)
  --image PATH         use a local image instead of downloading one
  --scenario NAME      all (default), ps2 (scene 1+2), or xhci (scene 3)
  --output DIR         artifact directory (default out/qemu-smoke)
  --settle SECONDS     desktop settle time, must be 60..90 (default 75)
  --boot-timeout SEC   serial wait for the desktop (default 90)
  --qemu PATH          QEMU binary (default: qemu-novus if installed, else
                       qemu-system-x86_64)
  --ovmf-dir DIR       directory holding OVMF_CODE.fd and OVMF_VARS.fd
  --qemu-share DIR     QEMU -L data directory
  --keep               keep the /tmp work directory after the run
  -h, --help           show this help

The ps2 run boots with "-usb -device usb-kbd -device usb-mouse" and checks
scene 1 (serial markers, PS/2 ACKs) plus scene 2 (mouse_set 2 + mouse_move,
Tab/Return pixel diffs). The xhci run boots with "-device qemu-xhci -device
usb-kbd -device usb-mouse" and checks scene 3 ([USB] controller/enumeration).

Artifacts: serial-<scenario>.log, *-*.ppm, hmp-<scenario>.jsonl,
qemu-<scenario>.stderr.log, checks.jsonl, report.txt.
USAGE
}

while (($# > 0)); do
  case "$1" in
    --url) URL=${2:-}; shift 2 ;;
    --url=*) URL=${1#*=}; shift ;;
    --image) IMAGE=${2:-}; shift 2 ;;
    --image=*) IMAGE=${1#*=}; shift ;;
    --scenario) SCENARIOS=${2:-}; shift 2 ;;
    --scenario=*) SCENARIOS=${1#*=}; shift ;;
    --output) OUT=${2:-}; shift 2 ;;
    --output=*) OUT=${1#*=}; shift ;;
    --settle) SETTLE=${2:-}; shift 2 ;;
    --settle=*) SETTLE=${1#*=}; shift ;;
    --boot-timeout) BOOT_TIMEOUT=${2:-}; shift 2 ;;
    --boot-timeout=*) BOOT_TIMEOUT=${1#*=}; shift ;;
    --qemu) QEMU_BIN=${2:-}; shift 2 ;;
    --qemu=*) QEMU_BIN=${1#*=}; shift ;;
    --ovmf-dir) OVMF_DIR=${2:-}; shift 2 ;;
    --ovmf-dir=*) OVMF_DIR=${1#*=}; shift ;;
    --qemu-share) QEMU_SHARE=${2:-}; shift 2 ;;
    --qemu-share=*) QEMU_SHARE=${1#*=}; shift ;;
    --keep) KEEP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown argument: $1" ;;
  esac
done

[[ $SETTLE =~ ^[0-9]+$ ]] || die "--settle must be an integer number of seconds"
SETTLE=$((10#$SETTLE))
((SETTLE >= 60 && SETTLE <= 90)) || die "--settle must be in 60..90 (got $SETTLE)"
[[ $BOOT_TIMEOUT =~ ^[0-9]+$ ]] || die "--boot-timeout must be an integer"
BOOT_TIMEOUT=$((10#$BOOT_TIMEOUT))
((BOOT_TIMEOUT > 0)) || die "--boot-timeout must be positive"

case "$SCENARIOS" in
  all|ps2|xhci) ;;
  *) die "--scenario must be all, ps2 or xhci (got $SCENARIOS)" ;;
esac

[[ "$OUT" = /* ]] || OUT="$ROOT/$OUT"
if [[ -n "$IMAGE" && "$IMAGE" != /* ]]; then
  IMAGE="$ROOT/$IMAGE"
fi

PYTHON=${PYTHON:-python3}
command -v "$PYTHON" >/dev/null 2>&1 || die "python3 not found (set PYTHON)"
"$PYTHON" -c 'import PIL' >/dev/null 2>&1 ||
  die "Pillow is required: $PYTHON -m pip install pillow"

if [[ -z "$QEMU_BIN" ]]; then
  if [[ -x /home/user/local/bin/qemu-novus ]]; then
    QEMU_BIN=/home/user/local/bin/qemu-novus
  else
    QEMU_BIN=qemu-system-x86_64
  fi
fi
QEMU_PATH=$(command -v "$QEMU_BIN" 2>/dev/null || true)
[[ -n "$QEMU_PATH" && -x "$QEMU_PATH" ]] || die "QEMU is not executable: $QEMU_BIN"

# The project ships QEMU under /home/user/local with its own shared libraries
# (libfuse3, libpmem, ...). Only touch the loader path for that install.
if [[ "$QEMU_PATH" == /home/user/local/* ]]; then
  for d in /home/user/local/usr/lib/x86_64-linux-gnu /home/user/local/lib/x86_64-linux-gnu; do
    [[ -d "$d" ]] && LD_LIBRARY_PATH="$d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  done
  export LD_LIBRARY_PATH
fi

CODE="$OVMF_DIR/OVMF_CODE.fd"
VARS="$OVMF_DIR/OVMF_VARS.fd"
[[ -f "$CODE" ]] || die "missing OVMF code image: $CODE"
[[ -f "$VARS" ]] || die "missing OVMF variable store: $VARS"
[[ -d "$QEMU_SHARE" ]] || die "missing QEMU data directory: $QEMU_SHARE"

WORK=$(mktemp -d /tmp/novus-qemu-smoke.XXXXXX) || die "cannot create /tmp workdir"
HELPER="$WORK/smoke.py"

stop_qemu() {
  if [[ -n "$QPID" ]] && kill -0 "$QPID" 2>/dev/null; then
    if [[ -n "$SOCK" && -S "$SOCK" && -f "$HELPER" ]]; then
      "$PYTHON" "$HELPER" hmp --sock "$SOCK" --log "$WORK/quit.jsonl" \
        --timeout 3 --cmd quit >/dev/null 2>&1 || true
    fi
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
      kill -0 "$QPID" 2>/dev/null || break
      sleep 0.2
    done
    if kill -0 "$QPID" 2>/dev/null; then
      kill "$QPID" 2>/dev/null || true
      sleep 0.5
    fi
    if kill -0 "$QPID" 2>/dev/null; then
      kill -9 "$QPID" 2>/dev/null || true
    fi
    wait "$QPID" 2>/dev/null || true
  fi
  QPID=""
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM
  stop_qemu
  if [[ -n "$WORK" ]]; then
    if ((KEEP)); then
      printf 'qemu-smoke: workdir kept: %s\n' "$WORK"
    else
      rm -rf -- "$WORK"
    fi
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM

cat >"$HELPER" <<'PY'
#!/usr/bin/env python3
"""Helper for scripts/qemu-smoke.sh: HMP client, serial checks, report."""

import argparse
import json
import re
import socket
import sys
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
FATAL = re.compile(r"\[FATAL\]|\[TRAP\] kernel fault|\[ND\] FAIL:|"
                   r"\[HALT\] panic|display list over|app launch failed")
ERROR_WORDS = re.compile(r"unknown command|invalid|no such|could not|error",
                         re.I)


def clean(text):
    text = ANSI.sub("", text)
    out = []
    for ch in text:
        if ch == "\b":
            if out:
                out.pop()
        elif ch != "\r":
            out.append(ch)
    return "".join(out)


def read_lines(path):
    try:
        raw = Path(path).read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return []
    return [clean(line) for line in raw.splitlines()]


def record(scenario, name, ok, detail, raw=None):
    return {
        "scenario": scenario,
        "name": name,
        "status": "PASS" if ok else "FAIL",
        "detail": detail,
        "raw": [clean(x) for x in (raw or [])][:8],
    }


def tail(lines, tag, count=5):
    hits = [line for line in lines if tag in line]
    return hits[-count:]


# ---------------------------------------------------------------- HMP client

def connect_monitor(sock, timeout):
    deadline = time.time() + timeout
    while True:
        try:
            stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            stream.settimeout(timeout)
            stream.connect(sock)
            return stream
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            if time.time() > deadline:
                raise
            time.sleep(0.1)


def read_prompt(sock, timeout):
    sock.settimeout(timeout)
    buf = b""
    while True:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            return buf.decode("utf-8", "replace")
        if not chunk:
            return buf.decode("utf-8", "replace")
        buf += chunk
        if buf.endswith(b"(qemu) "):
            return buf.decode("utf-8", "replace")


def cmd_hmp(args):
    responses = []
    sock = None
    try:
        sock = connect_monitor(args.sock, args.timeout)
        read_prompt(sock, args.timeout)
        for command in args.cmd:
            if command.startswith("sleep:"):
                time.sleep(float(command.split(":", 1)[1]))
                continue
            sock.sendall((command + "\n").encode())
            reply = clean(read_prompt(sock, args.timeout))
            responses.append({
                "cmd": command,
                "response": reply,
                "ok": not ERROR_WORDS.search(reply),
            })
    except Exception as exc:  # noqa: BLE001 - recorded as a failed check
        responses.append({"cmd": "(connect)", "response": str(exc), "ok": False})
    finally:
        if sock is not None:
            sock.close()
    with open(args.log, "a", encoding="utf-8") as stream:
        for item in responses:
            stream.write(json.dumps(item, ensure_ascii=False) + "\n")
    return 0


# --------------------------------------------------------------- serial side

def check_fatal(lines, scenario):
    bad = [line for line in lines if FATAL.search(line)]
    return record(scenario, "guest.no-fatal", not bad,
                  "no [FATAL]/[TRAP]/[ND] FAIL marker" if not bad
                  else "%d fatal line(s)" % len(bad),
                  bad)


def check_desktop(lines, scenario):
    seen = any("[ND] demo desktop painted" in line for line in lines)
    return record(scenario, "boot.desktop-painted", seen,
                  "[ND] demo desktop painted seen" if seen
                  else "missing [ND] demo desktop painted",
                  tail(lines, "[ND]", 3))


def check_identity(lines, scenario):
    found = None
    for line in lines:
        match = re.search(
            r"\[MM\] identity map .*?RAM WB (\d+) bytes, MMIO UC (\d+) bytes",
            line)
        if match:
            found = (line, int(match.group(1)), int(match.group(2)))
    if not found:
        return record(scenario, "boot.identity-map", False,
                      "missing [MM] identity map RAM WB/MMIO UC counters",
                      tail(lines, "[MM]", 5))
    line, ram, mmio = found
    ok = ram > 0 and mmio > 0
    return record(scenario, "boot.identity-map", ok,
                  "RAM WB %d bytes, MMIO UC %d bytes" % (ram, mmio),
                  [] if ok else [line])


def check_paging(lines, scenario):
    seen = any("[MM] paging enabled" in line for line in lines)
    return record(scenario, "boot.paging", seen,
                  "[MM] paging enabled seen" if seen
                  else "missing [MM] paging enabled",
                  tail(lines, "[MM] paging", 3))


def check_timeline(lines, scenario):
    values = []
    for line in lines:
        match = re.search(r"\[T\] \+(\d+)ms", line)
        if match:
            values.append(int(match.group(1)))
    if len(values) < 3:
        return record(scenario, "boot.timeline", False,
                      "only %d [T] + stage line(s)" % len(values),
                      tail(lines, "[T] +", 5))
    increasing = all(b >= a for a, b in zip(values, values[1:])) and \
        values[-1] > values[0]
    detail = ("%d stages, %d..%d ms" % (len(values), values[0], values[-1])
              if increasing else
              "%d stages but not monotonic: %s" % (len(values), values[:8]))
    return record(scenario, "boot.timeline", increasing, detail,
                  [] if increasing else tail(lines, "[T] +", 5))


PS2_REQUIRED = (
    "[PS2] controller self-test: pass",
    "[PS2] aux port test: pass",
    "[PS2] mouse reset (0xFF): ACK",
    "[PS2] mouse self-test: pass",
    "[PS2] mouse device id: plain 3-byte",
    "[PS2] set sample rate (0xF3): ACK",
    "[PS2] sample rate 100 Hz: ACK",
    "[PS2] set resolution (0xE8): ACK",
    "[PS2] resolution 3/mm: ACK",
    "[PS2] scaling 1:1 (0xE7): ACK",
    "[PS2] enable reporting (0xF4): ACK",
    "[PS2] IRQ enable: config",
    "[PS2] keyboard IRQ1 + mouse IRQ12 enabled",
)
PS2_BAD = re.compile(
    r": (?:no reply|FAIL|unexpected|resend requested|WARN)|"
    r"\[PS2\] WARN|did not answer|no 8042 controller")


def check_ps2(lines, scenario):
    ps2 = [line[line.index("[PS2]"):] for line in lines if "[PS2]" in line]
    missing = [want for want in PS2_REQUIRED
               if not any(want in line for line in ps2)]
    bad = [line for line in ps2 if PS2_BAD.search(line)]
    acks = sum(1 for line in ps2 if ": ACK" in line)
    ok = not missing and not bad and acks >= 7
    detail = "%d ACK lines, %d/%d required" % (
        acks, len(PS2_REQUIRED) - len(missing), len(PS2_REQUIRED))
    if missing:
        detail += ", missing: " + "; ".join(missing[:2])
    if bad:
        detail += ", %d bad verdict(s)" % len(bad)
    raw = bad if bad else ps2[-6:]
    return record(scenario, "input.ps2-all-ack", ok, detail, raw)


# ------------------------------------------------------------------ [USB]

USB_ERRORS = (
    "[USB] no reachable BAR0",
    "[USB] no DMA memory",
    "[USB] enumeration failed",
    "[USB] ep0 rc",
    "[USB] bulk transfer failed",
    "[USB] HID report descriptor read failed",
    "[USB] HID interface ignored",
)


def check_usb(lines, scenario):
    usb = [line[line.index("[USB]"):] for line in lines if "[USB]" in line]
    # usb_log_bdf() prints "[USB] xhci <bus>:<dev>.<fn> ports=<hex>";
    # older logs used "[USB] xHCI ports=". Accept either plus the BAR0 move.
    controller = [line for line in usb if
                  re.search(r"\[USB\] (?:xhci .*|xHCI) ports=", line) or
                  line.startswith("[USB] BAR0 moved to")]
    ports = [line for line in usb if re.search(
        r"\[USB\] port 0x[0-9A-Fa-f]+ connected, speed=", line)]
    hid = [line for line in usb if "[USB] HID interface ready" in line]
    errors = [line for line in usb if any(e in line for e in USB_ERRORS)]

    # The kernel prints one controller line and one "HID interface ready"
    # line per enumerated device; it has no "[USB] ... attached" line, so
    # enumeration success is the port-connected + HID-ready pair.
    checks = [
        record(scenario, "usb.controller", bool(controller),
               "controller line seen" if controller else
               "missing [USB] xHCI ports=/BAR0 moved",
               controller[-3:] or tail(lines, "[USB]", 5)),
        record(scenario, "usb.enumeration", bool(ports and hid),
               "%d port(s) connected, %d HID interface(s) ready"
               % (len(ports), len(hid)),
               [] if (ports and hid) else (ports + hid)[-4:] or tail(lines, "[USB]", 5)),
        record(scenario, "usb.no-errors", not errors,
               "no USB failure line" if not errors
               else "%d USB failure line(s)" % len(errors),
               errors),
    ]
    return checks


# ---------------------------------------------------------------- screenshots

def clock_rect(width, height):
    """Dock clock block, derived from nd_demo.c (ND_DOCK_W/H/GAP)."""
    if width < 700 or height < 600:
        return (0, 0, 0, 0)
    dock_w, dock_h, gap = 660, 56, 10
    tx = (width - dock_w) // 2
    ty = height - dock_h - gap
    # Time at (tx + dock_w - 112, ty + 9, 96, 18); date at ty + 31. Pad 4 px.
    return (max(0, tx + dock_w - 112 - 4), max(0, ty + 9 - 4), 96 + 8, 48)


def changed_mask(image):
    bands = image.split()
    mask = bands[0].point(lambda v: 255 if v else 0)
    for band in bands[1:]:
        mask = ImageChops.lighter(mask, band.point(lambda v: 255 if v else 0))
    return mask


def screen_check(path, scenario, name="screen.desktop-capture"):
    if not path or not Path(path).is_file():
        return record(scenario, name, False,
                      "screenshot missing: %s" % (path or "(not captured)"), [])
    try:
        image = Image.open(path)
        image.load()
        rgb = image.convert("RGB")
    except Exception as exc:  # noqa: BLE001
        return record(scenario, name, False, "cannot read screenshot: %s" % exc, [])
    colors = rgb.getcolors(1 << 20)
    unique = len(colors) if colors is not None else 2000000
    blank = colors is not None and unique <= 8
    ok = not blank and rgb.width >= 640 and rgb.height >= 480
    detail = "%dx%d, %s colours" % (
        rgb.width, rgb.height, ">1M" if colors is None else unique)
    if blank:
        detail += " (looks blank)"
    return record(scenario, name, ok, detail, [])


def diff_check(path_a, path_b, scenario, name):
    if not path_a or not path_b:
        return record(scenario, name, False, "screenshots were not captured", [])
    for path in (path_a, path_b):
        if not Path(path).is_file():
            return record(scenario, name, False,
                          "screenshot missing: %s" % path, [])
    try:
        before = Image.open(path_a).convert("RGB")
        after = Image.open(path_b).convert("RGB")
    except Exception as exc:  # noqa: BLE001
        return record(scenario, name, False, "cannot read screenshots: %s" % exc, [])
    if before.size != after.size:
        return record(scenario, name, False,
                      "size changed %s -> %s" % (before.size, after.size), [])

    clock = clock_rect(*before.size)
    diff = ImageChops.difference(before, after)
    mask = changed_mask(diff)
    outside = mask.copy()
    if clock[2] and clock[3]:
        x, y, w, h = clock
        ImageDraw.Draw(outside).rectangle((x, y, x + w - 1, y + h - 1), fill=0)
    full_box = mask.getbbox()
    box = outside.getbbox()
    changed = outside.histogram()[255]
    ignored = mask.histogram()[255] - changed
    clock_text = "clock=%d,%d,%d,%d" % clock if clock[2] else "clock=none"

    if box is None:
        if full_box is None:
            detail = "no pixel change at all between the two screendumps (%s)" % clock_text
        else:
            detail = ("pixel changes are confined to the dock clock (%s); "
                      "full bbox=%s, %d clock pixel(s)" % (clock_text, full_box, ignored))
        return record(scenario, name, False, detail, [])
    if clock[2] and clock[3]:
        x, y, w, h = clock
        hit = box[0] < x + w and box[2] > x and box[1] < y + h and box[3] > y
    else:
        hit = False
    ok = not hit and changed > 0
    detail = ("bbox=%s, %d changed pixel(s) outside; %d ignored in dock clock "
              "(%s)" % (list(box), changed, ignored, clock_text))
    return record(scenario, name, ok, detail, [])


# ---------------------------------------------------------------- HMP checks

def read_jsonl(path):
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return []
    out = []
    for line in text.splitlines():
        if line.strip():
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return out


def hmp_check(records, scenario, name, command):
    hits = [item for item in records if item.get("cmd") == command]
    if not hits:
        return record(scenario, name, False,
                      "HMP command was not accepted/run: %s" % command, [])
    item = hits[-1]
    response = (item.get("response") or "").strip()
    ok = bool(item.get("ok")) and not ERROR_WORDS.search(response)
    detail = "accepted" if ok else "HMP rejected: %s" % response[-120:]
    return record(scenario, name, ok, detail, [] if ok else [response])


def hmp_checks(path, scenario):
    records = read_jsonl(path)
    return [
        hmp_check(records, scenario, "input.mouse-set-2", "mouse_set 2"),
        hmp_check(records, scenario, "input.mouse-move", "mouse_move -180 120"),
        hmp_check(records, scenario, "input.sendkey-tab", "sendkey tab"),
        hmp_check(records, scenario, "input.sendkey-ret", "sendkey ret"),
    ]


# ------------------------------------------------------------------- verify

def cmd_verify(args):
    lines = read_lines(args.log)
    checks = [
        check_fatal(lines, args.scenario),
        check_desktop(lines, args.scenario),
        check_identity(lines, args.scenario),
        check_paging(lines, args.scenario),
        check_timeline(lines, args.scenario),
    ]
    if args.scenario == "ps2":
        checks.append(check_ps2(lines, args.scenario))
        checks.append(screen_check(args.shot, args.scenario))
        checks.extend(hmp_checks(args.hmp, args.scenario))
        checks.append(diff_check(args.shot_a, args.shot_b, args.scenario,
                                 "pointer.mouse-move"))
        checks.append(diff_check(args.shot_b, args.shot_tab, args.scenario,
                                 "keyboard.tab"))
        checks.append(diff_check(args.shot_tab, args.shot_ret, args.scenario,
                                 "keyboard.ret"))
    else:
        checks.extend(check_usb(lines, args.scenario))
        checks.append(screen_check(args.shot, args.scenario))

    with open(args.out, "a", encoding="utf-8") as stream:
        for item in checks:
            stream.write(json.dumps(item, ensure_ascii=False) + "\n")
    return 0


def cmd_note(args):
    raw = read_lines(args.raw_file)[-6:] if args.raw_file else []
    item = record(args.scenario, args.name, args.status == "PASS",
                  args.detail, raw)
    with open(args.out, "a", encoding="utf-8") as stream:
        stream.write(json.dumps(item, ensure_ascii=False) + "\n")
    return 0


# ------------------------------------------------------------------- report

def cmd_report(args):
    records = read_jsonl(args.checks)
    if not records:
        print("no checks recorded - the run failed before any scenario")
        return 2

    order = []
    groups = {}
    for item in records:
        scenario = item.get("scenario", "?")
        if scenario not in groups:
            groups[scenario] = []
            order.append(scenario)
        groups[scenario].append(item)

    w1 = max(len("SCENARIO"), max(len(s) for s in order))
    w2 = max(len("CHECK"), max(len(i.get("name", "")) for i in records))
    lines = []
    lines.append("%-*s  %-*s  %-6s  %s" % (w1, "SCENARIO", w2, "CHECK",
                                           "RESULT", "DETAIL"))
    lines.append("%s  %s  %s  %s" % ("-" * w1, "-" * w2, "-" * 6, "-" * 6))
    for scenario in order:
        for item in groups[scenario]:
            lines.append("%-*s  %-*s  %-6s  %s" % (
                w1, scenario, w2, item.get("name", ""),
                item.get("status", "FAIL"), item.get("detail", "")))

    failures = [i for i in records if i.get("status") != "PASS"]
    passed = len(records) - len(failures)
    lines.append("")
    if failures:
        lines.append("FAILED CHECKS - raw log lines")
        for item in failures:
            lines.append("[%s] %s: %s" % (
                item.get("scenario", "?"), item.get("name", "?"),
                item.get("detail", "")))
            raw = item.get("raw") or []
            if raw:
                for line in raw:
                    lines.append("    raw: " + line)
            else:
                lines.append("    raw: (no serial line for this check)")
        lines.append("")
        lines.append("RESULT: FAIL - %d failed, %d passed (of %d)"
                     % (len(failures), passed, len(records)))
    else:
        lines.append("RESULT: PASS - %d checks" % len(records))
    lines.append("artifacts: %s" % args.artifacts)

    text = "\n".join(lines) + "\n"
    print(text, end="")
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    return 0 if not failures else 1


# --------------------------------------------------------------- image check

def cmd_image_check(args):
    path = Path(args.path)
    if not path.is_file():
        print("image missing: %s" % path)
        return 1
    size = path.stat().st_size
    head = path.read_bytes()[:512]
    mbr = len(head) == 512 and head[510:512] == b"\x55\xaa"
    html = head.lstrip().startswith(b"<")
    ok = size >= 16 * 1024 * 1024 and mbr and not html
    print("%d bytes, MBR=%s, HTML=%s" % (size, "yes" if mbr else "no",
                                         "yes" if html else "no"))
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="action", required=True)

    hmp = sub.add_parser("hmp")
    hmp.add_argument("--sock", required=True)
    hmp.add_argument("--log", required=True)
    hmp.add_argument("--timeout", type=float, default=5.0)
    hmp.add_argument("--cmd", action="append", default=[])
    hmp.set_defaults(func=cmd_hmp)

    verify = sub.add_parser("verify")
    verify.add_argument("--scenario", required=True, choices=["ps2", "xhci"])
    verify.add_argument("--log", required=True)
    verify.add_argument("--shot")
    verify.add_argument("--shot-a")
    verify.add_argument("--shot-b")
    verify.add_argument("--shot-tab")
    verify.add_argument("--shot-ret")
    verify.add_argument("--hmp")
    verify.add_argument("--out", required=True)
    verify.set_defaults(func=cmd_verify)

    note = sub.add_parser("note")
    note.add_argument("--out", required=True)
    note.add_argument("--scenario", required=True)
    note.add_argument("--name", required=True)
    note.add_argument("--detail", required=True)
    note.add_argument("--status", default="FAIL")
    note.add_argument("--raw-file")
    note.set_defaults(func=cmd_note)

    report = sub.add_parser("report")
    report.add_argument("--checks", required=True)
    report.add_argument("--out")
    report.add_argument("--artifacts", default="")
    report.set_defaults(func=cmd_report)

    image = sub.add_parser("image-check")
    image.add_argument("path")
    image.set_defaults(func=cmd_image_check)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
PY

mkdir -p -- "$OUT" || die "cannot create output directory: $OUT"
rm -f -- "$OUT"/serial-*.log "$OUT"/hmp-*.jsonl \
  "$OUT"/qemu-*.stdout.log "$OUT"/qemu-*.stderr.log \
  "$OUT"/ps2-*.ppm "$OUT"/xhci-*.ppm "$OUT"/report.txt
: >"$OUT/checks.jsonl"

say() {
  printf 'qemu-smoke: %s\n' "$*"
}

say "qemu:      $QEMU_PATH"
say "ovmf:      $CODE + $VARS (copied per run)"
say "qemu data: $QEMU_SHARE"
say "output:    $OUT"

if [[ -z "$IMAGE" ]]; then
  if [[ -z "$URL" && -s "$ROOT/out/latest-image-url.txt" ]]; then
    URL=$(head -n 1 "$ROOT/out/latest-image-url.txt" | tr -d '[:space:]')
  fi
  [[ -n "$URL" ]] || URL=$DEFAULT_IMAGE_URL
  IMG="$WORK/novus-disk.img"
  say "image url: $URL"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 900 \
      -o "$IMG.part" "$URL" || die "image download failed: $URL"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --tries=3 --timeout=30 -O "$IMG.part" "$URL" ||
      die "image download failed: $URL"
  else
    die "curl or wget is required to download the image"
  fi
  mv -- "$IMG.part" "$IMG"
else
  IMG=$IMAGE
  [[ -f "$IMG" ]] || die "image does not exist: $IMG"
  say "image file: $IMG"
fi

"$PYTHON" "$HELPER" image-check "$IMG" || die "image is not a raw Novus disk"
IMG_SIZE=$(stat -c%s "$IMG" 2>/dev/null || echo "?")
say "image:     $IMG ($IMG_SIZE bytes)"

start_qemu() {
  local scenario=$1
  shift
  NAME=$scenario
  LOG="$OUT/serial-$scenario.log"
  HMPLOG="$OUT/hmp-$scenario.jsonl"
  SOCK="$WORK/hmp-$scenario.sock"
  QEMU_START=$(date +%s)
  : >"$LOG"
  : >"$HMPLOG"
  rm -f -- "$SOCK"
  local vars="$WORK/vars-$scenario.fd"
  cp -- "$VARS" "$vars" || die "cannot copy OVMF variable store"

  # i440fx, 3072 MiB, split OVMF, one serial file, one HMP unix socket, no NIC.
  # vmport=off stops QEMU exposing the VMware absolute pointer, which the guest
  # otherwise prefers over PS/2; without it "mouse_set 2 + mouse_move" is
  # silently ignored and the pixel diff can only see clock ticks.
  # -snapshot keeps repeated runs on the same pristine image.
  "$QEMU_PATH" -machine pc,vmport=off -accel tcg -smp 2 -m 3072 \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$vars" \
    -drive "file=$IMG,format=raw,if=ide,index=0,media=disk" \
    -snapshot -boot order=c \
    -serial "file:$LOG" \
    -monitor "unix:$SOCK,server=on,wait=off" \
    -L "$QEMU_SHARE" -nic none -display none -no-reboot \
    "$@" >"$OUT/qemu-$scenario.stdout.log" 2>"$OUT/qemu-$scenario.stderr.log" &
  QPID=$!
}

wait_serial() {
  local marker=$1 limit=$2 i
  for ((i = 0; i < limit * 10; i++)); do
    if grep -qF -- "$marker" "$LOG" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
      return 2
    fi
    if grep -qE '\[FATAL\]|\[TRAP\] kernel fault|\[ND\] FAIL:|\[HALT\] panic' \
      "$LOG" 2>/dev/null; then
      return 3
    fi
    sleep 0.1
  done
  return 1
}

note_check() {
  local name=$1 detail=$2 raw=${3:-}
  local extra=()
  if [[ -n "$raw" && -f "$raw" ]]; then
    extra=(--raw-file "$raw")
  fi
  "$PYTHON" "$HELPER" note --out "$OUT/checks.jsonl" --scenario "$NAME" \
    --name "$name" --status FAIL --detail "$detail" "${extra[@]}" || true
}

wait_and_settle() {
  local rc=0
  wait_serial "[ND] demo desktop painted" "$BOOT_TIMEOUT" || rc=$?
  case $rc in
    2)
      note_check "qemu.process" \
        "QEMU exited before the desktop; see qemu-$NAME.stderr.log" \
        "$OUT/qemu-$NAME.stderr.log"
      return 1
      ;;
    3)
      note_check "guest.early-failure" \
        "a fatal serial marker appeared before the desktop" "$LOG"
      return 1
      ;;
    0) ;;
    *)
      note_check "boot.desktop-timeout" \
        "no [ND] demo desktop painted within ${BOOT_TIMEOUT}s" "$LOG"
      return 1
      ;;
  esac
  local elapsed=$(( $(date +%s) - QEMU_START ))
  if ((elapsed < SETTLE)); then
    sleep $((SETTLE - elapsed))
  fi
  return 0
}

hmp_batch() {
  "$PYTHON" "$HELPER" hmp --sock "$SOCK" --log "$HMPLOG" "$@" || true
}

run_ps2() {
  say "scene 1+2: i440fx, -usb + usb-kbd + usb-mouse"
  start_qemu ps2 -usb -device usb-kbd -device usb-mouse
  local ready=0
  if wait_and_settle; then
    ready=1
  fi
  if ((ready)); then
    # mouse_set 2 is the PS/2 mouse QEMU registers after the machine's 8042;
    # A/B bracket one relative move, C/D bracket Tab and Return.
    hmp_batch \
      --cmd "screendump $OUT/ps2-desktop.ppm" \
      --cmd "mouse_set 2" \
      --cmd "screendump $OUT/ps2-pointer-a.ppm" \
      --cmd "mouse_move -180 120" \
      --cmd "sleep:0.7" \
      --cmd "screendump $OUT/ps2-pointer-b.ppm" \
      --cmd "sendkey tab" \
      --cmd "sleep:0.9" \
      --cmd "screendump $OUT/ps2-tab.ppm" \
      --cmd "sendkey ret" \
      --cmd "sleep:0.9" \
      --cmd "screendump $OUT/ps2-ret.ppm"
  fi
  "$PYTHON" "$HELPER" verify --scenario ps2 --log "$LOG" \
    --shot "$OUT/ps2-desktop.ppm" \
    --shot-a "$OUT/ps2-pointer-a.ppm" \
    --shot-b "$OUT/ps2-pointer-b.ppm" \
    --shot-tab "$OUT/ps2-tab.ppm" \
    --shot-ret "$OUT/ps2-ret.ppm" \
    --hmp "$HMPLOG" --out "$OUT/checks.jsonl" || true
  stop_qemu
}

run_xhci() {
  say "scene 3: i440fx, -device qemu-xhci + usb-kbd + usb-mouse"
  start_qemu xhci -device qemu-xhci -device usb-kbd -device usb-mouse
  local ready=0
  if wait_and_settle; then
    ready=1
  fi
  if ((ready)); then
    hmp_batch --cmd "screendump $OUT/xhci-desktop.ppm"
  fi
  "$PYTHON" "$HELPER" verify --scenario xhci --log "$LOG" \
    --shot "$OUT/xhci-desktop.ppm" \
    --hmp "$HMPLOG" --out "$OUT/checks.jsonl" || true
  stop_qemu
}

case $SCENARIOS in
  all) run_ps2; run_xhci ;;
  ps2) run_ps2 ;;
  xhci) run_xhci ;;
esac

"$PYTHON" "$HELPER" report --checks "$OUT/checks.jsonl" \
  --out "$OUT/report.txt" --artifacts "$OUT"
RC=$?
exit "$RC"
