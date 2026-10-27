// SPDX-License-Identifier: GPL-2.0
/*
 * nucfmt.c — NUC 格式构建 / 解析 / 校验 / dump
 * 严格遵循《NUC 可执行二进制格式规范 V1.0》。
 */
#include "nucfmt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ====================== 动态字节缓冲 ====================== */
typedef struct { uint8_t* p; size_t len, cap; } Buf;
static void buf_init(Buf* b) { b->p = NULL; b->len = 0; b->cap = 0; }
static void buf_put(Buf* b, const void* d, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) > 64 ? (b->len + n) * 2 : 64;
        b->p = (uint8_t*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void put8(Buf* b, uint8_t v) { buf_put(b, &v, 1); }
static void put16(Buf* b, uint16_t v) { uint8_t t[2] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff) }; buf_put(b, t, 2); }
static void put32(Buf* b, uint32_t v) { uint8_t t[4] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff), (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff) }; buf_put(b, t, 4); }
static void put64(Buf* b, uint64_t v) { uint8_t t[8]; for (int i = 0; i < 8; i++) t[i] = (uint8_t)((v >> (8 * i)) & 0xff); buf_put(b, t, 8); }

static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }
static uint64_t align4k(uint64_t x) { return (x + 0xFFFu) & ~0xFFFull; }

/* ====================== 构建器 ====================== */
typedef struct {
    char* name;
    uint8_t type;
    uint8_t bind;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} SymTmp;

struct NucBuilder {
    NucSegment* segs; int seg_count, seg_cap; uint8_t** seg_data; uint32_t* seg_datalen;
    SymTmp* syms;
    int sym_count, sym_cap;
    NucReloc* rels; int rel_count, rel_cap;
    uint16_t flags;
    uint32_t entry;
    uint32_t stack;
};

NucBuilder* nuc_builder_new(void) {
    NucBuilder* b = (NucBuilder*)calloc(1, sizeof(NucBuilder));
    b->flags = (uint16_t)(NUC_FLAG_64BIT | NUC_FLAG_STATIC);
    b->stack = 0x800000u;
    return b;
}

static int seg_ensure(NucBuilder* b) {
    if (b->seg_count == b->seg_cap) {
        b->seg_cap = b->seg_cap < 4 ? 4 : b->seg_cap * 2;
        b->segs = (NucSegment*)realloc(b->segs, b->seg_cap * sizeof(NucSegment));
        b->seg_data = (uint8_t**)realloc(b->seg_data, b->seg_cap * sizeof(uint8_t*));
        b->seg_datalen = (uint32_t*)realloc(b->seg_datalen, b->seg_cap * sizeof(uint32_t));
    }
    return b->seg_count++;
}

int nuc_builder_add_segment(NucBuilder* b, uint32_t type, uint64_t vaddr, uint64_t memsize,
                            uint32_t flags, const uint8_t* data, uint32_t datalen) {
    if (b->seg_count >= 16) return -1;
    int i = seg_ensure(b);
    b->segs[i].type = type;
    b->segs[i].vaddr = vaddr;
    b->segs[i].size = memsize;
    b->segs[i].flags = flags;
    b->segs[i].reserved = 0;
    if (type == NUC_SEG_BSS || data == NULL) {
        b->segs[i].file_offset = 0;
        b->seg_data[i] = NULL;
        b->seg_datalen[i] = 0;
    } else {
        b->seg_data[i] = (uint8_t*)malloc(datalen);
        memcpy(b->seg_data[i], data, datalen);
        b->seg_datalen[i] = datalen;
    }
    return 0;
}

int nuc_builder_add_symbol(NucBuilder* b, const char* name, uint8_t type, uint8_t bind,
                           uint16_t shndx, uint64_t value, uint64_t size) {
    if (b->sym_count == b->sym_cap) {
        b->sym_cap = b->sym_cap < 4 ? 4 : b->sym_cap * 2;
        b->syms = (SymTmp*)realloc(b->syms, b->sym_cap * sizeof(SymTmp));
    }
    int i = b->sym_count++;
    b->syms[i].name = (char*)malloc(strlen(name) + 1);
    strcpy(b->syms[i].name, name);
    b->syms[i].type = type;
    b->syms[i].bind = bind;
    b->syms[i].shndx = shndx;
    b->syms[i].value = value;
    b->syms[i].size = size;
    return 0;
}

