// SPDX-License-Identifier: GPL-2.0
/*
 * nucutil.c — NUC 文件工具
 *   dump <file.nuc>       人类可读 dump
 *   validate <file.nuc>   按 §10 ABI 规则校验
 *   emit-demo <out.nuc>    生成一个带 PC32 重定位、可在真机跑通的最小示例
 *   run <file.nuc>         （仅 Windows）映射 TEXT 段并执行入口，验证加载流程
 */
#if !defined(_WIN32)
#define _DEFAULT_SOURCE 1  /* 让 POSIX 头文件暴露 sysconf/_SC_PAGESIZE/MAP_ANONYMOUS（Linux run 用） */
#endif
#include "nucfmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(sz ? (size_t)sz : 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f); *out_len = (size_t)sz; return b;
}
static int write_file(const char* path, const uint8_t* b, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(b, 1, n, f);
    fclose(f);
    return 0;
}

static int emit_demo(const char* out_path) {
    NucBuilder* b = nuc_builder_new();
    nuc_builder_set_flags(b, NUC_FLAG_64BIT | NUC_FLAG_STATIC);
    nuc_builder_set_entry(b, 0);
    nuc_builder_set_stack(b, 0x800000);
    /* .text：offset0 call main(PC32 重定位)；offset5 mov eax,0x2A；offset10 ret */
    uint8_t text[] = { 0xE8, 0, 0, 0, 0, 0xB8, 0x2A, 0, 0, 0, 0xC3 };
    nuc_builder_add_segment(b, NUC_SEG_TEXT, 0x400000, sizeof(text),
                            NUC_VM_READ | NUC_VM_EXEC | NUC_VM_USER, text, (uint32_t)sizeof(text));
    nuc_builder_add_symbol(b, "main", NUC_SYM_FUNC, NUC_BIND_LOCAL, 0, 5, 6);
    nuc_builder_add_reloc(b, 0x400000 + 1, R_X86_64_PC32, 0);

    size_t n; char err[128];
    uint8_t* blob = nuc_builder_build(b, &n, err, sizeof err);
    nuc_builder_free(b);
    if (!blob) { fprintf(stderr, "build failed: %s\n", err); return 1; }
    if (write_file(out_path, blob, n) != 0) { fprintf(stderr, "write failed\n"); free(blob); return 1; }
    printf("wrote %s (%zu bytes)\n", out_path, n);
    free(blob);
    return 0;
}

/* ===== 跨平台共享的 .nuc 真机执行辅助 ===== */
static NucSegment* find_text_seg(NucFile* f) {
    for (uint16_t i = 0; i < f->seg_count; i++)
        if (f->segments[i].type == NUC_SEG_TEXT) return &f->segments[i];
    return NULL;
}
static NucSegment* find_rodata_seg(NucFile* f) {
    for (uint16_t i = 0; i < f->seg_count; i++)
        if (f->segments[i].type == NUC_SEG_RODATA) return &f->segments[i];
    return NULL;
}
static NucSegment* find_bss_seg(NucFile* f) {
    for (uint16_t i = 0; i < f->seg_count; i++)
        if (f->segments[i].type == NUC_SEG_BSS) return &f->segments[i];
    return NULL;
}
/* 宿主执行时给 RODATA 找的映射位置：TEXT 之后紧跟，保证 lea rax,[rip+d] 能落到数据上。
 * 必须与编译期假定的「RODATA_vaddr - TEXT_vaddr」差值一致，否则 RIP 相对寻址会指错。
 * 编译期用 DATA_VADDR(0x500000) - TEXT_VADDR(0x400000) = 0x100000，
 * 故这里取 TEXT 实际基址 + 0x100000（并确保有足够空间）。 */
#define RODATA_VA_DELTA 0x100000u
/* BSS 紧随 RODATA：编译期 NUOMI_BSS_VADDR(0x600000) - TEXT_VADDR(0x400000) = 0x200000 */
#define BSS_VA_DELTA    0x200000u

