// SPDX-License-Identifier: GPL-2.0
/*
 * codegen.c — Route-A 编译后端：Nuomi 子集 → x86_64 机器码 → NUC 文件
 *
 * M2 里程碑：整数 + 函数(可递归) + 局部变量 + if/while/begin + 比较 + 跨函数调用。
 *
 * 严格遵循《NUC 可执行二进制格式规范 V1.0》：
 *   - 每个函数一个 FUNC/LOCAL 符号（§5），跨函数调用生成 R_X86_64_PC32 重定位（§7），
 *     加载器按 §7.3 在装载期解析 —— 这正是 novus-os 加载器的职责；
 *   - entry_point 为 .text 段内字节偏移（§9.1），entry_rip = text.vaddr + entry_point；
 *   - TEXT 段 R|X|USER（§4.3 bit3 必置），W^X。
 * 调用约定：System V AMD64（rdi,rsi,rdx,rcx,r8,r9 传参 / rax 返回），
 * 与 NUC 规范 §1 声明的系统调用参数寄存器一致。
 *
 * 本里程碑已知限制：仅整数；无字符串/向量/内置 I/O（novus 系统调用号表未公布，
 * 公布后加 (syscall n ...) 即可）；布尔用 1/0（0=假）；while 返回循环结果；
 * 最多 6 参数；每函数最多 16 个局部槽；顶层 define 为编译期常量（不可 set!）。
 */
#include "nuomi.h"
#include "nucfmt.h"
#include "novus_syscall.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ====================== 内置运行时辅助：itoa ======================
 *
 * print-int 需要把整数变成十进制文本。编译模式目前没有堆、也没有静态数组，
 * 无法用 Nuomi 源码写这段逻辑，故在编译期直接内嵌一段 x86-64 机器码，
 * 作为**内置函数** __nuomi_itoa 发射到 .text 段。
 *
 * 签名（SysV AMD64，与 NUC 规范 §1 一致）：
 *     void* __nuomi_itoa(int64 v, char* buf)
 *   rdi = 数值, rsi = 缓冲区（至少 40 字节）
 *   返回 rax = 文本首地址, rdx = 字节长度
 * 数字从缓冲区末尾往前写，故缓冲区无需预先清零。
 *
 * 这段机器码由 tools/itoa_gen.py 生成，并用 x86-64 子集模拟器
 * （tools/x86sim.py）验证过 2017 个取值（含 0/正负/边界/±10^12）全部正确。
 * ⚠️ 改动本数组务必重新跑模拟器验证。
 */
static const uint8_t NUOMI_ITOA_CODE[178] = {
    0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x89, 0x7D, 0xF8,
    0x48, 0x89, 0x75, 0xF0, 0x48, 0x89, 0xF0, 0x48, 0x83, 0xC0, 0x20, 0x48,
    0x89, 0x45, 0xE8, 0x48, 0xC7, 0x45, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x83, 0x7D, 0xF8, 0x00, 0x7D, 0x13, 0x48, 0xC7, 0x45, 0xE0, 0x01, 0x00,
    0x00, 0x00, 0x48, 0x8B, 0x45, 0xF8, 0x48, 0xF7, 0xD8, 0x48, 0x89, 0x45,
    0xF8, 0x48, 0x83, 0x7D, 0xF8, 0x00, 0x75, 0x11, 0x48, 0x8B, 0x45, 0xE8,
    0x48, 0x83, 0xE8, 0x01, 0x48, 0x89, 0x45, 0xE8, 0xC6, 0x00, 0x30, 0xEB,
    0x35, 0x48, 0x8B, 0x45, 0xF8, 0x48, 0x85, 0xC0, 0x74, 0x2C, 0x48, 0x8B,
    0x4D, 0xE8, 0x48, 0x83, 0xE9, 0x01, 0x48, 0x89, 0x4D, 0xE8, 0x48, 0x8B,
    0x45, 0xF8, 0x48, 0x99, 0x49, 0xC7, 0xC2, 0x0A, 0x00, 0x00, 0x00, 0x49,
    0xF7, 0xFA, 0x48, 0x83, 0xC2, 0x30, 0x48, 0x8B, 0x4D, 0xE8, 0x88, 0x11,
    0x48, 0x89, 0x45, 0xF8, 0xEB, 0xCB, 0x48, 0x83, 0x7D, 0xE0, 0x00, 0x74,
    0x0F, 0x48, 0x8B, 0x45, 0xE8, 0x48, 0x83, 0xE8, 0x01, 0x48, 0x89, 0x45,
    0xE8, 0xC6, 0x00, 0x2D, 0x48, 0x8B, 0x45, 0xE8, 0x48, 0x8B, 0x55, 0xF0,
    0x48, 0x83, 0xC2, 0x20, 0x48, 0x2B, 0x55, 0xE8, 0xC9, 0xC3,
};

/* itoa 用的可写缓冲区（放BSS 段，运行时由内核按零初始化） */
#define NUOMI_ITOA_BUFSZ 64