int nuc_builder_add_reloc(NucBuilder* b, uint64_t r_offset, uint32_t r_type, uint32_t sym_idx) {
    if (b->rel_count == b->rel_cap) {
        b->rel_cap = b->rel_cap < 4 ? 4 : b->rel_cap * 2;
        b->rels = (NucReloc*)realloc(b->rels, b->rel_cap * sizeof(NucReloc));
    }
    int i = b->rel_count++;
    b->rels[i].r_offset = r_offset;
    b->rels[i].r_type = r_type;
    b->rels[i].sym_idx = sym_idx;
    return 0;
}

void nuc_builder_set_entry(NucBuilder* b, uint32_t entry_point) { b->entry = entry_point; }
void nuc_builder_set_flags(NucBuilder* b, uint16_t flags) { b->flags = flags; }
void nuc_builder_set_stack(NucBuilder* b, uint32_t stack_size) { b->stack = stack_size; }

/* errbuf is where a failure is described. It is not decoration: the only
 * ways this can fail are a bad argument and a failed allocation, and a
 * caller that gets NULL back needs to know which it was. */
static void nuc_build_fail(char* errbuf, size_t errlen, const char* why) {
    if (errbuf != NULL && errlen != 0) snprintf(errbuf, errlen, "%s", why);
}