static void apply_relocs(NucFile* f, NucSegment* text, uint8_t* base) {
    /* 宿主实际映射地址与规范里的逻辑 vaddr 未必相等（VirtualAlloc/mmap 给的是随机地址）。
     * 重定位算的是「字节偏移差」，与绝对地址无关，因此先把符号值换算成段内偏移，
     * 再把调用点也换算成段内偏移，二者相减即得正确的 rel32。
     *   （早前实现直接拿规范 vaddr（0x400000+）参与运算，在真实映射地址不同的机器上
     *     会算出错误的跳��，是 nucutil run 偶发段错误的历史根因。） */
    for (uint32_t i = 0; i < f->rel_count; i++) {
        NucReloc* r = &f->relocs[i];
        uint64_t sym_off = 0;                 /* 符号在 TEXT 段内的偏移 */
        if (r->sym_idx < f->symtab_count) {
            NucSymbol* s = &f->symbols[r->sym_idx];
            sym_off = s->value + f->segments[s->shndx].vaddr - text->vaddr;
        }
        uint64_t site_off = r->r_offset - text->vaddr;   /* 调用点在 TEXT 段内的偏移 */
        uint8_t* p = base + site_off;
        if (r->r_type == R_X86_64_PC32) {
            /* §7.2: *((int32_t*)site) = S + A - P - 4（此处 S、P 均为段内偏移） */
            *(int32_t*)p = (int32_t)((int64_t)sym_off - (int64_t)site_off - 4);
        } else if (r->r_type == R_X86_64_32) {
            /* §7.2: *((uint32_t*)site) = S（加载器可再叠加实际加载基址） */
            *(uint32_t*)p = (uint32_t)sym_off;
        } else {
            fprintf(stderr, "reloc type %u unsupported by host runner (skipped)\n", r->r_type);
        }
    }
}
/* 拷贝 TEXT 到 base、应用重定位并调用入口；返回入口返回值（64 位，勿用 int 再接！） */
/* ====================== 宿主侧 syscall 模拟 ======================
 *
 * 编译产物里所有 syscall 都 `call` 到 .text 段中的**统一跳板**
 * __nuomi_syscall_trampoline（内容：int 0x80 ; ret）。
 * 真机上：跳板 → int 0x80 → novus 内核 → 返回。
 *
 * 宿主上没有内核，故把跳板那 32 字节**原地替换**成一段调用宿主桩的序列：
 *     mov r10, rdx / mov r11, rsi          （暂存 arg3 / arg2）
 *     mov rcx, rax / mov rdx, rdi / mov r8, r11 / mov r9, r10   （SysV → Win x64 搬运）
 *     mov rax, <宿主桩地址> / call rax / ret
 * 共 31 字节，落在跳板的 32 字节预留区内。
 *
 * 这样「编译 → syscall 发射 → 号表 → 字符串布局 → itoa」整条链路都能在本机端到端验证。
 *
 * 约定见 include/novus_syscall.h：rax=号, rdi/rsi/rdx=参数, 返回在 rax。
 */
#include "novus_syscall.h"

#if defined(_WIN32) && !defined(NO_WIN_RUN)
#include <windows.h>

static int g_syscall_exit_code = 0;
static int g_syscall_count = 0;

/* 宿主桩：Windows x64 约定（rcx=号, rdx=arg1, r8=arg2, r9=arg3）→ 返回值在 rax */
static unsigned long long __stdcall host_syscall_stub(unsigned long long nr,
                                                      unsigned long long a0,
                                                      unsigned long long a1,
                                                      unsigned long long a2) {
    g_syscall_count++;
    switch (nr) {
        case NOVUS_SYS_WRITE: {
            const uint8_t* buf = (const uint8_t*)(uintptr_t)a1;
            size_t len = (size_t)a2;
            FILE* out = (a0 == NOVUS_FD_STDERR) ? stderr : stdout;
            if (buf && len) fwrite(buf, 1, len, out);
            fflush(out);
            return len;                    /* 返回写入字节数 */
        }
        case NOVUS_SYS_EXIT:
            g_syscall_exit_code = (int)a0;
            fflush(stdout);
            ExitProcess((UINT)(a0 & 0xFF));
            return 0;
        default:
            return (unsigned long long)-38;   /* -ENOSYS */
    }
}

