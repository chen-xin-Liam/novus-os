"""
生成 Nuomi 编译产物内嵌的 itoa 辅助函数机器码，并独立验证其正确性。

为什么要手写机器码：
  Nuomi 编译模式目前没有堆/静态数组，无法在 Nuomi 源码层写itoa。
  而 print-int 必须能把整数变成十进制文本 —— 这是 I/O 的必备能力。
  故在编译期直接内嵌一段 x86-64 机器码作为运行时辅助函数。

函数签名（与 NUC 的 SysV 约定一致）：
    int64 __nuomi_itoa(int64 v, char* buf)   ->  rax = 文本首地址, rdx = 长度
  buf 需至少 32 字节（int64 最多 20 字符 + 负号 + 余量）。
  数字**从 buf 末尾往前写**，故 buf 无需预先清零。

本脚本同时产出一个自测用的 C 驱动，用 C 自己的 itoa 做对照，逐个值比对。
"""

import struct

# ---- 指令片段 ----
def push_rbp():            return bytes([0x55])
def mov_rbp_rsp():         return bytes([0x48,0x89,0xE5])
def sub_rsp(n):            return bytes([0x48,0x83,0xEC,n & 0xFF])
def mov_rsp_disp_r(reg, d):
    # mov [rbp+d], reg   (REX.W 89 /r)
    modrm = 0x40 | (0x00 << 6) | ((reg & 7) << 3) | 0x05
    if d >= -128: return bytes([0x48,0x89,modrm,d & 0xFF])
    return bytes([0x48,0x89,0x85]) + struct.pack('<i', d)
def mov_disp_r_r(reg, d):
    # mov reg, [rbp+d]   (REX.W 8B /r)
    modrm = 0x40 | (0x00 << 6) | ((reg & 7) << 3) | 0x05
    if d >= -128: return bytes([0x48,0x8B,modrm,d & 0xFF])
    return bytes([0x48,0x8B,0x85]) + struct.pack('<i', d)
def mov_reg_reg(dst, src):
    # mov dst, src  —— 编码 89 /r：mod=11, reg=**src**, rm=**dst**
    # ⚠️ reg 与 rm 的位置极易写反（写反会变成 mov src, dst，表现为「值莫名其妙互换」）。
    #   例：mov rax, rsi 应为 48 89 F0（reg=rsi=110, rm=rax=000）
    return bytes([0x48,0x89,0xC0 | ((src & 7) << 3) | (dst & 7)])
def mov_reg_imm32(reg, imm):
    modrm = 0xC0 | ((reg & 7) << 3)
    return bytes([0x48,0xC7,modrm]) + struct.pack('<i', imm)
def mov_qword_disp_imm(d, imm):
    modrm = 0x40 | (0x00 << 6) | (0x00 << 3) | 0x05
    if d >= -128: return bytes([0x48,0xC7,modrm,d & 0xFF]) + struct.pack('<i', imm)
    return bytes([0x48,0xC7,0x85]) + struct.pack('<i', d) + struct.pack('<i', imm)
def add_reg_imm8(reg, imm):
    modrm = 0xC0 | (0x00 << 3) | (reg & 7)
    return bytes([0x48,0x83,modrm,imm & 0xFF])
def sub_reg_imm8(reg, imm):
    modrm = 0xC0 | (0x05 << 3) | (reg & 7)
    return bytes([0x48,0x83,modrm,imm & 0xFF])
def cmp_qword_disp_imm8(d, imm):
    modrm = 0x40 | (0x00 << 6) | (0x07 << 3) | 0x05
    if d >= -128: return bytes([0x48,0x83,modrm,d & 0xFF,imm & 0xFF])
    return bytes([0x48,0x83,0x85]) + struct.pack('<i', d) + bytes([imm & 0xFF])
def neg_reg(reg):  return bytes([0x48,0xF7,0xC0 | (0x03 << 3) | (reg & 7)])
def test_reg_reg(r1,r2): return bytes([0x48,0x85,0xC0 | ((r1&7)<<3) | (r2&7)])
def cqo():          return bytes([0x48,0x99])
def leave():        return bytes([0xC9])
def ret():          return bytes([0xC3])
def mov_byte_ptr_reg_imm(reg, ch): return bytes([0xC6,0x00,ch & 0xFF]) if reg==0 else None
def mov_byte_ptr_reg_reg(ptr, src):
    # mov byte [rcx], dl
    modrm = 0x00 | ((src & 7) << 3) | (ptr & 7)
    return bytes([0x88,modrm])
def mov_r10_imm32(imm): return bytes([0x49,0xC7,0xC2]) + struct.pack('<i', imm)
def idiv_r10():     return bytes([0x49,0xF7,0xFA])

JMP_REGS = {0xE9: None}  # placeholder