uint8_t* nuc_builder_build(NucBuilder* b, size_t* out_size, char* errbuf, size_t errlen) {
    if (b == NULL || out_size == NULL) {
        nuc_build_fail(errbuf, errlen, "nuc_builder_build: invalid argument");
        return NULL;
    }
    /* ---- 字符串表：首字节 \0，符号名去重 ---- */
    Buf strtab; buf_init(&strtab);
    put8(&strtab, 0);
    uint32_t* symoff = (uint32_t*)malloc(sizeof(uint32_t) * (b->sym_count ? b->sym_count : 1));
    if (symoff == NULL) {
        nuc_build_fail(errbuf, errlen, "out of memory building the string table");
        free(strtab.p);
        return NULL;
    }
    for (int i = 0; i < b->sym_count; i++) {
        uint32_t off = 0; bool found = false;
        size_t p = 1; /* 跳过首字节 \0 */
        while (p < strtab.len) {
            if (strcmp((char*)strtab.p + p, b->syms[i].name) == 0) { off = (uint32_t)p; found = true; break; }
            p += strlen((char*)strtab.p + p) + 1;
        }
        if (!found) { off = (uint32_t)strtab.len; buf_put(&strtab, b->syms[i].name, strlen(b->syms[i].name) + 1); }
        symoff[i] = off;
    }

    /* ---- 布局偏移 ---- */
    uint32_t segTbl = (uint32_t)b->seg_count * 32;
    uint32_t symTbl = (uint32_t)b->sym_count * 24;
    uint32_t relTbl = (uint32_t)b->rel_count * 16;
    uint32_t meta = NUC_HDR_SIZE + segTbl + symTbl + relTbl;
    uint32_t rel_offset = NUC_HDR_SIZE + segTbl + symTbl;
    uint32_t strtab_offset = align4(meta);
    uint64_t data_start = align4k((uint64_t)strtab_offset + strtab.len);

    /* 段数据文件偏移 */
    uint64_t* segfile = (uint64_t*)malloc(sizeof(uint64_t) * (b->seg_count ? b->seg_count : 1));
    if (segfile == NULL) {
        nuc_build_fail(errbuf, errlen, "out of memory laying out the segments");
        free(symoff); free(strtab.p);
        return NULL;
    }
    uint64_t cur = data_start;
    for (int i = 0; i < b->seg_count; i++) {
        if (b->segs[i].type == NUC_SEG_BSS) { segfile[i] = 0; continue; }
        cur = align4k(cur);
        segfile[i] = cur;
        cur += b->seg_datalen[i];
    }

    /* ---- 组装文件 ---- */
    Buf out; buf_init(&out);
    /* Header 48 */
    buf_put(&out, "NUC!", 4);
    put16(&out, 0x0100);                 /* abi_version: major=1, minor=0 */
    put16(&out, b->flags);
    put32(&out, b->entry);
    put16(&out, (uint16_t)b->seg_count);
    put16(&out, (uint16_t)b->sym_count);
    put32(&out, strtab_offset);
    put32(&out, rel_offset);
    put32(&out, b->stack);
    put32(&out, (uint32_t)b->rel_count);
    /* 头部填充到 NUC_HDR_SIZE：magic(4)+字段(28)=32 字节，其余为 reserved（规范 §3.2：0x20 起预留，全 0） */
    for (int i = 0; i < (NUC_HDR_SIZE - 32); i++) put8(&out, 0);

    /* Segment Table */
    for (int i = 0; i < b->seg_count; i++) {
        put32(&out, b->segs[i].type);
        put32(&out, (uint32_t)(b->segs[i].type == NUC_SEG_BSS ? 0 : segfile[i]));
        put64(&out, b->segs[i].vaddr);
        put64(&out, b->segs[i].size);
        put32(&out, b->segs[i].flags);
        put32(&out, 0);
    }
    /* Symbol Table */
    for (int i = 0; i < b->sym_count; i++) {
        put32(&out, symoff[i]);
        put8(&out, b->syms[i].type);
        put8(&out, b->syms[i].bind);
        put16(&out, b->syms[i].shndx);
        put64(&out, b->syms[i].value);
        put64(&out, b->syms[i].size);
    }
    /* Relocation Table */
    for (int i = 0; i < b->rel_count; i++) {
        put64(&out, b->rels[i].r_offset);
        put32(&out, b->rels[i].r_type);
        put32(&out, b->rels[i].sym_idx);
    }
    /* String Table */
    while (out.len < strtab_offset) put8(&out, 0);   /* 对齐填充 */
    buf_put(&out, strtab.p, strtab.len);
    /* 段数据：先填充到 data_start，再按 4KiB 页对齐写各段 */
    while (out.len < data_start) put8(&out, 0);
    for (int i = 0; i < b->seg_count; i++) {
        if (b->segs[i].type == NUC_SEG_BSS) continue;
        while (out.len < segfile[i]) put8(&out, 0);
        buf_put(&out, b->seg_data[i], b->seg_datalen[i]);
    }

    free(symoff); free(segfile); free(strtab.p);
    /* 注意：seg_data[i] 仍归 NucBuilder 所有，由 nuc_builder_free 负责释放，
       这里不能 free，否则会与 nuc_builder_free 形成双重释放（堆损坏）。 */
    *out_size = out.len;
    return out.p;
}

void nuc_builder_free(NucBuilder* b) {
    if (!b) return;
    for (int i = 0; i < b->seg_count; i++) free(b->seg_data[i]);
    free(b->segs); free(b->seg_data); free(b->seg_datalen);
    for (int i = 0; i < b->sym_count; i++) free(b->syms[i].name);
    free(b->syms);
    free(b->rels);
    free(b);
}

/* ====================== 解析 ====================== */
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8 * i); return v;
}

