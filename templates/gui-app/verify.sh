#!/bin/bash
# 脱离主仓单独编译模板，检查产物是否满足 nuc_spawn_blob 的加载约定：
#   - ELF 入口 = 0x400000（link.ld 固定，镜像按该地址映射，无需重定位）
#   - objcopy -O binary 后就是一个可嵌入内核的扁平镜像
set -e
cd "$(dirname "$0")"

if ! command -v cargo > /dev/null 2>&1; then
    [ -x "$HOME/.cargo/bin/cargo" ] && PATH="$HOME/.cargo/bin:$PATH"
fi
if ! command -v cargo > /dev/null 2>&1; then
    echo "cargo not found: install Rust, or add ~/.cargo/bin to PATH" >&2
    exit 1
fi

OBJCOPY=${OBJCOPY:-objcopy}
if ! command -v "$OBJCOPY" > /dev/null 2>&1; then
    OBJCOPY=x86_64-w64-mingw32-objcopy
fi
if ! command -v "$OBJCOPY" > /dev/null 2>&1; then
    echo "no objcopy found: install binutils or the mingw-w64 cross toolchain" >&2
    exit 1
fi

cargo build --release
elf=target/x86_64-unknown-none/release/hello-app
readelf -h "$elf" | grep -E 'Entry point|Type:'
"$OBJCOPY" -O binary "$elf" /tmp/hello-app.bin
wc -c /tmp/hello-app.bin
