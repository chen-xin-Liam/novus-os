#!/usr/bin/env bash
#
# Novus OS - QEMU runner for the live ISO
#
# Boots out/novus-live.iso as a CD-ROM. The firmware reads the El Torito
# boot catalog, loads the embedded FAT32 ESP image as a virtual disk and
# starts EFI/BOOT/BOOTX64.EFI from it - the same path a real machine
# takes, which is the whole point of building an ISO rather than booting
# the bare .EFI off a vvfat directory.
#
# Usage: scripts/run-iso.sh [MEM] [DISK]
#   MEM   guest memory size, defaults to 4G
#   DISK  optional second drive, e.g. an install target image. Passed
#         through as a raw virtio-blk device. Nothing is written to it
#         unless the live menu's installer is told to.

set -euo pipefail

MEM="${1:-4G}"
DISK="${2:-}"

cd "$(dirname "$0")/.."

if [ ! -f out/novus-live.iso ]; then
    echo "out/novus-live.iso not found. Run 'make iso' first." >&2
    exit 1
fi

# Pristine non-volatile variables on first use, so firmware state (boot
# order, the installed-system boot entry) survives across runs.
if [ ! -f out/OVMF_VARS.fd ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd out/OVMF_VARS.fd
fi

if [ -n "${DISPLAY:-}" ]; then
    DISPLAY_ARGS="-display gtk"
else
    DISPLAY_ARGS="-display none"
fi

DISK_ARGS=()
if [ -n "$DISK" ]; then
    if [ ! -e "$DISK" ]; then
        echo "disk image not found: $DISK" >&2
        exit 1
    fi
    DISK_ARGS=(-drive "format=raw,file=$DISK,if=ide")
fi

# -no-reboot turns a triple fault into an exit instead of a loop, and the
# serial port on stdio is where the live menu and the Merkle report go.
exec qemu-system-x86_64 -machine q35 -smp 2 -m "$MEM" \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
    -cdrom out/novus-live.iso \
    "${DISK_ARGS[@]}" \
    -serial stdio -no-reboot $DISPLAY_ARGS -net none
