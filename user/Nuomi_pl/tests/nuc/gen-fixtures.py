#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# tests/nuc/gen-fixtures.py -- 生成 NUC 一致性测试向量（与 novus-os 共享的跨端 fixtures）
# 用法: python tests/nuc/gen-fixtures.py
# 依赖: 仓库里的 nuomi.exe / nucutil.exe（路径可用环境变量 NUOMI / NUCUTIL 覆盖；Linux 下分别是 ./nuomi ./nucutil）
# 产物:
#   golden-valid.nuc     合法件, run=42
#   bss-valid.nuc        合法件(含 BSS 段), run=42
#   overflow-mul.nuc     (* 100000 100000), run=10000000000  (锁死 64 位算术+64 位入口返回)
#   bad-*.nuc            非法件, 各对应规范一条规则/错误码
import os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
def _pick(env, win, lin):
    p = os.environ.get(env)
    if p: return p
    w = os.path.join(ROOT, win); l = os.path.join(ROOT, lin)
    return w if os.path.exists(w) else l
NUCUTIL = _pick("NUCUTIL", "nucutil.exe", "nucutil")
NUOMI  = _pick("NUOMI", "nuomi.exe", "nuomi")

def w(path, data): open(path, "wb").write(data); print(f"  wrote {os.path.basename(path)} ({len(data)} bytes)")

def emit_demo(path):
    subprocess.run([NUCUTIL, "emit-demo", path], check=True, capture_output=True)
    return bytearray(open(path, "rb").read())

def compile_mul(path):
    src = os.path.join(HERE, "_mul.tmp")
    open(src, "w").write("(* 100000 100000)\n")
    subprocess.run([NUOMI, "-c", src, "-o", path], check=True, capture_output=True)
    os.remove(src)
    return bytearray(open(path, "rb").read())

def craft_bss(path, bss_file_offset):
    """手工构造 2 段最小文件：TEXT(mov eax,42;ret) + BSS。bss_file_offset=0 合法, 非 0 非法。"""
    text = b"\xB8\x2A\x00\x00\x00\xC3"
    seg_count, sym_count, rel_count = 2, 0, 0
    strtab_off = (48 + seg_count * 32 + 3) & ~3
    data_start = (strtab_off + 1 + 0xFFF) & ~0xFFF
    total = data_start + len(text)
    b = bytearray(total)
    b[0:4] = b"NUC!"
    struct.pack_into("<H", b, 4, 0x0100)            # abi 1.0
    struct.pack_into("<H", b, 6, 0x05)              # 64bit|static
    struct.pack_into("<I", b, 8, 0)                 # entry = TEXT 内偏移 0
    struct.pack_into("<H", b, 12, seg_count)
    struct.pack_into("<H", b, 14, sym_count)
    struct.pack_into("<I", b, 16, strtab_off)
    struct.pack_into("<I", b, 20, 0)                # rel_offset（无重定位）
    struct.pack_into("<I", b, 24, 0x800000)         # stack 8MiB
    struct.pack_into("<I", b, 28, rel_count)
    for i, (t, fo, va, sz, fl) in enumerate([
        (0, data_start, 0x400000, len(text), 0xD),  # TEXT R+X+User
        (2, bss_file_offset, 0x500000, 0x100, 0xB) # BSS R+W+User(bss_file_offset=0 合法)
    ]):
        o = 48 + i * 32
        struct.pack_into("<IIQQ", b, o, t, fo, va, sz)
        struct.pack_into("<I", b, o + 24, fl)
    b[strtab_off] = 0
    b[data_start:data_start + len(text)] = text
    w(path, bytes(b))

def main():
    print(f"NUCUTIL={NUCUTIL}\nNUOMI ={NUOMI}")
    print("合法件:")
    w(os.path.join(HERE, "golden-valid.nuc"), emit_demo(os.path.join(HERE, "golden-valid.nuc")))
    w(os.path.join(HERE, "overflow-mul.nuc"), compile_mul(os.path.join(HERE, "overflow-mul.nuc")))
    craft_bss(os.path.join(HERE, "bss-valid.nuc"), 0)
    print("非法件:")
    # reserved 非 0 → EINVAL
    x = emit_demo(os.path.join(HERE, "bad-reserved.nuc")); x[0x20] = 0xFF
    w(os.path.join(HERE, "bad-reserved.nuc"), x)
    # TEXT 去 VM_USER(bit3) → EINVAL
    x = emit_demo(os.path.join(HERE, "bad-no-vmuser.nuc"))
    struct.pack_into("<I", x, 48 + 24, struct.unpack_from("<I", x, 48 + 24)[0] & ~8)
    w(os.path.join(HERE, "bad-no-vmuser.nuc"), x)
    # TEXT 去 R(bit0) 保留 X → EINVAL "TEXT must be R+X"
    x = emit_demo(os.path.join(HERE, "bad-text-noread.nuc"))
    struct.pack_into("<I", x, 48 + 24, struct.unpack_from("<I", x, 48 + 24)[0] & ~1)
    w(os.path.join(HERE, "bad-text-noread.nuc"), x)
    # STATIC + EXTERN 符号 → EINVAL
    x = emit_demo(os.path.join(HERE, "bad-static-extern.nuc"))
    x[48 + 32 + 5] = 2   # sym0.bind @ 段表后 5
    w(os.path.join(HERE, "bad-static-extern.nuc"), x)
    # BSS 带 file_offset → EINVAL
    craft_bss(os.path.join(HERE, "bad-bssfileoffset.nuc"), 0x999)
    # TEXT 段越界 → EOVERFLOW
    x = emit_demo(os.path.join(HERE, "bad-seg-oob.nuc"))
    struct.pack_into("<I", x, 48 + 4, 0xFFFFFFF0)
    w(os.path.join(HERE, "bad-seg-oob.nuc"), x)
    # reloc sym_idx 越界 → EOVERFLOW
    x = emit_demo(os.path.join(HERE, "bad-rel-symi.nuc"))
    rel_off = struct.unpack_from("<I", x, 20)[0]
    struct.pack_into("<I", x, rel_off + 12, 99)
    w(os.path.join(HERE, "bad-rel-symi.nuc"), x)
    print("done.")

if __name__ == "__main__":
    main()
