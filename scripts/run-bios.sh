#!/usr/bin/env bash
set -euo pipefail

MEM="${1:-4G}"
QEMU="${QEMU:-qemu-system-x86_64}"

cd "$(dirname "$0")/.."

if [ ! -f out/bios.img ]; then
    echo "out/bios.img not found. Run 'make bios-img' first." >&2
    exit 1
fi

exec "$QEMU" -machine pc -smp 2 -m "$MEM" \
    -drive file=out/bios.img,format=raw \
    -serial stdio -no-reboot -display none