/* ====================== 字节缓冲 ====================== */
typedef struct { uint8_t* p; size_t len, cap; } CGBuf;
static void cg_init(CGBuf* b) { b->p = NULL; b->len = 0; b->cap = 0; }
static void cg_put(CGBuf* b, const uint8_t* d, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) > 64 ? (b->len + n) * 2 : 64;
        b->p = (uint8_t*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void cg_u8(CGBuf* b, uint8_t v)   { cg_put(b, &v, 1); }
static void cg_u32(CGBuf* b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v&0xff),(uint8_t)((v>>8)&0xff),(uint8_t)((v>>16)&0xff),(uint8_t)((v>>24)&0xff) }; cg_put(b, t, 4); }
static void cg_u64(CGBuf* b, uint64_t v) { uint8_t t[8]; for (int i=0;i<8;i++) t[i]=(uint8_t)((v>>(8*i))&0xff); cg_put(b, t, 8); }
static void cg_patch_u32(CGBuf* b, size_t pos, uint32_t v) {
    for (int i = 0; i < 4; i++) b->p[pos + i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

/* ====================== 指令编码（x86_64, System V） ====================== */
static void cg_mov_rax_imm(CGBuf* b, int64_t v) {
    if (v >= -2147483648LL && v <= 2147483647LL) {
        cg_u8(b,0x48); cg_u8(b,0xC7); cg_u8(b,0xC0); cg_u32(b,(uint32_t)(int32_t)v); /* mov rax, imm32(符号扩展) */
    } else {
        cg_u8(b,0x48); cg_u8(b,0xB8); cg_u64(b,(uint64_t)v);                          /* movabs rax, imm64 */
    }
}
static void cg_push_rax(CGBuf* b)  { cg_u8(b,0x50); }
static void cg_pop_rdx(CGBuf* b)   { cg_u8(b,0x5A); }                  /* pop rdx */
static void cg_add_rax_rdx(CGBuf* b){ cg_u8(b,0x48); cg_u8(b,0x01); cg_u8(b,0xD0); }
static void cg_imul_rax_rdx(CGBuf* b){ cg_u8(b,0x48); cg_u8(b,0x0F); cg_u8(b,0xAF); cg_u8(b,0xC2); }
static void cg_neg_rax(CGBuf* b)   { cg_u8(b,0x48); cg_u8(b,0xF7); cg_u8(b,0xD8); }
static void cg_xor_eax(CGBuf* b)   { cg_u8(b,0x31); cg_u8(b,0xC0); }
static void cg_cmp_rdx_rax(CGBuf* b){ cg_u8(b,0x48); cg_u8(b,0x39); cg_u8(b,0xC2); } /* cmp rdx, rax (左-右) */
static void cg_setcc_al(CGBuf* b, uint8_t cc){ cg_u8(b,0x0F); cg_u8(b,cc); cg_u8(b,0xC0); }
static void cg_movzx_eax_al(CGBuf* b){ cg_u8(b,0x0F); cg_u8(b,0xB6); cg_u8(b,0xC0); }
static void cg_test_rax_rax(CGBuf* b){ cg_u8(b,0x48); cg_u8(b,0x85); cg_u8(b,0xC0); }
static void cg_jmp_rel32(CGBuf* b)       { cg_u8(b,0xE9); cg_u32(b,0); }
static void cg_jcc_rel32(CGBuf* b, uint8_t cc){ cg_u8(b,0x0F); cg_u8(b,cc); cg_u32(b,0); }
static void cg_call_rel32(CGBuf* b)      { cg_u8(b,0xE8); cg_u32(b,0); }
static void cg_ret(CGBuf* b)             { cg_u8(b,0xC3); }
static void cg_leave(CGBuf* b)           { cg_u8(b,0xC9); }
static void cg_prologue(CGBuf* b) {
    cg_u8(b,0x55);                                   /* push rbp */
    cg_u8(b,0x48); cg_u8(b,0x89); cg_u8(b,0xE5);     /* mov rbp, rsp */
    cg_u8(b,0x48); cg_u8(b,0x81); cg_u8(b,0xEC);     /* sub rsp, 256 (32 槽: 参数+局部+隐藏) */
    cg_u8(b,0x00); cg_u8(b,0x01); cg_u8(b,0x00); cg_u8(b,0x00);
}
/* 存第 regidx 个参数寄存器到 [rbp+disp]（disp 为负——参数槽在被调函数自己帧内，
   绝不写 [rbp+16..]：那会覆盖调用方压栈暂存的中间值！这是本编译器最关键的栈纪律。） */
static void cg_store_reg_local(CGBuf* b, int regidx, int disp) {
    static const uint8_t base[6][3] = {
        {0x48,0x89,0x7D},{0x48,0x89,0x75},{0x48,0x89,0x55},
        {0x48,0x89,0x4D},{0x4C,0x89,0x45},{0x4C,0x89,0x4D},
    };
    static const uint8_t base32[6][3] = {
        {0x48,0x89,0xBD},{0x48,0x89,0xB5},{0x48,0x89,0x95},
        {0x48,0x89,0x8D},{0x4C,0x89,0x85},{0x4C,0x89,0x8D},
    };
    if (disp >= -128) { cg_put(b, base[regidx], 3); cg_u8(b, (uint8_t)(uint8_t)disp); }
    else { cg_put(b, base32[regidx], 3); cg_u32(b, (uint32_t)(int32_t)disp); }
}
static void cg_load_rax_local(CGBuf* b, int disp) {   /* disp 为负 */
    if (disp >= -128) { cg_u8(b,0x48); cg_u8(b,0x8B); cg_u8(b,0x45); cg_u8(b,(uint8_t)disp); }
    else { cg_u8(b,0x48); cg_u8(b,0x8B); cg_u8(b,0x85); cg_u32(b,(uint32_t)(int32_t)disp); }
}
static void cg_store_rax_local(CGBuf* b, int disp) {
    if (disp >= -128) { cg_u8(b,0x48); cg_u8(b,0x89); cg_u8(b,0x45); cg_u8(b,(uint8_t)disp); }
    else { cg_u8(b,0x48); cg_u8(b,0x89); cg_u8(b,0x85); cg_u32(b,(uint32_t)(int32_t)disp); }
}
/* 存 rdx 到局部槽（mov [rbp+disp], rdx）—— 并行 let 弹栈后直接落槽用。
 *
 * 编码陷阱（务必留意）：x86-64 的 ModRM.mod 字段含义是
 *   mod=00 → 无位移（rm=101 时例外，见下）    mod=01 → disp8
 *   mod=10 → disp32
 * 「rbp + 负位移」的 1 字节符号扩展形式必须用 mod=01（ModRM = 0x55 表示 reg=rdx），
 * 曾误写成 mod=10（0x95）却只给1 字节位移，CPU 会把后续字节当作 disp32 继续读，
 * 整条指令错位 → 生成的 .nuc 一执行就崩。 */
static void cg_store_rdx_local(CGBuf* b, int disp) {
    if (disp >= -128) { cg_u8(b,0x48); cg_u8(b,0x89); cg_u8(b,0x55); cg_u8(b,(uint8_t)disp); }  /* mod=01 */
    else              { cg_u8(b,0x48); cg_u8(b,0x89); cg_u8(b,0x95); cg_u32(b,(uint32_t)(int32_t)disp); } /* mod=10 */
}
/* lea rax, [rip+disp32] (48 8D 05 xx xx xx xx) —— 取 RODATA 里字符串的地址。
 * 用 RIP 相对寻址：位置无关，且只需7 字节；disp32 在链接/装载期由重定位补上。 */
static void cg_lea_rax_rip(CGBuf* b) { cg_u8(b,0x48); cg_u8(b,0x8D); cg_u8(b,0x05); cg_u32(b,0); }

/* ---- 系统调用发射 ----
 * 约定（见 include/novus_syscall.h）：
 *   rax = syscall 号，rdi/rsi/rdx = 参数，返回值在 rax
 *   （第 4 参是 r10，见 include/novus_syscall.h）
 *   入口是 syscall 指令（官方规范 §2：MSR_LSTAR → syscall_entry，非 int 0x80）
 *
 * 实现方式：所有 syscall 站点都 `call` 到 .text 段里的**统一跳板**，
 * 跳板内容是 `syscall ; ret`（真机上：进入内核 → 返回 → 回到调用点）。
 * 这样做的好处：
 *   ① syscall 指令只出现一次，宿主/调试器只需处理一个位置；
 *   ② 跳板有富余字节，宿主可原地把它替换成「调宿主桩」的序列，
 *      从而在本机端到端验证 I/O 链路（真机语义不受影响）。
 */
static void cg_mov_rdi_imm(CGBuf* b, int64_t v) { cg_u8(b,0x48); cg_u8(b,0xBF); cg_u64(b,(uint64_t)v); } /* mov rdi, imm64 */
static void cg_mov_rsi_imm(CGBuf* b, int64_t v) { cg_u8(b,0x48); cg_u8(b,0xBE); cg_u64(b,(uint64_t)v); } /* mov rsi, imm64 */
static void cg_mov_rdx_imm(CGBuf* b, int64_t v) { cg_u8(b,0x48); cg_u8(b,0xBA); cg_u64(b,(uint64_t)v); } /* mov rdx, imm64 */
static void cg_mov_rcx_imm(CGBuf* b, int64_t v) { cg_u8(b,0x48); cg_u8(b,0xB9); cg_u64(b,(uint64_t)v); } /* mov rcx, imm64 */
/* syscall 跳板：syscall ; ret。放在 .text 段末尾，符号 __nuomi_syscall_trampoline */
#define NUOMI_SYSCALL_TRAMP_SIZE 32

/* ====================== 符号辅助 ====================== */
static char* cg_strdup(const char* s) { size_t l = strlen(s)+1; char* p = (char*)malloc(l); memcpy(p,s,l); return p; }
static bool cg_isSymbol(Value v) {
    return IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING && ((ObjString*)AS_OBJ(v))->isSymbol;
}
static const char* cg_symChars(Value v) { return ((ObjString*)AS_OBJ(v))->chars; }

/* 编译期常量表（顶层 (define name expr)） */
typedef struct { char* name; int64_t val; } Const;
typedef struct { Const* c; int n, cap; } ConstTab;
static void ct_add(ConstTab* t, const char* name, int64_t v) {
    for (int i=0;i<t->n;i++) if (strcmp(t->c[i].name,name)==0){ t->c[i].val=v; return; }
    if (t->n==t->cap){ t->cap = t->cap<4?4:t->cap*2; t->c=(Const*)realloc(t->c,t->cap*sizeof(Const)); }
    t->c[t->n].name = cg_strdup(name); t->c[t->n].val = v; t->n++;
}
static bool ct_find(ConstTab* t, const char* name, int64_t* out) {
    for (int i=0;i<t->n;i++) if (strcmp(t->c[i].name,name)==0){ *out=t->c[i].val; return true; }
    return false;
}

/* 函数定义表 */
typedef struct {
    char* name;
    Value params;      /* 符号列表 */
    Value body;        /* 形式列表 */
    int nparams;
    int symidx;        /* NUC 符号表下标 */
    uint64_t offset;   /* 在 .text 中的偏移 */
    uint64_t size;
} FuncDef;
typedef struct { FuncDef* f; int n, cap; } FuncTab;

/* 局部变量（参数为正偏移，局部为负偏移） */
typedef struct { char* name; int disp; } Local;

/* 重定位（记录最终 .text 内偏移，构建时换算成 VA） */
typedef struct { size_t text_pos; int symidx; } CGRel;

/* ---- 只读数据段（字符串字面量）----
 * 布局见 nucfmt.h 的「Nuomi 字符串的运行时内存布局」：
 *   [int64 len][int64 hash][bytes...]，整体按 8 字节对齐。
 * 字符串引用表达式的值 = 该字符串首字节的地址（一个指针即可代表整串）。 */
typedef struct { CGBuf buf; uint64_t vaddr; } CGRodata;

/* FNV-1a 64 位哈希：运行时比较字符串前先比它 */
static uint64_t nuomiHash(const uint8_t* d, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
    return h;
}

/* 把一个字符串字面量放进 RODATA，返回它在段内的偏移 */
static size_t rodataAddStr(CGRodata* rd, const char* s, int len) {
    /* 去重：完全相同的字面量只存一份，运行时比较与体积都受益 */
    for (size_t off = 0; off + NUOMI_STR_HEADER <= rd->buf.len; ) {
        int64_t elen;
        memcpy(&elen, rd->buf.p + off, 8);
        size_t total = NUOMI_STR_BYTES((size_t)elen);
        if (elen == len && memcmp(rd->buf.p + off + NUOMI_STR_HEADER, s, (size_t)len) == 0)
            return off;                       /* 命中已有字面量 */
        off += total;
    }
    size_t start = rd->buf.len;
    uint64_t hash = nuomiHash((const uint8_t*)s, (size_t)len);
    size_t total = NUOMI_STR_BYTES((size_t)len);
    uint8_t pad[NUOMI_STR_HEADER + 8];
    memset(pad, 0, sizeof pad);
    int64_t l64 = (int64_t)len, h64 = (int64_t)hash;
    memcpy(pad, &l64, 8);
    memcpy(pad + 8, &h64, 8);
    cg_put(&rd->buf, pad, NUOMI_STR_HEADER);
    cg_put(&rd->buf, (const uint8_t*)s, (size_t)len);
    size_t tail = total - NUOMI_STR_HEADER - (size_t)len;   /* 补齐到 8 字节对齐 */
    for (size_t i = 0; i < tail; i++) cg_u8(&rd->buf, 0);
    return start;
}

/* ====================== 编译上下文 ====================== */
typedef struct {
    FuncTab ft;
    ConstTab ct;
    CGBuf* out;        /* 当前发射缓冲（函数用临时缓冲，入口直接用 .text） */
    /* 标签/跳转修复（针对当前 out） */
    struct { int id; size_t pos; } *labs; int nlab, labcap;
    struct { int id; size_t pos; } *fixs; int nfix, fixcap;
    int nextlab;
    /* 变量作用域（每函数重建） */
    Local* vars; int nvar, varcap;
    int nslots;        /* 已用局部槽（负偏移计数） */
    int* scopes; int nsc, sccap;   /* 局部作用域栈（let 进出） */
    /* 重定位收集（最终 .text 偏移） */
    CGRel* rels; int nrel, relcap;
    size_t text_base;  /* 本次发射内容将拼接进 .text 的起始偏移 */
    CGRodata rodata;   /* 字符串字面量池 */
    /* lea rax,[rip+d] 里d 的延迟回填：记录 (text内lea字段偏移, rodata内目标偏移)。
     * text 与 rodata 最终拼进文件时的相对位置要到收尾才确定，故先记后补。 */
    struct { size_t text_pos; size_t rodata_off; } *strfix; int nstrfix, strfixcap;
    char err[256]; size_t errlen;
} CG;

static void cg_strfix_add(CG* c, size_t text_pos, size_t rodata_off) {
    if (c->nstrfix == c->strfixcap) {
        c->strfixcap = c->strfixcap < 8 ? 8 : c->strfixcap * 2;
        c->strfix = realloc(c->strfix, c->strfixcap * sizeof(*c->strfix));
    }
    c->strfix[c->nstrfix].text_pos = text_pos;
    c->strfix[c->nstrfix].rodata_off = rodata_off;
    c->nstrfix++;
}

static int lab_new(CG* c) { return c->nextlab++; }
static void lab_mark(CG* c, int id) {
    if (c->nlab==c->labcap){ c->labcap=c->labcap<8?8:c->labcap*2; c->labs=realloc(c->labs,c->labcap*sizeof(*c->labs)); }
    c->labs[c->nlab].id=id; c->labs[c->nlab].pos=c->out->len; c->nlab++;
}
static void fix_add(CG* c, int id) {
    /* 紧跟在跳转指令(E9 / 0F 8x)之后调用：rel32 字段位于 len-4 处 */
    if (c->nfix==c->fixcap){ c->fixcap=c->fixcap<8?8:c->fixcap*2; c->fixs=realloc(c->fixs,c->fixcap*sizeof(*c->fixs)); }
    c->fixs[c->nfix].id=id; c->fixs[c->nfix].pos=c->out->len - 4; c->nfix++;
}
/* 解析当前 out 里全部未决跳转；有未定义标签则报错 */
static int lab_resolve(CG* c) {
    for (int i = 0; i < c->nfix; i++) {
        ssize_t target = -1;
        for (int j = 0; j < c->nlab; j++)
            if (c->labs[j].id == c->fixs[i].id) { target = (ssize_t)c->labs[j].pos; break; }
        if (target < 0) { snprintf(c->err, c->errlen, "internal: unresolved label %d", c->fixs[i].id); return -1; }
        int64_t rel = target - (ssize_t)(c->fixs[i].pos + 4);
        cg_patch_u32(c->out, c->fixs[i].pos, (uint32_t)rel);
    }
    c->nlab = 0; c->nfix = 0;
    return 0;
}
static void scope_reset(CG* c) { c->nvar = 0; c->nslots = 0; c->nsc = 0; }
/* 局部作用域栈：记录进入块作用域时的 nvar 快照，出作用域时回滚。
   没有它，let 内部定义的变量会泄漏到外层（名字可见性错误）。 */
static void scope_push(CG* c) {
    if (c->nsc == c->sccap) {
        c->sccap = c->sccap < 8 ? 8 : c->sccap * 2;
        c->scopes = (int*)realloc(c->scopes, c->sccap * sizeof(int));
    }
    c->scopes[c->nsc++] = c->nvar;
}
static void scope_pop(CG* c) {
    if (c->nsc > 0) c->nvar = c->scopes[--c->nsc];
}
static bool var_find(CG* c, const char* name, int* disp) {
    for (int i=0;i<c->nvar;i++) if (strcmp(c->vars[i].name,name)==0){ *disp=c->vars[i].disp; return true; }
    return false;
}
static int var_alloc(CG* c, const char* name) {
    for (int i=0;i<c->nvar;i++) if (strcmp(c->vars[i].name,name)==0) return c->vars[i].disp;
    if (c->nslots >= 32) { snprintf(c->err,c->errlen,"too many locals (max 32 slots per function)"); return 0; }
    if (c->nvar==c->varcap){ c->varcap=c->varcap<8?8:c->varcap*2; c->vars=realloc(c->vars,c->varcap*sizeof(Local)); }
    c->nslots++;
    int disp = -8 * c->nslots;
    c->vars[c->nvar].name = cg_strdup(name); c->vars[c->nvar].disp = disp; c->nvar++;
    return disp;
}
static void rel_add(CG* c, size_t text_pos, int symidx) {
    if (c->nrel==c->relcap){ c->relcap=c->relcap<8?8:c->relcap*2; c->rels=realloc(c->rels,c->relcap*sizeof(CGRel)); }
    c->rels[c->nrel].text_pos = text_pos; c->rels[c->nrel].symidx = symidx; c->nrel++;
}
static int func_find(CG* c, const char* name) {
    for (int i=0;i<c->ft.n;i++) if (strcmp(c->ft.f[i].name,name)==0) return i;
    return -1;
}

static bool cg_expr(CG* c, Value node);
static void cg_strfix_add(CG* c, size_t text_pos, size_t rodata_off);

/* call __nuomi_syscall_trampoline —— 所有 syscall 站点统一走跳板 */
static void cg_call_trampoline(CG* c) {
    int ti = func_find(c, "__nuomi_syscall_trampoline");
    if (ti < 0) { /* 跳板还没发射（首次使用时按需补） */
        snprintf(c->err, c->errlen, "internal: syscall trampoline missing");
        return;
    }
    FuncDef* fd = &c->ft.f[ti];
    cg_call_rel32(c->out);
    rel_add(c, c->text_base + c->out->len - 4, fd->symidx);
}

/* ---- 内置函数：向文件描述符写一段字节 ----
 * 已在 rax 里算好的「字符串首地址」（Nuomi 字符串对象地址）时：
 *   真实数据在头部之后 NUOMI_STR_HEADER 字节处，长度在头部第 0 字节。
 *   write(fd, buf, len) →  rdi=fd, rsi=buf, rdx=len, rax=号
 */
static void cg_emit_write_str(CG* c, Value strAddrNode) {
    /* rax 已是字符串对象地址；先把它挪到 r10 保存 */
    cg_u8(c->out, 0x49); cg_u8(c->out, 0x89); cg_u8(c->out, 0xC2);  /* mov r10, rax */
    /* rsi = r10 + 16（数据起始） */
    cg_u8(c->out, 0x49); cg_u8(c->out, 0x8B); cg_u8(c->out, 0xD2);  /* mov rdx, r10 (REX.B 扩 rm) */
    cg_u8(c->out, 0x48); cg_u8(c->out, 0x83); cg_u8(c->out, 0xC2); cg_u8(c->out, 16); /* add rdx, 16 */
    cg_u8(c->out, 0x48); cg_u8(c->out, 0x89); cg_u8(c->out, 0xD6);  /* mov rsi, rdx
        * 目标 rsi 在 reg 字段、源 rdx 在 rm 字段，两者都 <8 → **不需要 REX.B**。
        * 曾误写 49（REX.B 会把 rm 扩成 r14），导致 rsi 取到垃圾、write 的 buf 变成 0。 */
    /* rdx = 长度（从头部第 0 字节读） */
    cg_u8(c->out, 0x49); cg_u8(c->out, 0x8B); cg_u8(c->out, 0x52); cg_u8(c->out, 0x00); /* mov rdx, [r10] */
    /* rdi = fd, rax = syscall 号 */
    cg_mov_rdi_imm(c->out, NOVUS_FD_STDOUT);
    cg_mov_rax_imm(c->out, NOVUS_SYS_WRITE);
    cg_call_trampoline(c);
}

/* 内置函数：打印整数
 * 流程：调用 __nuomi_itoa(值, BSS缓冲区) → rax=文本地址, rdx=长度 → write
 * itoa 需要 2 个参数（rdi=值, rsi=buf），用 SysV 寄存器传。
 */
static bool cg_emit_print_int(CG* c, Value argNode) {
    /* 求值参数 → rax */
    if (!cg_expr(c, argNode)) return false;
    /* 挪到 rdi 作为 itoa 的第 1 参数 */
    cg_u8(c->out, 0x48); cg_u8(c->out, 0x89); cg_u8(c->out, 0xC7);  /* mov rdi, rax */
    /* rsi = BSS 缓冲区地址（BSS_VADDR 是编译期固定虚拟地址） */
    cg_mov_rsi_imm(c->out, (int64_t)NUOMI_BSS_VADDR);
    /* call __nuomi_itoa（相对调用 + 重定位） */
    int itoaIdx = func_find(c, "__nuomi_itoa");
    if (itoaIdx < 0) { snprintf(c->err,c->errlen,"internal: itoa helper not registered"); return false; }
    {
        FuncDef* fd = &c->ft.f[itoaIdx];
        cg_call_rel32(c->out);
        rel_add(c, c->text_base + c->out->len - 4, fd->symidx);
    }
    /* itoa 返回：rax = 文本首地址（裸字节，无字符串头部），rdx = 长度。
       write(fd, buf, len) 需要 rdi=fd, rsi=buf, rdx=len —— rdx 已经是长度，rax 已经是首地址，
       只需把 buf 指针挪到 rsi（rsi = rax，无需 +16：itoa 输出的是纯文本，不是 Nuomi 字符串对象）。 */
    cg_u8(c->out, 0x48); cg_u8(c->out, 0x89); cg_u8(c->out, 0xC6);  /* mov rsi, rax */
    cg_mov_rdi_imm(c->out, NOVUS_FD_STDOUT);
    cg_mov_rax_imm(c->out, NOVUS_SYS_WRITE);
    cg_call_trampoline(c);
    return true;
}

/* 发射 call rel32 + PC32 重定位（加载期解析，天然支持前向/递归引用） */
static bool cg_call_by_index(CG* c, int fi, Value args) {
    FuncDef* fd = &c->ft.f[fi];
    int cnt = 0;
    for (Value p = args; isList(p); p = listCdr(p)) cnt++;
    if (cnt != fd->nparams) { snprintf(c->err,c->errlen,"'%s' expects %d args, got %d", fd->name, fd->nparams, cnt); return false; }
    if (cnt > 6) { snprintf(c->err,c->errlen,"more than 6 args not supported"); return false; }
    for (Value p = args; isList(p); p = listCdr(p)) {
        if (!cg_expr(c, listCar(p))) return false;
        cg_push_rax(c->out);
    }
    static const uint8_t popreg[6][2] = {
        {0x5F,0x00}, /* pop rdi */ {0x5E,0x00}, /* pop rsi */ {0x5A,0x00}, /* pop rdx */
        {0x59,0x00}, /* pop rcx */ {0x41,0x58}, /* pop r8  */ {0x41,0x59}, /* pop r9  */
    };
    for (int i = cnt; i >= 1; i--) {
        cg_put(c->out, popreg[i-1], popreg[i-1][1] ? 2 : 1);
    }
    cg_call_rel32(c->out);
    rel_add(c, c->text_base + c->out->len - 4, fd->symidx);  /* rel32 字段的最终 .text 偏移 */
    return true;
}

static bool cg_expr(CG* c, Value node) {
    if (IS_NUM(node)) { cg_mov_rax_imm(c->out, (int64_t)AS_NUM(node)); return true; }
    /* 字符串字面量（非符号）→放进 RODATA，值为其地址 */
    if (IS_OBJ(node) && AS_OBJ(node)->type == OBJ_STRING && !((ObjString*)AS_OBJ(node))->isSymbol) {
        ObjString* s = (ObjString*)AS_OBJ(node);
        size_t off = rodataAddStr(&c->rodata, s->chars, s->length);
        cg_lea_rax_rip(c->out);
        /* RIP 相对位移在发射时已知：目标 RODATA vaddr + off 减 (本指令末尾 +4)，
         * 但 text 与 rodata 何时拼进最终布局取决于后续段大小，故先记重定位、
         * 待 text 全部确定后在收尾阶段统一回填。 */
        cg_strfix_add(c, c->text_base + c->out->len - 4, off);
        return true;
    }
    if (cg_isSymbol(node)) {
        const char* nm = cg_symChars(node);
        if (strcmp(nm,"true")==0)  { cg_mov_rax_imm(c->out, 1); return true; }
        if (strcmp(nm,"false")==0) { cg_xor_eax(c->out); return true; }
        int disp;
        if (var_find(c, nm, &disp)) { cg_load_rax_local(c->out, disp); return true; }
        int64_t v;
        if (ct_find(&c->ct, nm, &v)) { cg_mov_rax_imm(c->out, v); return true; }
        if (func_find(c, nm) >= 0) { snprintf(c->err,c->errlen,"function '%s' used as value (not supported)", nm); return false; }
        snprintf(c->err,c->errlen,"undefined symbol '%s'", nm); return false;
    }
    if (!isList(node)) { snprintf(c->err,c->errlen,"unsupported expression in compile mode"); return false; }
    Value head = listCar(node);
    if (!cg_isSymbol(head)) { snprintf(c->err,c->errlen,"bad form head"); return false; }
    const char* op = cg_symChars(head);
    Value args = listCdr(node);

    /* ---- 算术 ---- */
    if (strcmp(op,"+")==0 || strcmp(op,"-")==0 || strcmp(op,"*")==0) {
        int cnt = 0; for (Value p = args; isList(p); p = listCdr(p)) cnt++;
        if (cnt == 0) { snprintf(c->err,c->errlen,"%s expects >=1 argument", op); return false; }
        bool isSub = (op[0]=='-' && op[1]==0), isMul = (op[0]=='*' && op[1]==0);
        if (!cg_expr(c, listCar(args))) return false;
        for (Value p = listCdr(args); isList(p); p = listCdr(p)) {
            cg_push_rax(c->out);
            if (!cg_expr(c, listCar(p))) return false;
            cg_pop_rdx(c->out);
            if (isSub)      { cg_neg_rax(c->out); cg_add_rax_rdx(c->out); }
            else if (isMul) cg_imul_rax_rdx(c->out);
            else            cg_add_rax_rdx(c->out);
        }
        if (isSub && cnt == 1) cg_neg_rax(c->out);
        return true;
    }
    /* ---- 比较 ---- */
    if (strcmp(op,"=")==0||strcmp(op,"<")==0||strcmp(op,">")==0||
        strcmp(op,"<=")==0||strcmp(op,">=")==0) {
        uint8_t cc;
        if (op[0]=='=' ) cc=0x94; else if (op[0]=='<'&&op[1]==0) cc=0x9C;
        else if (op[0]=='>'&&op[1]==0) cc=0x9F; else if (op[0]=='<') cc=0x9E; else cc=0x9D;
        if (!isList(args) || !isList(listCdr(args)) || isList(listCdr(listCdr(args)))) {
            snprintf(c->err,c->errlen,"%s expects exactly 2 arguments", op); return false;
        }
        if (!cg_expr(c, listCar(args))) return false;          /* 左 -> rax */
        cg_push_rax(c->out);
        if (!cg_expr(c, listCar(listCdr(args)))) return false; /* 右 -> rax */
        cg_pop_rdx(c->out);                                    /* rdx=左, rax=右 */
        cg_cmp_rdx_rax(c->out);                                /* cmp 左, 右 */
        cg_setcc_al(c->out, cc);
        cg_movzx_eax_al(c->out);
        return true;
    }
    /* ---- 内置 I/O：print / println / print-int ---- */
    if (strcmp(op,"print")==0 || strcmp(op,"println")==0) {
        bool nl = (op[5] == 'l');   /* "println" 的第 6 个字符是 'l' */
        int narg = 0; for (Value p = args; isList(p); p = listCdr(p)) narg++;
        if (narg < 1) { snprintf(c->err,c->errlen,"%s expects at least 1 argument", op); return false; }
        for (Value p = args; isList(p); p = listCdr(p)) {
            Value a = listCar(p);
            if (IS_OBJ(a) && AS_OBJ(a)->type == OBJ_STRING && !((ObjString*)AS_OBJ(a))->isSymbol) {
                /* 字符串字面量：rax = 字符串对象地址 → write */
                if (!cg_expr(c, a)) return false;
                cg_emit_write_str(c, a);
            } else {
                /* 其它值按整数打印（走 itoa） */
                if (!cg_emit_print_int(c, a)) return false;
            }
        }
        if (nl) {
            /* 打印换行：write(1, "\n", 1)。把 "\n" 放进 RODATA */
            size_t off = rodataAddStr(&c->rodata, "\n", 1);
            cg_lea_rax_rip(c->out);
            cg_strfix_add(c, c->text_base + c->out->len - 4, off);
            cg_emit_write_str(c, NIL_VAL);
        }
        cg_xor_eax(c->out);   /* 返回 nil */
        return true;
    }
    if (strcmp(op,"print-int")==0) {
        int narg = 0; for (Value p = args; isList(p); p = listCdr(p)) narg++;
        if (narg != 1) { snprintf(c->err,c->errlen,"print-int expects 1 argument"); return false; }
        if (!cg_emit_print_int(c, listCar(args))) return false;
        cg_xor_eax(c->out);
        return true;
    }
    /* ---- let / let* / letrec ---- */
    if (strcmp(op,"let")==0 || strcmp(op,"let*")==0 || strcmp(op,"letrec")==0) {
        if (!isList(args)) { snprintf(c->err,c->errlen,"%s: bad bindings", op); return false; }
        Value bindings = listCar(args);
        Value body = listCdr(args);
        bool isRec = (op[3]=='r');   /* letrec */
        bool isSeq = (op[3]=='*');   /* let*  */

        scope_push(c);
        /* letrec：先把所有名字占位（赋 0），让 init 能自/互引用 */
        if (isRec) {
            for (Value b = bindings; isList(b); b = listCdr(b)) {
                Value pair = listCar(b);
                if (!isList(pair)) { snprintf(c->err,c->errlen,"letrec: bad binding"); goto letfail; }
                Value nv = listCar(pair);
                if (!cg_isSymbol(nv)) { snprintf(c->err,c->errlen,"letrec: binding name is not a symbol"); goto letfail; }
                var_alloc(c, cg_symChars(nv));
                if (c->err[0]) goto letfail;
            }
        }
        /* let / letrec：所有 init 先在外层求值再绑定（并行语义）
           —— 但编译器里都在同一帧内，顺序发射即可满足「init 看不到本次新绑定」。
              为严格保证并行语义，let 的 init 必须在此刻绑定前求值；
              由于 var_find 只查已注册名字，且我们「先求值全部再 var_alloc」，
              并行/顺序在编译期即可区分。 */
        if (!isSeq && !isRec) {
            /* 并行 let：先发射全部 init（压栈暂存），再 var_alloc + store */
            int nb = 0;
            for (Value b = bindings; isList(b); b = listCdr(b)) nb++;
            int* disps = (int*)malloc((nb>0?nb:1) * sizeof(int));
            ObjString** names = (ObjString**)malloc((nb>0?nb:1) * sizeof(ObjString*));
            int i = 0;
            for (Value b = bindings; isList(b); b = listCdr(b), i++) {
                Value pair = listCar(b);
                if (!isList(pair) || !isList(listCdr(pair))) { free(disps); free(names); snprintf(c->err,c->errlen,"let: bad binding"); goto letfail; }
                Value nv = listCar(pair);
                if (!cg_isSymbol(nv)) { free(disps); free(names); snprintf(c->err,c->errlen,"let: binding name is not a symbol"); goto letfail; }
                names[i] = (ObjString*)AS_OBJ(nv);
                if (!cg_expr(c, listCar(listCdr(pair)))) { free(disps); free(names); goto letfail; }
                cg_push_rax(c->out);   /* 暂存 init 值 */
                disps[i] = 0;
            }
            i = 0;
            for (Value b = bindings; isList(b); b = listCdr(b), i++) {
                disps[i] = var_alloc(c, names[i]->chars);
                if (c->err[0]) { free(disps); free(names); goto letfail; }
            }
            /* 从栈顶依次弹出写入各自槽位 */
            for (int k = nb - 1; k >= 0; k--) {
                cg_pop_rdx(c->out);
                cg_store_rdx_local(c->out, disps[k]);
            }
            free(disps); free(names);
        } else {
            /* let* / letrec：顺序绑定，每个 init 立即可用前面的名字 */
            for (Value b = bindings; isList(b); b = listCdr(b)) {
                Value pair = listCar(b);
                if (!isList(pair) || !isList(listCdr(pair))) { snprintf(c->err,c->errlen,"%s: bad binding", op); goto letfail; }
                Value nv = listCar(pair);
                if (!cg_isSymbol(nv)) { snprintf(c->err,c->errlen,"%s: binding name is not a symbol", op); goto letfail; }
                int disp = var_alloc(c, cg_symChars(nv));
                if (c->err[0]) goto letfail;
                if (!cg_expr(c, listCar(listCdr(pair)))) goto letfail;
                cg_store_rax_local(c->out, disp);
            }
        }
        /* body：顺序求值，返回最后一个 */
        if (isList(body)) {
            bool any = false;
            for (Value p = body; isList(p); p = listCdr(p)) {
                if (!cg_expr(c, listCar(p))) goto letfail;
                any = true;
            }
            if (!any) cg_xor_eax(c->out);
        } else {
            cg_xor_eax(c->out);
        }
        scope_pop(c);
        return true;
    letfail:
        scope_pop(c);
        return false;
    }
    /* ---- if ---- */
    if (strcmp(op,"if")==0) {
        int narg = 0; for (Value p = args; isList(p); p = listCdr(p)) narg++;
        if (narg < 2 || narg > 3) { snprintf(c->err,c->errlen,"if expects 2 or 3 args"); return false; }
        int Lelse = lab_new(c), Lend = lab_new(c);
        if (!cg_expr(c, listCar(args))) return false;          /* cond */
        cg_test_rax_rax(c->out);
        cg_jcc_rel32(c->out, 0x84);                            /* je Lelse */
        fix_add(c, Lelse);
        if (!cg_expr(c, listCar(listCdr(args)))) return false; /* then */
        cg_jmp_rel32(c->out); fix_add(c, Lend);
        lab_mark(c, Lelse);
        if (narg == 3) { if (!cg_expr(c, listCar(listCdr(listCdr(args))))) return false; }
        else cg_xor_eax(c->out);                               /* 无 else -> 0 */
        lab_mark(c, Lend);
        return lab_resolve(c) == 0;
    }
    /* ---- while ---- */
    if (strcmp(op,"while")==0) {
        int narg = 0; for (Value p = args; isList(p); p = listCdr(p)) narg++;
        if (narg < 1) { snprintf(c->err,c->errlen,"while expects cond + body"); return false; }
        char slotname[32]; snprintf(slotname,sizeof slotname,"__while%d", c->nextlab);
        int slot = var_alloc(c, slotname);
        if (c->err[0]) return false;
        int Lstart = lab_new(c), Lend = lab_new(c);
        cg_xor_eax(c->out); cg_store_rax_local(c->out, slot);
        lab_mark(c, Lstart);
        if (!cg_expr(c, listCar(args))) return false;          /* cond */
        cg_test_rax_rax(c->out);
        cg_jcc_rel32(c->out, 0x84); fix_add(c, Lend);          /* je Lend */
        for (Value p = listCdr(args); isList(p); p = listCdr(p))
            if (!cg_expr(c, listCar(p))) return false;         /* body */
        cg_store_rax_local(c->out, slot);                      /* 记录本次结果 */
        cg_jmp_rel32(c->out); fix_add(c, Lstart);
        lab_mark(c, Lend);
        cg_load_rax_local(c->out, slot);                       /* while 值 = 最后一次 body 值 */
        return lab_resolve(c) == 0;
    }
    /* ---- begin ---- */
    if (strcmp(op,"begin")==0) {
        bool any = false;
        for (Value p = args; isList(p); p = listCdr(p)) {
            if (!cg_expr(c, listCar(p))) return false;
            any = true;
        }
        if (!any) cg_xor_eax(c->out);
        return true;
    }
    /* ---- set!（仅函数内局部/参数） ---- */
    if (strcmp(op,"set!")==0) {
        Value nm = listCar(args);
        if (!cg_isSymbol(nm)) { snprintf(c->err,c->errlen,"set!: not a symbol"); return false; }
        int disp;
        if (!var_find(c, cg_symChars(nm), &disp)) {
            snprintf(c->err,c->errlen,"set!: '%s' is not a local (top-level defines are compile-time constants)", cg_symChars(nm));
            return false;
        }
        if (!cg_expr(c, listCar(listCdr(args)))) return false;
        cg_store_rax_local(c->out, disp);
        return true;
    }
    /* ---- define（函数内局部） ---- */
    if (strcmp(op,"define")==0) {
        Value nm = listCar(args);
        if (!cg_isSymbol(nm)) { snprintf(c->err,c->errlen,"define inside function: (define name expr)"); return false; }
        int disp = var_alloc(c, cg_symChars(nm));
        if (c->err[0]) return false;
        if (!cg_expr(c, listCar(listCdr(args)))) return false;
        cg_store_rax_local(c->out, disp);
        return true;
    }
    /* ---- 函数调用 ---- */
    int fi = func_find(c, op);
    if (fi >= 0) return cg_call_by_index(c, fi, args);
    snprintf(c->err,c->errlen,"'%s' is not a function or supported form in compile mode", op);
    return false;
}

/* 编译单个函数到临时缓冲，再拼进 text */
static bool cg_compile_function(CG* c, FuncDef* fd, CGBuf* text) {
    CGBuf tmp; cg_init(&tmp);
    c->out = &tmp; scope_reset(c); c->err[0] = 0;
    cg_prologue(c->out);
    /* 参数：SysV 寄存器 -> 被调函数自己帧内的负偏移槽（不可写 [rbp+16..]，见 cg_store_reg_local） */
    int i = 0;
    for (Value p = fd->params; isList(p); p = listCdr(p), i++) {
        Value pv = listCar(p);
        if (!cg_isSymbol(pv)) { snprintf(c->err,c->errlen,"param is not a symbol"); goto done; }
        char nm[128]; snprintf(nm,sizeof nm,"%s", cg_symChars(pv));
        c->nslots++;
        int disp = -8 * c->nslots;
        if (c->nvar==c->varcap){ c->varcap=c->varcap<8?8:c->varcap*2; c->vars=realloc(c->vars,c->varcap*sizeof(Local)); }
        c->vars[c->nvar].name = cg_strdup(nm);
        c->vars[c->nvar].disp = disp;
        c->nvar++;
        cg_store_reg_local(c->out, i, disp);
    }
    for (Value f = fd->body; isList(f); f = listCdr(f)) {
        if (!cg_expr(c, listCar(f))) goto done;   /* 依次求值，值留在 rax */
    }
    cg_leave(c->out);
    cg_ret(c->out);
    if (lab_resolve(c) != 0) goto done;
    fd->offset = text->len;
    fd->size = tmp.len;
    cg_put(text, tmp.p, tmp.len);
done:
    free(tmp.p);
    c->out = NULL;
    return c->err[0] == 0;
}

/* 编译期常量折叠（顶层 (define name expr)，仅字面量/已知常量/+ - *） */
static bool fold_const(CG* c, Value node, int64_t* out) {
    if (IS_NUM(node)) { *out = (int64_t)AS_NUM(node); return true; }
    if (cg_isSymbol(node)) {
        int64_t v;
        if (!ct_find(&c->ct, cg_symChars(node), &v)) {
            snprintf(c->err,c->errlen,"undefined symbol '%s'", cg_symChars(node)); return false;
        }
        *out = v; return true;
    }
    if (isList(node)) {
        Value head = listCar(node);
        if (!cg_isSymbol(head)) { snprintf(c->err,c->errlen,"bad form head"); return false; }
        const char* op = cg_symChars(head);
        Value args = listCdr(node);
        int cnt = 0; for (Value p = args; isList(p); p = listCdr(p)) cnt++;
        if (cnt == 0) { snprintf(c->err,c->errlen,"%s expects >=1 argument", op); return false; }
        int64_t acc = 0; bool first = true;
        for (Value p = args; isList(p); p = listCdr(p)) {
            int64_t v; if (!fold_const(c, listCar(p), &v)) return false;
            if (op[0]=='+'&&op[1]==0)      acc = first ? v : acc + v;
            else if (op[0]=='-'&&op[1]==0) acc = first ? v : acc - v;
            else if (op[0]=='*'&&op[1]==0) acc = first ? v : acc * v;
            else { snprintf(c->err,c->errlen,"operator '%s' not supported in compile mode", op); return false; }
            first = false;
        }
        if (op[0]=='-'&&op[1]==0 && cnt==1) acc = -acc;
        *out = acc; return true;
    }
    snprintf(c->err,c->errlen,"unsupported expression in compile mode");
    return false;
}

int nuomi_compile_to_nuc(const char* src, uint8_t** out, size_t* out_size, char* err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    Reader r;
    r.src = src; r.len = strlen(src); r.pos = 0; r.error = false; r.done = false; r.errmsg = NULL;

    CG c; memset(&c, 0, sizeof c);
    c.errlen = sizeof c.err;

    Value forms[256]; int nf = 0;
    while (!r.done && !r.error) {
        Value f = readForm(&r);
        if (r.error) { snprintf(err, errlen, "parse error: %s", r.errmsg ? r.errmsg : "?"); return -1; }
        if (IS_NIL(f) && r.done) break;
        if (nf < 256) forms[nf++] = f;
    }

    /* ---- 第一遍：分类顶层表单 ---- */
    int entryIdx = -1;
    for (int i = 0; i < nf; i++) {
        Value f = forms[i];
        if (isList(f) && cg_isSymbol(listCar(f)) && strcmp(cg_symChars(listCar(f)), "define") == 0) {
            Value rest = listCdr(f);
            if (isList(rest) && isList(listCar(rest))) {
                /* 函数定义：(define (name params...) body...) */
                Value head = listCar(rest);
                if (!cg_isSymbol(listCar(head))) { snprintf(err,errlen,"define: bad function name"); return -1; }
                if (c.ft.n == c.ft.cap){ c.ft.cap=c.ft.cap<4?4:c.ft.cap*2; c.ft.f=realloc(c.ft.f,c.ft.cap*sizeof(FuncDef)); }
                FuncDef* fd = &c.ft.f[c.ft.n++];
                memset(fd, 0, sizeof *fd);
                fd->name = cg_strdup(cg_symChars(listCar(head)));
                fd->params = listCdr(head);
                fd->body = listCdr(rest);
                for (Value p = fd->params; isList(p); p = listCdr(p)) fd->nparams++;
                continue;
            }
            if (isList(rest) && cg_isSymbol(listCar(rest))) {
                /* 常量定义（编译期折叠；顶层绑定不可 set!） */
                const char* nm = cg_symChars(listCar(rest));
                int64_t v;
                if (!fold_const(&c, listCar(listCdr(rest)), &v)) { snprintf(err,errlen,"%s", c.err); return -1; }
                ct_add(&c.ct, nm, v);
                continue;
            }
            snprintf(err,errlen,"define: (define name expr) or (define (name params) body)");
            return -1;
        }
        entryIdx = i;
    }
    if (entryIdx < 0 && c.ft.n == 0) { snprintf(err,errlen,"nothing to compile"); return -1; }

    /* ---- 注册函数符号（值占位 0，发射后回填） ---- */
    CGBuf text; cg_init(&text);

    /* ---- 检测是否需要内置运行时辅助（必须在发射用户函数之前决定） ---- */
    bool needItoa = false, needSyscall = false;
    for (int i = 0; i < nf; i++) {
        Value f = forms[i];
        if (isList(f) && cg_isSymbol(listCar(f))) {
            const char* h = cg_symChars(listCar(f));
            if (strcmp(h,"print")==0 || strcmp(h,"println")==0 || strcmp(h,"print-int")==0) {
                needItoa = true; needSyscall = true;
            }
        }
    }

    /* ---- 内置辅助插到用户函数之前 ----
     * 用户函数会 call 它们，所以它们必须先被注册（func_find 能查到）、
     * 且先被发射（重定位指向正确的 .text 偏移）。
     * 做法：把用户函数整体后移，把内置辅助插到数组前部。 */
    int nUser = c.ft.n;
    int nBuilt = (needSyscall?1:0) + (needItoa?1:0);
    if (nBuilt > 0) {
        c.ft.cap += nBuilt;
        c.ft.f = (FuncDef*)realloc(c.ft.f, c.ft.cap*sizeof(FuncDef));
        memmove(c.ft.f + nBuilt, c.ft.f, nUser*sizeof(FuncDef));
        c.ft.n = nBuilt + nUser;
        memset(c.ft.f, 0, nBuilt*sizeof(FuncDef));
        int k = 0;
        if (needSyscall) {
            FuncDef* fd = &c.ft.f[k++];
            fd->name = cg_strdup("__nuomi_syscall_trampoline");
            fd->nparams = 0;
        }
        if (needItoa) {
            FuncDef* fd = &c.ft.f[k++];
            fd->name = cg_strdup("__nuomi_itoa");
            fd->nparams = 2;
        }
    }
    for (int i = 0; i < c.ft.n; i++) c.ft.f[i].symidx = i;   /* 符号表下标 = 注册顺序 */

    /* ---- 第二遍：发射每个函数 ---- */
    for (int i = 0; i < c.ft.n; i++) {
        FuncDef* fd = &c.ft.f[i];
        if (fd->name && strcmp(fd->name, "__nuomi_syscall_trampoline") == 0) {
            /* 跳板：syscall ; ret ；后 13 字节留白，供宿主原地替换成宿主桩 */
            fd->offset = text.len;
            cg_u8(&text, 0x0F);
            cg_u8(&text, 0x05);
            cg_u8(&text, 0xC3);
            for (int k = 3; k < NUOMI_SYSCALL_TRAMP_SIZE; k++) cg_u8(&text, 0x90);
            fd->size = NUOMI_SYSCALL_TRAMP_SIZE;
            continue;
        }
        if (fd->name && strcmp(fd->name, "__nuomi_itoa") == 0) {
            /* itoa：机器码固定，直接贴进 .text */
            fd->offset = text.len;
            cg_put(&text, NUOMI_ITOA_CODE, sizeof NUOMI_ITOA_CODE);
            fd->size = sizeof NUOMI_ITOA_CODE;
            continue;
        }
        c.text_base = text.len;
        if (!cg_compile_function(&c, &c.ft.f[i], &text)) { snprintf(err,errlen,"%s", c.err); goto fail; }
    }

    /* ---- 入口（自带栈帧：入口里的 define/while 局部槽才安全） ----
     * 入口会依次执行**所有**非函数定义的顶层表达式
     * （早前只执行最后一个，导致 (println ...) 多行时只输出一行）。 */
    size_t entry_off = text.len;
    c.text_base = 0;              /* 入口直接写进 text（out 即最终位置），故 base=0 */
    c.out = &text; scope_reset(&c); c.err[0] = 0;
    cg_prologue(&text);
    {
        int nTop = 0;
        for (int i = 0; i < nf; i++) {
            Value f = forms[i];
            if (isList(f) && cg_isSymbol(listCar(f))
                && strcmp(cg_symChars(listCar(f)), "define") == 0) continue;  /* 定义已单独处理 */
            if (!cg_expr(&c, f)) { snprintf(err,errlen,"%s", c.err); goto fail; }
            nTop++;
        }
        if (nTop == 0) {
            /* 没有顶层表达式 → 调用 main 函数 */
            int mi = -1;
            for (int i = 0; i < c.ft.n; i++) if (strcmp(c.ft.f[i].name,"main")==0) { mi = i; break; }
            if (mi < 0) { snprintf(err,errlen,"no entry expression and no 'main' function"); goto fail; }
            if (!cg_call_by_index(&c, mi, NIL_VAL)) { snprintf(err,errlen,"%s", c.err); goto fail; }
        }
    }
    if (lab_resolve(&c) != 0) { snprintf(err,errlen,"%s", c.err); goto fail; }
    cg_leave(&text);
    cg_ret(&text);
    c.out = NULL;

    /* ---- 组装 NUC ---- */
    const uint64_t TEXT_VADDR = 0x400000;
    const uint64_t DATA_VADDR = 0x500000;   /* RODATA紧随 TEXT 之后的固定虚拟地址 */
    /* lea rax,[rip+d] 的 d 依赖两个段的最终相对位置，这里回填：
     * rip 在执行时= 指令所在 TEXT 地址 + 7；目标 = RODATA_vaddr + 段内偏移。
     * d = (DATA_VADDR + rodata_off) - (TEXT_VADDR + text_pos + 4)
     * （text_pos 指向 disp32 字段起点，+4 是 disp32 末尾即 lea 指令末尾）。 */
    if (c.nstrfix > 0) {
        for (int i = 0; i < c.nstrfix; i++) {
            size_t field = c.strfix[i].text_pos;
            if (field + 4 > text.len) { snprintf(err, errlen, "internal: lea fixup out of range"); goto fail; }
            int64_t rip_next = (int64_t)(TEXT_VADDR + field + 4);
            int64_t target = (int64_t)(DATA_VADDR + c.strfix[i].rodata_off);
            cg_patch_u32(&text, field, (uint32_t)(int32_t)(target - rip_next));
        }
    }

    NucBuilder* nb = nuc_builder_new();
    nuc_builder_set_flags(nb, NUC_FLAG_64BIT | NUC_FLAG_STATIC);
    nuc_builder_set_entry(nb, (uint32_t)entry_off);   /* §9.1: entry_point = .text 段内偏移 */
    nuc_builder_set_stack(nb, 0x800000);
    /* 符号：所有函数（FUNC/LOCAL，shndx=0 即 .text，value=段内偏移） */
    for (int i = 0; i < c.ft.n; i++)
        nuc_builder_add_symbol(nb, c.ft.f[i].name, NUC_SYM_FUNC, NUC_BIND_LOCAL, 0,
                               c.ft.f[i].offset, c.ft.f[i].size);
    nuc_builder_add_segment(nb, NUC_SEG_TEXT, TEXT_VADDR, text.len,
                            NUC_VM_READ | NUC_VM_EXEC | NUC_VM_USER, text.p, (uint32_t)text.len);
    /* RODATA：字符串字面量池（只读、用户可读；绝不可执行/可写） */
    if (c.rodata.buf.len > 0)
        nuc_builder_add_segment(nb, NUC_SEG_RODATA, DATA_VADDR, c.rodata.buf.len,
                                NUC_VM_READ | NUC_VM_USER, c.rodata.buf.p, (uint32_t)c.rodata.buf.len);
    /* BSS：可写缓冲区（itoa 用），不占文件空间，加载时由内核清零 */
    if (needItoa)
        nuc_builder_add_segment(nb, NUC_SEG_BSS, NUOMI_BSS_VADDR, NUOMI_ITOA_BUFSZ,
                                NUC_VM_READ | NUC_VM_WRITE | NUC_VM_USER, NULL, 0);
    /* 重定位：调用点 VA = text.vaddr + rel32 偏移（§7.1/§7.2） */
    for (int i = 0; i < c.nrel; i++)
        nuc_builder_add_reloc(nb, TEXT_VADDR + (uint64_t)c.rels[i].text_pos, R_X86_64_PC32,
                              (uint32_t)c.rels[i].symidx);

    size_t n; uint8_t* blob = nuc_builder_build(nb, &n, err, errlen);
    nuc_builder_free(nb);
    if (!blob) goto fail;
    *out = blob; *out_size = n;

    free(text.p);
    free(c.rodata.buf.p);
    for (int i = 0; i < c.ct.n; i++) free(c.ct.c[i].name);
    free(c.ct.c);
    for (int i = 0; i < c.ft.n; i++) free(c.ft.f[i].name);
    free(c.ft.f);
    free(c.vars); free(c.labs); free(c.fixs); free(c.rels); free(c.scopes); free(c.strfix);
    return 0;

fail:
    free(text.p);
    free(c.rodata.buf.p);
    for (int i = 0; i < c.ct.n; i++) free(c.ct.c[i].name);
    free(c.ct.c);
    for (int i = 0; i < c.ft.n; i++) free(c.ft.f[i].name);
    free(c.ft.f);
    free(c.vars); free(c.labs); free(c.fixs); free(c.rels); free(c.scopes); free(c.strfix);
    return -1;
}