NucFile* nuc_parse(const uint8_t* buf, size_t len) {
    if (len < 48) return NULL;
    if (memcmp(buf, "NUC!", 4) != 0) return NULL;
    NucFile* f = (NucFile*)calloc(1, sizeof(NucFile));
    f->raw_size = len;
    f->raw = (uint8_t*)malloc(len);
    memcpy(f->raw, buf, len);

    const uint8_t* h = f->raw;
    f->abi_version = rd16(h + 4);
    f->compile_flag = rd16(h + 6);
    f->entry_point = rd32(h + 8);
    f->seg_count = rd16(h + 12);
    f->symtab_count = rd16(h + 14);
    f->strtab_offset = rd32(h + 16);
    f->rel_offset = rd32(h + 20);
    f->stack_size = rd32(h + 24);
    f->rel_count = rd32(h + 28);

    f->segments = (NucSegment*)malloc(sizeof(NucSegment) * (f->seg_count ? f->seg_count : 1));
    const uint8_t* st = f->raw + NUC_HDR_SIZE;
    for (uint16_t i = 0; i < f->seg_count; i++) {
        const uint8_t* e = st + (size_t)i * 32;
        f->segments[i].type = rd32(e);
        f->segments[i].file_offset = rd32(e + 4);
        f->segments[i].vaddr = rd64(e + 8);
        f->segments[i].size = rd64(e + 16);
        f->segments[i].flags = rd32(e + 24);
        f->segments[i].reserved = rd32(e + 28);
    }
    f->symbols = (NucSymbol*)malloc(sizeof(NucSymbol) * (f->symtab_count ? f->symtab_count : 1));
    const uint8_t* sy = f->raw + NUC_HDR_SIZE + (size_t)f->seg_count * 32;
    for (uint16_t i = 0; i < f->symtab_count; i++) {
        const uint8_t* e = sy + (size_t)i * 24;
        f->symbols[i].name_off = rd32(e);
        f->symbols[i].type = e[4];
        f->symbols[i].bind = e[5];
        f->symbols[i].shndx = rd16(e + 6);
        f->symbols[i].value = rd64(e + 8);
        f->symbols[i].size = rd64(e + 16);
    }
    f->relocs = (NucReloc*)malloc(sizeof(NucReloc) * (f->rel_count ? f->rel_count : 1));
    const uint8_t* rl = f->raw + f->rel_offset;
    for (uint32_t i = 0; i < f->rel_count; i++) {
        const uint8_t* e = rl + (size_t)i * 16;
        f->relocs[i].r_offset = rd64(e);
        f->relocs[i].r_type = rd32(e + 8);
        f->relocs[i].sym_idx = rd32(e + 12);
    }
    f->strtab = (char*)(f->raw + f->strtab_offset);
    /* strtab_size：到段数据起点为止 */
    uint64_t data_start = f->raw_size;
    for (uint16_t i = 0; i < f->seg_count; i++) {
        if (f->segments[i].type != NUC_SEG_BSS && f->segments[i].file_offset &&
            f->segments[i].file_offset < data_start)
            data_start = f->segments[i].file_offset;
    }
    f->strtab_size = (uint32_t)(data_start - f->strtab_offset);
    return f;
}

void nuc_file_free(NucFile* f) {
    if (!f) return;
    free(f->segments); free(f->symbols); free(f->relocs); free(f->raw);
    free(f);
}

