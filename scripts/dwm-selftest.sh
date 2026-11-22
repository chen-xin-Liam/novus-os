#!/usr/bin/env bash
# Mini-DWM self test. Compiles and runs the host-side composition operator
# checks; with QEMU=1 it also boots the image and greps the serial log for the
# kernel selftest / compositor startup markers.
set -euo pipefail
cd "$(dirname "$0")/.."
HOSTCC="${HOSTCC:-cc}"
mkdir -p build/tests

"$HOSTCC" -O2 -Wall -Wextra -Werror scripts/dwm-selftest.c \
    -o build/tests/dwm-selftest
build/tests/dwm-selftest

if [ "${QEMU:-0}" = "1" ]; then
    if [ ! -f out/NOVUS.EFI ]; then
        echo "[DWM-TEST] out/NOVUS.EFI missing; run make first" >&2
        exit 1
    fi
    command -v qemu-system-x86_64 >/dev/null || {
        echo "[DWM-TEST] qemu-system-x86_64 not found" >&2
        exit 1
    }
    mkdir -p esp/EFI/BOOT
    cp out/NOVUS.EFI esp/EFI/BOOT/BOOTX64.EFI
    if [ -f out/wallpapers.pkg ]; then cp out/wallpapers.pkg esp/WPAPERS.PKG; fi
    [ -f out/OVMF_VARS.fd ] || cp /usr/share/OVMF/OVMF_VARS_4M.fd out/OVMF_VARS.fd

    log=$(mktemp)
    trap 'rm -f -- "$log"' EXIT
    timeout "${TIMEOUT:-25}" qemu-system-x86_64 -machine q35 -smp 2 \
        -m "${MEM:-4G}" \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
        -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
        -drive format=raw,file=fat:rw:esp \
        -serial "file:$log" -display none -no-reboot \
        -netdev user,id=novusnet -device e1000,netdev=novusnet >/dev/null 2>&1 || true

    for marker in \
        '[VIDMEM] selftest PASS' \
        '[CRTC] selftest PASS' \
        '[BOOT] dwm compositor port ready' \
        '[DWM] compositor starting' \
        '[DWM] primary w='; do
        if grep -Fq "$marker" "$log"; then
            echo "[DWM-TEST] boot marker PASS: $marker"
        else
            echo "[DWM-TEST] boot marker FAIL: $marker" >&2
            exit 1
        fi
    done
fi