/* 在符号表里找 __nuomi_syscall_trampoline 的 .text 内偏移；找不到返回 (size_t)-1 */
static size_t find_trampoline_off(NucFile* f) {
    for (uint16_t i = 0; i < f->symtab_count; i++) {
        const char* nm = f->strtab + f->symbols[i].name_off;
        if (nm && strcmp(nm, "__nuomi_syscall_trampoline") == 0)
            return (size_t)f->symbols[i].value;   /* FUNC 符号的 value = 段内偏移 */
    }
    return (size_t)-1;
}

/* 把跳板（32 字节）替换成「搬运寄存器 + call 宿主桩 + ret」 */
static int patch_syscall_trampoline(uint8_t* text, size_t size, size_t tramp_off) {
    if (tramp_off + 32 > size) return 0;
    uint8_t* t = text + tramp_off;
    int i = 0;
    t[i++]=0x49; t[i++]=0x89; t[i++]=0xD2;   /* mov r10, rdx   (arg3) */
    t[i++]=0x49; t[i++]=0x89; t[i++]=0xF3;   /* mov r11, rsi   (arg2) */
    t[i++]=0x48; t[i++]=0x89; t[i++]=0xC1;   /* mov rcx, rax   (号) */
    t[i++]=0x48; t[i++]=0x89; t[i++]=0xFA;   /* mov rdx, rdi   (arg1) */
    t[i++]=0x4D; t[i++]=0x89; t[i++]=0xD8;   /* mov r8, r11    (arg2) */
    t[i++]=0x4D; t[i++]=0x89; t[i++]=0xCA;   /* mov r9, r10    (arg3) */
    t[i++]=0x48; t[i++]=0xB8;                 /* mov rax, imm64  (桩地址) */
    uint64_t fn = (uint64_t)(uintptr_t)host_syscall_stub;
    for (int k = 0; k < 8; k++) t[i++] = (uint8_t)(fn >> (8*k));
    t[i++]=0xFF; t[i++]=0xD0;                 /* call rax */
    t[i++]=0xC3;                              /* ret */
    return 1;
}
#else
static int g_syscall_exit_code = 0;
static int g_syscall_count = 0;
static int patch_syscall_trampoline(uint8_t* text, size_t size, size_t tramp_off) { return 0; }
#endif

static int64_t exec_text(NucFile* f, NucSegment* text, uint8_t* base) {
    memcpy(base, f->raw + text->file_offset, text->size);
    apply_relocs(f, text, base);
    /* 该产物含系统调用（print/println/print-int）：
       真机上由 novus 内核的 int 0x80 处理，宿主无内核，无法执行。
       明确告知而不是让程序崩在 int 0x80 上。 */
    if (find_trampoline_off(f) != (size_t)-1) {
        fprintf(stderr, "[run] 该产物含系统调用（print/println/print-int），"
                        "需在 novus 内核上运行；宿主仅能执行纯计算部分。\n");
        fprintf(stderr, "[run] 可用 nucrun.py 在本机做等价验证（模拟内核）。\n");
        VirtualFree(base, 0, MEM_RELEASE);
        nuc_file_free(f);
        return 1;
    }
    uint8_t* entry = base + f->entry_point;
    int64_t (*fn)(void) = (int64_t (*)(void))entry;
    int64_t rc = fn();
    printf("entry returned: %lld (0x%llX)\n", (long long)rc, (unsigned long long)rc);
    return rc;
}
/* 打开+解析+校验+定位 TEXT 段；失败返回 NULL；成功置 *out_text */
static NucFile* run_load(const char* path, NucSegment** out_text) {
    size_t len; uint8_t* raw = read_file(path, &len); if (!raw) return NULL;
    NucFile* f = nuc_parse(raw, len); free(raw);
    if (!f) { fprintf(stderr, "parse failed (bad magic / too small)\n"); return NULL; }
    NucValidate v = nuc_validate(f);
    if (v.code != NOVUS_OK) { fprintf(stderr, "validate: %s\n", v.msg); nuc_file_free(f); return NULL; }
    NucSegment* text = find_text_seg(f);
    if (!text) { fprintf(stderr, "no TEXT segment\n"); nuc_file_free(f); return NULL; }
    *out_text = text;
    return f;
}