/* ====================== 校验 ====================== */
NucValidate nuc_validate(const NucFile* f) {
    if (memcmp(f->raw, "NUC!", 4) != 0)
        return (NucValidate){ NOVUS_ENOTFOUND, "bad magic (not 'NUC!')" };
    if ((f->abi_version >> 8) != 1)
        return (NucValidate){ NOVUS_EINVAL, "abi major version != 1" };
    if (!(f->compile_flag & NUC_FLAG_64BIT))
        return (NucValidate){ NOVUS_EINVAL, "not 64-bit (compile_flag bit0 must be 1)" };
    if (f->seg_count < 1 || f->seg_count > 16)
        return (NucValidate){ NOVUS_EINVAL, "seg_count out of range [1,16]" };

    /* §3.2: reserved 区（0x20 起）必须全 0 */
    for (int i = 32; i < NUC_HDR_SIZE; i++)
        if (f->raw[i] != 0)
            return (NucValidate){ NOVUS_EINVAL, "reserved header bytes must be zero" };

    /* §2 布局铁律 + §12: 段表/符号表/重定位表/字符串表不得越出文件 */
    uint64_t segTblEnd = (uint64_t)NUC_HDR_SIZE + (uint64_t)f->seg_count * 32u;
    uint64_t symTblEnd = segTblEnd + (uint64_t)f->symtab_count * 24u;
    if (symTblEnd > f->raw_size)
        return (NucValidate){ NOVUS_EOVERFLOW, "segment/symbol table exceeds file size" };
    if (f->rel_count > 0 &&
        ((uint64_t)f->rel_offset < symTblEnd ||
         (uint64_t)f->rel_offset + (uint64_t)f->rel_count * 16u > f->raw_size))
        return (NucValidate){ NOVUS_EOVERFLOW, "relocation table out of bounds or overlaps" };
    if ((uint64_t)f->strtab_offset >= f->raw_size)
        return (NucValidate){ NOVUS_EOVERFLOW, "string table offset out of bounds" };

    if (f->compile_flag & NUC_FLAG_STATIC) {
        for (uint16_t i = 0; i < f->symtab_count; i++)
            if (f->symbols[i].bind == NUC_BIND_EXTERN)
                return (NucValidate){ NOVUS_EINVAL, "static binary must not have EXTERN symbols" };
    }
    /* §5/§7.3: 符号名偏移/段索引、重定位符号索引须合法；重定位类型仅 32/PC32 */
    for (uint16_t i = 0; i < f->symtab_count; i++) {
        if (f->symbols[i].name_off >= f->strtab_size)
            return (NucValidate){ NOVUS_EOVERFLOW, "symbol name offset out of string table" };
        if (f->symbols[i].shndx != 0 && f->symbols[i].shndx >= f->seg_count)
            return (NucValidate){ NOVUS_EOVERFLOW, "symbol shndx out of range" };
    }
    for (uint32_t i = 0; i < f->rel_count; i++) {
        if (f->relocs[i].sym_idx >= f->symtab_count)
            return (NucValidate){ NOVUS_EOVERFLOW, "reloc sym_idx out of range" };
        if (f->relocs[i].r_type != R_X86_64_32 && f->relocs[i].r_type != R_X86_64_PC32)
            return (NucValidate){ NOVUS_EINVAL, "unsupported relocation type (only R_X86_64_32/PC32)" };
    }

    /* 段规则：§4.3 VM_USER 必置；§4.4 TEXT=R+X、DATA 不可执行（W^X）、BSS 不占文件；§2 段数据不越界 */
    for (uint16_t i = 0; i < f->seg_count; i++) {
        const NucSegment* s = &f->segments[i];
        if (!(s->flags & NUC_VM_USER))
            return (NucValidate){ NOVUS_EINVAL, "segment must set VM_USER (bit3)" };
        if (s->type == NUC_SEG_TEXT) {
            if (!(s->flags & NUC_VM_READ) || !(s->flags & NUC_VM_EXEC) || (s->flags & NUC_VM_WRITE))
                return (NucValidate){ NOVUS_EINVAL, "TEXT must be R+X and not writable" };
            if ((uint64_t)s->file_offset + s->size > f->raw_size)
                return (NucValidate){ NOVUS_EOVERFLOW, "TEXT segment data exceeds file size" };
        }
        if (s->type == NUC_SEG_DATA || s->type == NUC_SEG_RODATA) {
            if (s->type == NUC_SEG_DATA && (s->flags & NUC_VM_EXEC))
                return (NucValidate){ NOVUS_EINVAL, "DATA must not be executable (W^X)" };
            if ((uint64_t)s->file_offset + s->size > f->raw_size)
                return (NucValidate){ NOVUS_EOVERFLOW, "segment data exceeds file size" };
        }
        if (s->type == NUC_SEG_BSS && s->file_offset != 0)
            return (NucValidate){ NOVUS_EINVAL, "BSS must not occupy file space (file_offset must be 0)" };
    }
    return (NucValidate){ NOVUS_OK, "valid NUC V1.0" };
}

