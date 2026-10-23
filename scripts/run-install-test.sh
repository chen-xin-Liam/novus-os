#!/usr/bin/env bash
#
# Novus OS - end-to-end installer smoke test.
#
# Boots the live ISO, waits for the UEFI menu, chooses the graphical
# installer (key 3) and captures the serial log plus a screenshot of the
# wizard window. The target is a fresh sparse image attached to an AHCI
# controller, which is the same path the installer writes on real
# hardware.
#
# Usage:
#   scripts/run-install-test.sh [SIZE_MB]
#
# Outputs:
#   /tmp/novus-install-serial.log   kernel + installer log
#   /tmp/novus-install.ppm          framebuffer capture (PPM)

set -euo pipefail

cd "$(dirname "$0")/.."

SIZE_MB="${1:-128}"
TARGET="${TARGET:-/tmp/novus-install-target.img}"
SERIAL="${SERIAL:-/tmp/novus-install-serial.log}"
SHOT="${SHOT:-/tmp/novus-install.ppm}"
MON="${MON:-/tmp/novus-install-mon.sock}"
TIMEOUT="${TIMEOUT:-75}"
# install (default) walks the wizard; desktop stops after the first frame,
# which is how the dark theme and the shell are captured.
FLOW="${FLOW:-install}"
EXTRA="${EXTRA:-}"

if [ ! -f out/novus-live.iso ]; then
    echo "out/novus-live.iso not found. Run 'make iso' first." >&2
    exit 1
fi

if [ ! -f "$TARGET" ]; then
    echo "creating $TARGET (${SIZE_MB} MiB)"
    truncate -s "${SIZE_MB}M" "$TARGET"
fi

rm -f "$SERIAL" "$SHOT" "$MON"

# A pristine variable store, so the CD is first in the boot order even if
# an earlier run left a Novus entry behind.
cp -f /usr/share/OVMF/OVMF_VARS_4M.fd /tmp/novus-install-vars.fd

qemu-system-x86_64 -machine q35 -smp 2 -m 4G \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=/tmp/novus-install-vars.fd \
    -cdrom out/novus-live.iso \
    -drive file="$TARGET",if=none,id=target,format=raw \
    -device ahci,id=ahci1 \
    -device ide-hd,drive=target,bus=ahci1.0 \
    -boot order=d -display none -no-reboot -net none \
    -serial "file:$SERIAL" \
    -monitor "unix:$MON,server,nowait" $EXTRA &
QPID=$!

cleanup() {
    kill "$QPID" 2>/dev/null || true
    wait "$QPID" 2>/dev/null || true
}
trap cleanup EXIT

python3 - "$MON" "$SHOT" "$TIMEOUT" "$FLOW" <<'PY'
import os
import socket
import sys
import time

mon, shot, timeout, flow = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
deadline = time.time() + 10
while time.time() < deadline:
    if os.path.exists(mon):
        break
    time.sleep(0.2)
else:
    print("monitor socket did not appear", file=sys.stderr)
    sys.exit(1)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(mon)
s.settimeout(1.0)


def cmd(text):
    s.sendall((text + "\n").encode())
    time.sleep(0.3)
    try:
        while True:
            s.recv(65536)
    except socket.timeout:
        pass


if flow == "desktop":
    time.sleep(38)
    cmd("screendump " + shot)
    print("captured desktop: " + shot)
    cmd("quit")
    sys.exit(0)

# The live menu polls for 20 s; 12 s leaves the firmware time to reach it.
time.sleep(12)
cmd("sendkey 3")
print("sent: 3 (graphical installer)")

# Kernel + WM + setup window.
time.sleep(20)
cmd("screendump " + shot)

# Walk the wizard with the keyboard, which is also the accessibility path:
# Welcome -> Disk -> Plan -> Confirm, tick the box, start.
time.sleep(1)
cmd("sendkey ret")   # welcome -> disk
time.sleep(2)
cmd("sendkey ret")   # disk -> plan
time.sleep(2)
cmd("sendkey ret")   # plan -> confirm
time.sleep(1)
cmd("screendump " + shot.replace(".ppm", "-confirm.ppm"))
cmd("sendkey spc")   # tick the confirmation
time.sleep(1)
cmd("sendkey ret")   # start install

# Copy + verification. The images total ~20 MiB, so this is seconds.
time.sleep(35)
cmd("screendump " + shot.replace(".ppm", "-done.ppm"))
print("captured: " + shot)
cmd("quit")
PY

echo "serial log: $SERIAL"
grep -aE '\[MENU\]|\[INST\]|\[APPS\]|\[APP\]|\[BOOT\] install|installer' "$SERIAL" || true
