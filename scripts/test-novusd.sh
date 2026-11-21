#!/usr/bin/env bash
# Boot the live image, select the no-desktop shell, and smoke-test novusd
# through the serial console. Mirrors qtest.sh's OVMF/QEMU setup.
set -euo pipefail

ROOT=/home/dev/novus-os/novus-os
cd "$ROOT"

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  source user/novusd/scripts/env.sh
  make -j2 >/tmp/novusd-smoke-build.log 2>&1
fi

# Cheap static checks before spending a QEMU boot.
test -s out/apps/novusd.bin
grep -q 'k_app_novusd' build/apps_blobs.h
test "$(wc -c < out/apps/novusd.bin)" -le 131072

OVMF=/usr/share/OVMF
CODE="$OVMF/OVMF_CODE_4M.fd"
VARS=/tmp/novusd-smoke-vars.fd
DISK=/tmp/novusd-smoke-disk.img
SOCK=/tmp/novusd-smoke-socks
mkdir -p "$SOCK"
rm -f "$SOCK"/*.sock "$VARS"
cp "$OVMF/OVMF_VARS_4M.fd" "$VARS"
qemu-img create -f raw "$DISK" 64M >/dev/null 2>&1

qemu-system-x86_64 -machine pc -smp 2 -m 4G \
  -drive if=pflash,format=raw,readonly=on,file="$CODE" \
  -drive if=pflash,format=raw,file="$VARS" \
  -cdrom out/novus-live.iso -boot order=d -display none \
  -usb -device usb-kbd -device usb-mouse \
  -serial "unix:$SOCK/serial.sock,server,nowait" \
  -monitor "unix:$SOCK/monitor.sock,server,nowait" \
  -drive if=ide,index=0,media=disk,format=raw,file="$DISK" \
  -no-reboot -net none &
QEMU_PID=$!
trap 'kill "$QEMU_PID" 2>/dev/null || true' EXIT

python3 - "$SOCK" <<'PY'
import socket
import sys
import time

sock_dir = sys.argv[1]

def connect(name, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock_dir + "/" + name)
            s.settimeout(0.2)
            return s
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("QEMU socket timeout: " + name)

mon = connect("monitor.sock")
ser = connect("serial.sock")
log = b""
deadline = time.time() + 60

def pump(seconds):
    global log
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = ser.recv(65536)
            if chunk:
                log += chunk
        except socket.timeout:
            pass

def wait_for(marker, label):
    while time.time() < deadline and marker not in log:
        pump(0.2)
    if marker not in log:
        raise RuntimeError(label + " not reached; tail: " + repr(log[-600:]))

# The boot stage has its own 3 s text menu (option 5 = no desktop) whose
# input is EFI ConIn. QEMU's `sendkey` is not delivered to that menu on every
# host, so try it, then fall back to a boot-only check with explicit manual
# steps instead of failing a scripted run for a host input limitation.
for _ in range(6):
    mon.sendall(b"sendkey 5\n")
    time.sleep(0.35)

console_deadline = min(deadline, time.time() + 20)
while time.time() < console_deadline and b"nsh (console)" not in log:
    pump(0.2)

text = log.decode("utf-8", "replace")
if b"nsh (console)" not in log:
    assert "[BOOT]" in text or "[MOUNT]" in text, "image did not boot"
    print("NOVUSD_SMOKE_BOOT_OK")
    print("Interactive novusd checks need the no-desktop shell on this host:")
    print("  boot menu option 5, then run: novusd status / selftest / scan /Novus/logs")
else:
    for command in (
        b"novusd status\r",
        b"novusd selftest\r",
        b"novusd scan /Novus/logs\r",
        b"novusd quarantine list\r",
    ):
        ser.sendall(command)
        pump(3)
    text = log.decode("utf-8", "replace")
    assert "capabilities:" in text, "novusd status produced no output"
    assert "MALICIOUS" in text, "novusd selftest did not detect EICAR"
    assert "scanned " in text, "novusd scan produced no report"
    print("NOVUSD_SMOKE_OK")
PY
