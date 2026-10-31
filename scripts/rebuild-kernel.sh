#!/usr/bin/env bash
# Force-rebuild the kernel (NOVUS.EFI) and the live ISO after a kernel-side
# source change, then re-run the headless smoke test.
set -euo pipefail
export PATH="/home/dev/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin:/home/dev/.cargo/bin:${PATH}"
cd /home/dev/novus-os/novus-os
rm -f build/graphics_nd_font.o out/NOVUS.EFI out/novus-live.iso
make iso
echo "=== iso built ==="
ls -la --time-style=+%H:%M:%S out/novus-live.iso
bash scripts/font_smoke.sh > /tmp/smoke_run.log 2>&1 || true
echo "=== smoke done ==="