/* ====================== Dump ====================== */
static const char* seg_type_name(uint32_t t) {
    switch (t) { case 0: return "TEXT"; case 1: return "DATA"; case 2: return "BSS"; case 3: return "RODATA"; }
    return "??";
}
static const char* sym_type_name(uint8_t t) {
    switch (t) { case 0: return "NOTYPE"; case 1: return "FUNC"; case 2: return "OBJECT"; case 3: return "FILE"; }
    return "??";
}
static const char* bind_name(uint8_t b) {
    switch (b) { case 0: return "LOCAL"; case 1: return "GLOBAL"; case 2: return "EXTERN"; }
    return "??";
}
static const char* rel_type_name(uint32_t t) {
    switch (t) { case 1: return "R_X86_64_32"; case 2: return "R_X86_64_PC32"; case 3: return "R_X86_64_64"; }
    return "??";
}

void nuc_dump(const NucFile* f, FILE* out) {
    fprintf(out, "=== NUC Header ===\n");
    fprintf(out, "magic         : NUC!\n");
    fprintf(out, "abi_version   : %u.%u\n", (f->abi_version >> 8) & 0xff, f->abi_version & 0xff);
    fprintf(out, "compile_flag  : 0x%04X (64bit=%d pic=%d static=%d stackprot=%d debug=%d)\n",
            f->compile_flag,
            (f->compile_flag >> 0) & 1, (f->compile_flag >> 1) & 1,
            (f->compile_flag >> 2) & 1, (f->compile_flag >> 3) & 1, (f->compile_flag >> 4) & 1);
    fprintf(out, "entry_point   : 0x%X (.text 内偏移)\n", f->entry_point);
    fprintf(out, "seg_count     : %u\n", f->seg_count);
    fprintf(out, "symtab_count  : %u\n", f->symtab_count);
    fprintf(out, "strtab_offset : 0x%X\n", f->strtab_offset);
    fprintf(out, "rel_offset    : 0x%X\n", f->rel_offset);
    fprintf(out, "stack_size    : 0x%X (%u bytes)\n", f->stack_size, f->stack_size);
    fprintf(out, "rel_count     : %u\n", f->rel_count);

    fprintf(out, "\n=== Segment Table ===\n");
    for (uint16_t i = 0; i < f->seg_count; i++) {
        const NucSegment* s = &f->segments[i];
        fprintf(out, "[%2u] %-7s file_off=0x%08X vaddr=0x%016llX size=0x%llX flags=0x%X\n",
                i, seg_type_name(s->type), s->file_offset,
                (unsigned long long)s->vaddr, (unsigned long long)s->size, s->flags);
    }
    if (f->symtab_count) {
        fprintf(out, "\n=== Symbol Table ===\n");
        for (uint16_t i = 0; i < f->symtab_count; i++) {
            const NucSymbol* s = &f->symbols[i];
            const char* nm = (s->name_off < f->strtab_size) ? (f->strtab + s->name_off) : "?";
            fprintf(out, "[%2u] %-16s type=%-7s bind=%-6s shndx=%u value=0x%llX size=0x%llX\n",
                    i, nm, sym_type_name(s->type), bind_name(s->bind), s->shndx,
                    (unsigned long long)s->value, (unsigned long long)s->size);
        }
    }
    if (f->rel_count) {
        fprintf(out, "\n=== Relocation Table ===\n");
        for (uint32_t i = 0; i < f->rel_count; i++) {
            const NucReloc* r = &f->relocs[i];
            const char* symnm = "?";
            if (r->sym_idx < f->symtab_count) {
                uint32_t no = f->symbols[r->sym_idx].name_off;
                if (no < f->strtab_size) symnm = f->strtab + no;
            }
            fprintf(out, "[%2u] r_offset=0x%016llX type=%-12s sym[%u]=%s\n",
                    i, (unsigned long long)r->r_offset, rel_type_name(r->r_type), r->sym_idx, symnm);
        }
    }
    /* entry_rip */
    for (uint16_t i = 0; i < f->seg_count; i++) {
        if (f->segments[i].type == NUC_SEG_TEXT) {
            fprintf(out, "\nentry_rip = text.vaddr + entry_point = 0x%016llX\n",
                    (unsigned long long)(f->segments[i].vaddr + f->entry_point));
            break;
        }
    }
}
