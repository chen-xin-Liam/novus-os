#!/usr/bin/env bash
# Rebuild the NovusFS system partition image from current source.
set -euo pipefail
export PATH="/home/dev/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin:/home/dev/.cargo/bin:${PATH}"
cd /home/dev/novus-os/novus-os
rm -f out/novus.img build/tools/mkfsnovus build/tools/tools_mkfsnovus_mkfsnovus.o
make out/novus.img
python3 scripts/inspect_img.py out/novus.img