#if defined(_WIN32) && !defined(NO_WIN_RUN)
#include <windows.h>
static int run_nuc(const char* path) {
    setbuf(stderr, NULL);   /* 崩溃时也要能看到诊断 */
    NucSegment* text; NucFile* f = run_load(path, &text);
    if (!f) return 1;
    /* 一次性映射 TEXT 及其后的 RODATA/BSS 空隙。
     * 各段的虚拟地址差必须与编译期假定的完全一致，否则 lea rax,[rip+d] 会指错：
     *   TEXT   @ 0x400000  → base + 0
     *   RODATA @ 0x500000  → base + RODATA_VA_DELTA
     *   BSS    @ 0x600000  → base + BSS_VA_DELTA
     */
    size_t span = (size_t)BSS_VA_DELTA + 0x1000;
    if ((size_t)text->size > span) span = text->size;
    uint8_t* base = (uint8_t*)VirtualAlloc(NULL, span,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!base) { fprintf(stderr, "VirtualAlloc failed\n"); nuc_file_free(f); return 1; }
    /* 把 RODATA 拷到 base + RODATA_VA_DELTA */
    NucSegment* ro = find_rodata_seg(f);
    if (ro && ro->file_offset)
        memcpy(base + RODATA_VA_DELTA, f->raw + ro->file_offset, ro->size);
    /* BSS：内核会清零，宿主也清零 */
    NucSegment* bs = find_bss_seg(f);
    if (bs) memset(base + BSS_VA_DELTA, 0, bs->size);
    exec_text(f, text, base);
    VirtualFree(base, 0, MEM_RELEASE);
    nuc_file_free(f);
    return 0;
}
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON  /* BSD/macOS 用 MAP_ANON */
#endif
static int run_nuc(const char* path) {
    setbuf(stderr, NULL);   /* 崩溃时也要能看到诊断 */
    NucSegment* text; NucFile* f = run_load(path, &text);
    if (!f) return 1;
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    size_t asize = ((size_t)text->size + (size_t)pg - 1u) & ~((size_t)pg - 1u); /* 页对齐 */
    void* m = mmap(NULL, asize, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { fprintf(stderr, "mmap(RWX) failed (需要可执行内存)\n"); nuc_file_free(f); return 1; }
    exec_text(f, text, (uint8_t*)m);
    munmap(m, asize);
    nuc_file_free(f);
    return 0;
}
#else
static int run_nuc(const char* path) {
    (void)path;
    fprintf(stderr, "run: 当前平台暂不支持真机执行（支持 Windows / Linux / macOS）\n");
    return 1;
}
#endif

/* 作为 nuomi.exe 的子命令入口：nuomi nuc <dump|validate|emit-demo|run> <file>
 * 调用约定：main.c 传 (argc-1, argv+1)，故 argv[0] 为占位 "nuc"，
 * 真正的子命令在 argv[1]、文件在 argv[2]，必须 argc>=3 才有文件参数。 */
int nuomi_nuc_main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: nuomi nuc <dump|validate|emit-demo|run> <file>\n");
        return 1;
    }
    if (strcmp(argv[1], "dump") == 0) {
        size_t len; uint8_t* raw = read_file(argv[2], &len);
        if (!raw) return 1;
        NucFile* f = nuc_parse(raw, len); free(raw);
        if (!f) { fprintf(stderr, "parse failed (bad magic / too small)\n"); return 1; }
        nuc_dump(f, stdout);
        nuc_file_free(f);
        return 0;
    }
    if (strcmp(argv[1], "validate") == 0) {
        size_t len; uint8_t* raw = read_file(argv[2], &len);
        if (!raw) return 1;
        NucFile* f = nuc_parse(raw, len); free(raw);
        if (!f) { fprintf(stderr, "parse failed (bad magic / too small)\n"); return 1; }
        NucValidate v = nuc_validate(f);
        printf("[%d] %s\n", v.code, v.msg);
        nuc_file_free(f);
        return v.code == NOVUS_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "emit-demo") == 0) return emit_demo(argv[2]);
    if (strcmp(argv[1], "run") == 0) return run_nuc(argv[2]);
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 1;
}
