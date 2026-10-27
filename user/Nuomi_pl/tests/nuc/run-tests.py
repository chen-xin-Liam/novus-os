#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# tests/nuc/run-tests.py -- 跑 NUC 一致性 fixtures，比对预期输出
# 用法: python tests/nuc/run-tests.py   （先跑 gen-fixtures.py 生成，或直接用仓库里已提交的）
# 预期: 全部 PASS 且退出码 0。任一条 FAIL 退出码 1 —— 可直接挂 CI。
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
def _pick(env, win, lin):
    p = os.environ.get(env)
    if p: return p
    w = os.path.join(ROOT, win); l = os.path.join(ROOT, lin)
    return w if os.path.exists(w) else l
NUCUTIL = _pick("NUCUTIL", "nucutil.exe", "nucutil")

# (文件, 命令, 期望输出包含的片段, 规范依据)
CASES = [
    ("golden-valid.nuc",     "run",     "entry returned: 42",                          "§9 入口加载"),
    ("bss-valid.nuc",        "run",     "entry returned: 42",                          "§4.4 BSS 不占文件空间"),
    ("overflow-mul.nuc",     "run",     "entry returned: 10000000000",                 "64 位算术 + 64 位入口返回"),
    ("bad-reserved.nuc",     "validate","reserved header bytes must be zero",          "§3.2"),
    ("bad-no-vmuser.nuc",    "validate","segment must set VM_USER",                    "§4.3 bit3 必置"),
    ("bad-text-noread.nuc",  "validate","TEXT must be R+X",                            "§4.4"),
    ("bad-static-extern.nuc","validate","static binary must not have EXTERN",          "§10"),
    ("bad-bssfileoffset.nuc","validate","BSS must not occupy file space",              "§4.2/§4.4"),
    ("bad-seg-oob.nuc",      "validate","segment data exceeds file size",              "§2/§12 EOVERFLOW"),
    ("bad-rel-symi.nuc",     "validate","reloc sym_idx out of range",                  "§7.3/§12 EOVERFLOW"),
]

def main():
    if not os.path.exists(NUCUTIL):
        print(f"nucutil not found: {NUCUTIL} (先构建)"); return 2
    npass = 0
    for fname, cmd, expect, ref in CASES:
        path = os.path.join(HERE, fname)
        if not os.path.exists(path):
            print(f"[SKIP] {fname} (不存在, 先跑 gen-fixtures.py)"); continue
        p = subprocess.run([NUCUTIL, cmd, path], capture_output=True, text=True)
        out = (p.stdout or "") + (p.stderr or "")
        ok = expect in out
        print(f"[{'PASS' if ok else 'FAIL'}] {cmd:8s} {fname:24s} 期望含: {expect!r}  ({ref})")
        if not ok:
            print(f"       实得: {out.strip()[:120]!r}")
        npass += ok
    total = len(CASES)
    print(f"\n{npass}/{total} passed")
    return 0 if npass == total else 1

if __name__ == "__main__":
    sys.exit(main())