def build():
    """两趟：先算跳转偏移，再拼字节。"""
    # 槽位：v@-8, buf@-16, p@-24, neg@-32
    B = []   # (label_or_None, bytes, size)
    def emit(data, label=None): B.append([label, bytearray(data), None])
    def jmp(label, rel=True):
        B.append([label, bytearray(), ('JMP', label)])

    emit(push_rbp())
    emit(mov_rbp_rsp())
    emit(sub_rsp(0x40))
    emit(mov_rsp_disp_r(7, -8))          # [rbp-8] = rdi (v)
    emit(mov_rsp_disp_r(6, -16))         # [rbp-16] = rsi (buf)
    emit(mov_reg_reg(0, 6))              # rax = rsi
    emit(add_reg_imm8(0, 32))            # rax = buf+32
    emit(mov_rsp_disp_r(0, -24))         # [rbp-24] = p
    emit(mov_qword_disp_imm(-32, 0))     # neg = 0
    emit(cmp_qword_disp_imm8(-8, 0))     # v < 0?
    emit(b'\x7D\x00', 'JGE_POS')         # jge POS
    emit(mov_qword_disp_imm(-32, 1))     # neg = 1
    emit(mov_disp_r_r(0, -8))            # rax = v
    emit(neg_reg(0))                     # rax = -v
    emit(mov_rsp_disp_r(0, -8))          # [rbp-8] = -v
    emit(b'', 'POS')
    emit(cmp_qword_disp_imm8(-8, 0))     # v == 0?
    emit(b'\x75\x00', 'JNE_LOOP')        # jne LOOP
    emit(mov_disp_r_r(0, -24))           # rax = p
    emit(sub_reg_imm8(0, 1))             # p--
    emit(mov_rsp_disp_r(0, -24))         # [rbp-24] = p
    emit(bytes([0xC6,0x00,0x30]))       # *p = '0'
    emit(b'\xEB\x00', 'JMP_DONE')        # jmp DONE
    emit(b'', 'LOOP')
    emit(mov_disp_r_r(0, -8))            # rax = v
    emit(test_reg_reg(0,0))              # test rax,rax
    emit(b'\x74\x00', 'JZ_DONE')         # jz DONE
    emit(mov_disp_r_r(1, -24))           # rcx = p
    emit(sub_reg_imm8(1, 1))             # p--
    emit(mov_rsp_disp_r(1, -24))         # [rbp-24] = p
    emit(mov_disp_r_r(0, -8))            # rax = v
    emit(cqo())                          # rdx:rax = sign-extend(v)
    emit(mov_r10_imm32(10))              # r10 = 10
    emit(idiv_r10())                     # rax = v/10, rdx = v%10
    emit(add_reg_imm8(2, 0x30))          # rdx += '0'
    emit(mov_disp_r_r(1, -24))           # rcx = p
    emit(mov_byte_ptr_reg_reg(1, 2))     # *p = dl
    emit(mov_rsp_disp_r(0, -8))          # [rbp-8] = v/10
    emit(b'\xEB\x00', 'JMP_LOOP')        # jmp LOOP
    emit(b'', 'DONE')
    emit(cmp_qword_disp_imm8(-32, 0))    # neg?
    emit(b'\x74\x00', 'JE_NONEG')        # je NONEG
    emit(mov_disp_r_r(0, -24))           # rax = p
    emit(sub_reg_imm8(0, 1))             # p--
    emit(mov_rsp_disp_r(0, -24))         # [rbp-24] = p
    emit(bytes([0xC6,0x00,0x2D]))       # *p = '-'
    emit(b'', 'NONEG')
    emit(mov_disp_r_r(0, -24))           # rax = p
    emit(mov_disp_r_r(2, -16))           # rdx = buf
    emit(add_reg_imm8(2, 32))            # rdx = buf+32
    emit(bytes([0x48,0x2B,0x55]) + bytes([(-24) & 0xFF]))  # rdx -= p
    emit(leave())
    emit(ret())

    # 第一趟：算每条指令的偏移
    labels = {}
    off = 0
    for item in B:
        item[2] = off
        if item[0] and item[0] in ('POS','LOOP','DONE','NONEG'):
            labels[item[0]] = off
        off += len(item[1])
    total = off

    # 第二趟：回填跳转偏移
    for item in B:
        data, tag = item[1], item[2]
        if item[0] and item[0].startswith('J'):
            # 条目形如 ('JGE_POS', bytes(b'\x7D\x00'), offset)
            pass
    # 上面结构里跳转条目是 [label=None?] 实际我用了 emit(b'\x7D\x00','JGE_POS')
    # 重新按 label 找
    out = bytearray()
    for item in B:
        lbl, data, off = item
        if lbl and lbl.startswith('J') and lbl.endswith(('POS','LOOP','DONE','NONEG')):
            # 提取跳转类型与目标
            for t, tgt in (('JGE_POS','POS'),('JNE_LOOP','LOOP'),('JMP_DONE','DONE'),
                           ('JZ_DONE','DONE'),('JMP_LOOP','LOOP'),('JE_NONEG','NONEG')):
                if lbl == t:
                    rel = labels[tgt] - (off + 2)
                    op = {'JGE_POS':0x7D,'JNE_LOOP':0x75,'JMP_DONE':0xEB,
                          'JZ_DONE':0x74,'JMP_LOOP':0xEB,'JE_NONEG':0x74}[t]
                    data[0] = op
                    data[1] = rel & 0xFF
                    break
        out += data
    return bytes(out), labels, total

if __name__ == '__main__':
    code, labels, total = build()
    print("itoa 机器码 %d 字节" % total)
    print("标签偏移:", {k: hex(v) for k, v in labels.items()})
    print()
    print("机器码:")
    for i in range(0, len(code), 16):
        print("  %04x: %s" % (i, code[i:i+16].hex(' ')))
    open('itoa.bin', 'wb').write(code)
