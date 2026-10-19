#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."
HOSTCC="${HOSTCC:-cc}"
flags=(-Iarch/x86_64/include -Ikernel -Igraphics -Iwm/novus-wm
       -O2 -ffreestanding -fno-builtin -fwhole-program -fno-pie
       -ffunction-sections -fdata-sections -Wall -Wextra -Werror)
link=(-Wl,--gc-sections -no-pie)
case "$("$HOSTCC" -dumpmachine)" in
    *mingw*) link+=(-Wl,--image-base,0x400000 -Wl,--disable-dynamicbase) ;;
esac

mkdir -p build/tests
"$HOSTCC" "${flags[@]}" -DCONFIG_NOVUS_NET=1 -DCONFIG_NOVUS_IRQ=1 \
    scripts/irq-selftest.c "${link[@]}" -o build/tests/irq-selftest.exe
build/tests/irq-selftest.exe
"$HOSTCC" "${flags[@]}" scripts/sched-selftest.c "${link[@]}" \
    -o build/tests/sched-selftest.exe
build/tests/sched-selftest.exe