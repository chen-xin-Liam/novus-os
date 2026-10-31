#!/usr/bin/env bash
# Boot the live ISO headless, capture serial, and report font/boot markers.
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f out/novus-live.iso ] || { echo "out/novus-live.iso missing" >&2; exit 1; }
# Fresh NVRAM each run so a stale boot order (e.g. the internal shell) does
# not win over the El Torito image.
cp /usr/share/OVMF/OVMF_VARS_4M.fd out/OVMF_VARS.fd
rm -f out/smoke.log
timeout 50 qemu-system-x86_64 -machine q35 -smp 2 -m 4G \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=out/OVMF_VARS.fd \
  -cdrom out/novus-live.iso \
  -serial file:out/smoke.log -no-reboot -display none -net none || true
echo "===== FONT / boot markers ====="
grep -E "\[FONT\]|\[M3\]|DEVTAB|NOVUS OS|panic|assert" out/smoke.log | head -20 || true
echo "===== line count ====="
wc -l out/smoke.log
