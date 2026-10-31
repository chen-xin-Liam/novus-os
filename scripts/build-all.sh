#!/usr/bin/env bash
# Full clean rebuild of tools, kernel (NOVUS.EFI), system image, ESP and ISO.
set -euo pipefail
export PATH="/home/dev/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin:/home/dev/.cargo/bin:${PATH}"
cd /home/dev/novus-os/novus-os
make clean
make iso
python3 scripts/inspect_img.py out/novus.img
