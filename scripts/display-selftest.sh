#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
HOSTCC="${HOSTCC:-cc}"
flags=(-Iarch/x86_64/include -Ikernel -Igraphics -Iwm/novus-wm -Ifs/novusfs -Ibuild
       -O2 -ffreestanding -fno-builtin -ffunction-sections -fdata-sections
       -Wall -Wextra -Werror)
link=(-Wl,--gc-sections)
case "$("$HOSTCC" -dumpmachine)" in
    *mingw*)
        link=(-Wl,--gc-sections -Wl,--image-base,0x400000 -Wl,--disable-dynamicbase)
        ;;
esac
mkdir -p build/tests
python3 tools/fontgen/fallback.py build/nd_font_fallback.h
"$HOSTCC" "${flags[@]}" scripts/memory-selftest.c "${link[@]}" \
    -o build/tests/memory-selftest.exe
build/tests/memory-selftest.exe
"$HOSTCC" "${flags[@]}" -DCONFIG_NOVUS_GUI=1 -DND_SELFTEST_ONLY=1 scripts/display-selftest.c \
    graphics/nd_core.c graphics/nd_font.c graphics/nd_font_uni.c \
    graphics/nd_lang.c graphics/nd_lang_dict.c graphics/nd_theme.c \
    graphics/nd_jpeg.c graphics/nd_demo.c \
    graphics/nd_ime.c graphics/nd_ime_dict.c "${link[@]}" \
    -o build/tests/display-selftest.exe
output=build/tests/wallpaper-selftest.raw
trap 'rm -f -- "$output"' EXIT
build/tests/display-selftest.exe \
    assets/backgrounds-screen/pexels-snapsbyclark-12981877.jpg "$output"
