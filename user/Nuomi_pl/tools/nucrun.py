#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""nucrun.py —— 用 unicorn（QEMU CPU 内核）模拟执行 .nuc 编译产物

替代此前手写的 x86sim.py（那个模拟器有多个难缠的 bug，调试成本过高）。
unicorn 是成熟 CPU 模拟器，指令语义完全可信；本脚本只负责：

1. 解析 .nuc 头/段表/符号表/重定位表（布局见 src/nucfmt.c 的写入顺序）
2. 按各段的 vaddr 映射内存（TEXT=0x400000, RODATA=0x500000, BSS=0x600000）
3. 应用重定位（R_X86_64_PC32=2：符号段内偏移 - 调用点段内偏移 - 4）
4. 搭好栈，从 entry_point 开跑
5. 用 UC_HOOK_INTR 截获 int 0x80，按 include/novus_syscall.h 的号表
   模拟 novus 内核的 write/exit（宿主替身，用于本机端到端验证 I/O 链路）

用法：
    python nucrun.py <file.nuc>            # 跑，打印 write 的输出
    python nucrun.py -v <file.nuc>         # 额外打印段表/符号/重定位/syscall 明细
"""
import struct
import sys

from unicorn import (
    Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_INTR,
    UC_PROT_ALL,
)
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
    UC_X86_REG_RIP, UC_X86_REG_RSP, UC_X86_REG_RBP,
)

# ---- 与编译器一致的段虚拟地址（src/codegen.c 组装段） ----
TEXT_VADDR = 0x400000
DATA_VADDR = 0x400000 + 0x100000      # RODATA = 0x500000
BSS_VADDR = 0x600000

# ---- NUC 段类型（include/nucfmt.h） ----
SEG_TEXT, SEG_DATA, SEG_BSS, SEG_RODATA = 0, 1, 2, 3

# ---- 重定位类型（include/nucfmt.h） ----
R_X86_64_32, R_X86_64_PC32 = 1, 2

# ---- syscall 号（include/novus_syscall.h，逆向自内核镜像，待官方确认） ----
NOVUS_SYS_WRITE = 1
NOVUS_SYS_EXIT = 7
NOVUS_FD_STDOUT, NOVUS_FD_STDERR = 1, 2

# 栈：放在一个独立的高地址区，1MB 足够（入口 prologue sub rsp,0x100 等）
STACK_BASE = 0x7000_0000
STACK_SIZE = 0x10_0000
STACK_TOP = STACK_BASE + STACK_SIZE - 0x1000


class NucFile:
    """.nuc 解析结果"""

    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        if d[:3] != b'NUC':
            raise ValueError('不是 .nuc 文件')
        self.entry = struct.unpack_from('<I', d, 8)[0]
        segc, symc = struct.unpack_from('<HH', d, 12)
        self.stroff, self.reloff = struct.unpack_from('<II', d, 16)[:2]
        self.stack_sz = struct.unpack_from('<I', d, 24)[0]
        relc = struct.unpack_from('<H', d, 28)[0]

        # 段表项 32B：type(4) file_off(4) vaddr(8) size(8) flags(4) rsvd(4)
        self.segs = []
        for i in range(segc):
            o = 48 + i * 32
            t, foff = struct.unpack_from('<II', d, o)
            vaddr, size = struct.unpack_from('<QQ', d, o + 8)
            flags, = struct.unpack_from('<I', d, o + 24)
            self.segs.append(dict(type=t, foff=foff, vaddr=vaddr, size=size, flags=flags))

        # 符号表项 24B：name_off(4) type(1) bind(1) shndx(2) value(8) size(8)
        self.syms = []
        for i in range(symc):
            o = 48 + segc * 32 + i * 24
            nameoff, = struct.unpack_from('<I', d, o)
            typ, bind = d[o + 4], d[o + 5]
            shndx, = struct.unpack_from('<H', d, o + 6)
            value, size = struct.unpack_from('<QQ', d, o + 8)
            end = d.index(b'\x00', self.stroff + nameoff)
            name = d[self.stroff + nameoff:end].decode('latin1')
            self.syms.append(dict(name=name, type=typ, bind=bind, shndx=shndx,
                                  value=value, size=size))

        # 重定位表项 16B：r_offset(8) r_type(4) sym_idx(4)
        self.rels = []
        for i in range(relc):
            o = self.reloff + i * 16
            roff, = struct.unpack_from('<Q', d, o)
            rt, si = struct.unpack_from('<II', d, o + 8)
            self.rels.append(dict(off=roff, type=rt, sym=si))

    def seg(self, want):
        for s in self.segs:
            if s['type'] == want:
                return s
        return None


def apply_relocs(nf, tcode):
    """在 TEXT 段内容上应用重定位。tcode: bytearray（段内容）"""
    for r in nf.rels:
        sym = nf.syms[r['sym']]
        site = r['off'] - TEXT_VADDR            # 调用点在 TEXT 段内偏移
        if r['type'] == R_X86_64_PC32:
            # 符号 value = 其所在段内的偏移；本编译器符号全在 TEXT 段
            rel = (sym['value'] - site - 4) & 0xFFFFFFFF
            struct.pack_into('<I', tcode, site, rel)
        elif r['type'] == R_X86_64_32:
            struct.pack_into('<I', tcode, site, sym['value'] & 0xFFFFFFFF)
        else:
            raise ValueError('不支持的重定位类型 %d' % r['type'])


def run(path, verbose=False, capture=True):
    """模拟执行 .nuc，返回 (stdout_bytes, exit_code)"""
    nf = NucFile(path)
    text = nf.seg(SEG_TEXT)
    rodata = nf.seg(SEG_RODATA)
    bss = nf.seg(SEG_BSS)

    if verbose:
        print('=== 段表 ===')
        for s in nf.segs:
            print('  type=%d vaddr=0x%X size=%d file_off=0x%X flags=0x%X'
                  % (s['type'], s['vaddr'], s['size'], s['foff'], s['flags']))
        print('=== 符号 ===')
        for s in nf.syms:
            print('  %-28s value=0x%X size=%d' % (s['name'], s['value'], s['size']))
        print('=== 重定位 ===')
        for r in nf.rels:
            print('  off=0x%X type=%d sym=%s' % (r['off'], r['type'], nf.syms[r['sym']]['name']))

    tcode = bytearray(nf.d[text['foff']:text['foff'] + text['size']])
    apply_relocs(nf, tcode)

    uc = Uc(UC_ARCH_X86, UC_MODE_64)

    # ---- 映射各段（unicorn 按页映射，大小向上取整到 4KB） ----
    def map_seg(vaddr, size, content=None):
        lo = vaddr & ~0xFFF
        hi = (vaddr + size + 0xFFF) & ~0xFFF
        uc.mem_map(lo, hi - lo, UC_PROT_ALL)
        if content:
            uc.mem_write(vaddr, bytes(content))

    map_seg(text['vaddr'], text['size'], tcode)
    if rodata:
        map_seg(rodata['vaddr'], rodata['size'], nf.d[rodata['foff']:rodata['foff'] + rodata['size']])
    if bss:
        map_seg(bss['vaddr'], bss['size'])      # BSS 全零即可
    uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)

    # ---- 寄存器 ----
    uc.reg_write(UC_X86_REG_RSP, STACK_TOP)
    uc.reg_write(UC_X86_REG_RBP, STACK_TOP)
    uc.reg_write(UC_X86_REG_RIP, text['vaddr'] + nf.entry)

    # ---- int 0x80 → 模拟 novus 内核 ----
    out = bytearray()
    err = bytearray()
    exit_code = [0]

    def on_intr(uc_, intno, user):
        if intno != 0x80:
            raise RuntimeError('未处理的中断 int 0x%X @ rip=0x%X'
                               % (intno, uc_.reg_read(UC_X86_REG_RIP)))
        rax = uc_.reg_read(UC_X86_REG_RAX)
        rdi = uc_.reg_read(UC_X86_REG_RDI)
        rsi = uc_.reg_read(UC_X86_REG_RSI)
        rdx = uc_.reg_read(UC_X86_REG_RDX)
        if rax == NOVUS_SYS_WRITE:
            data = bytes(uc_.mem_read(rsi, rdx)) if rdx else b''
            (err if rdi == NOVUS_FD_STDERR else out).extend(data)
            uc_.reg_write(UC_X86_REG_RAX, rdx)      # 返回写入字节数
            if verbose:
                print('  [syscall] write(fd=%d, buf=0x%X, len=%d) -> %d'
                      % (rdi, rsi, rdx, rdx))
        elif rax == NOVUS_SYS_EXIT:
            exit_code[0] = rdi & 0xFF
            if verbose:
                print('  [syscall] exit(%d)' % (rdi & 0xFF))
            uc_.emu_stop()
        else:
            uc_.reg_write(UC_X86_REG_RAX, (-38) & 0xFFFFFFFFFFFFFFFF)  # -ENOSYS

    uc.hook_add(UC_HOOK_INTR, on_intr)

    try:
        uc.emu_start(text['vaddr'] + nf.entry, 0)
    except Exception as e:  # unicorn 的 UcError
        print('[nucrun] 执行异常: %s (rip=0x%X)' % (e, uc.reg_read(UC_X86_REG_RIP)))
        raise

    if err:
        sys.stderr.buffer.write(bytes(err))
    if capture:
        return bytes(out), exit_code[0]
    sys.stdout.buffer.write(bytes(out))
    sys.stdout.flush()
    return None, exit_code[0]


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    verbose = '-v' in sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    data, code = run(args[0], verbose=verbose)
    if data is not None:
        sys.stdout.buffer.write(data)
        sys.stdout.flush()
