// SPDX-License-Identifier: GPL-2.0
/*
 * nucfmt.h — NUC 可执行二进制格式（novus-os）读写库  V1.0
 * 严格遵循《NUC 可执行二进制格式规范 V1.0》。
 * 布局铁律：Header(48) -> SegmentTable -> SymbolTable -> RelocationTable
 *           -> StringTable -> 段数据(4KiB 页对齐)
 */
#ifndef NUCFMT_H
#define NUCFMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ---------- 常量 ---------- */
#define NUC_MAGIC_LE  0x2143554E   /* 字节序 4E 55 43 21 = "NUC!" */
#define NUC_HDR_SIZE  48           /* nuc_hdr_t 固定长度（规范 §3：0x30=48 字节、布局铁律 Header(48)） */

/* 段类型 */
#define NUC_SEG_TEXT   0
#define NUC_SEG_DATA   1
#define NUC_SEG_BSS    2
#define NUC_SEG_RODATA 3

/* ---- Nuomi 字符串的运行时内存布局（编译产物专用）----
 *
 * 刻意不采用主流语言常见的「两字指针(指针,长度) + 独立字符数组」布局，
 * 而是给每个字符串一个**自描述头部**：
 *
 *   offset 0   int64_t  len    字节数（不含头部）
 *   offset 8   int64_t  hash   内容哈希（FNV-1a），供运行时快速比较/查重
 *   offset 16  char[]   字节数据（非 NUL 结尾；长度以 len 字段为准）
 *   总大小 = 16 + len，按 8 字节向上对齐
 *
 * 好处：
 *   ① 单个指针即代表整个字符串，传递只用一个寄存器，不必传 (ptr,len) 对；
 *   ② 比较字符串前先比 hash，不等才逐字节 —— 大量相等性判断会变快；
 *   ③ 头部让运行时能自省长度，无需外部元数据；
 *   ④ 数据段里天然紧凑，可直接 memcmp。
 *
 * 代码里取字段用 NUOMI_STR_LEN / NUOMI_STR_HASH / NUOMI_STR_BYTES，
 * 避免各处硬编码 16这个数字。
 */
#define NUOMI_STR_HEADER   16
#define NUOMI_STR_BYTES(sz) (NUOMI_STR_HEADER + (((sz) + 7u) & ~7u))

/* BSS 段（未初始化数据）虚拟地址：紧随 RODATA 之后。
 * 运行时供内置辅助函数（如 itoa）当可写缓冲区用。 */
#define NUOMI_BSS_VADDR    0x600000u

/* 段权限位 */
#define NUC_VM_READ   1
#define NUC_VM_WRITE  2
#define NUC_VM_EXEC   4
#define NUC_VM_USER   8

/* compile_flag 位 */
#define NUC_FLAG_64BIT    (1u << 0)
#define NUC_FLAG_PIC      (1u << 1)
#define NUC_FLAG_STATIC   (1u << 2)
#define NUC_FLAG_STACKPROT (1u << 3)
#define NUC_FLAG_DEBUG    (1u << 4)

/* 符号类型 */
#define NUC_SYM_NOTYPE 0
#define NUC_SYM_FUNC   1
#define NUC_SYM_OBJECT 2
#define NUC_SYM_FILE   3

/* 符号绑定 */
#define NUC_BIND_LOCAL  0
#define NUC_BIND_GLOBAL 1
#define NUC_BIND_EXTERN 2

/* 重定位类型 */
#define R_X86_64_32  1
#define R_X86_64_PC32 2
#define R_X86_64_64  3

/* 加载器错误码 */
#define NOVUS_OK        0
#define NOVUS_EINVAL   -1
#define NOVUS_ENOMEM   -2
#define NOVUS_ENOTFOUND -3
#define NOVUS_EIO      -6
#define NOVUS_EOVERFLOW -11

/* ---------- 表项结构（主机端表示，非文件布局） ---------- */
typedef struct {
    uint32_t type;
    uint32_t file_offset;
    uint64_t vaddr;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
} NucSegment;

typedef struct {
    uint32_t name_off;
    uint8_t  type;
    uint8_t  bind;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
} NucSymbol;

typedef struct {
    uint64_t r_offset;
    uint32_t r_type;
    uint32_t sym_idx;
} NucReloc;

typedef struct {
    /* 头部字段 */
    uint16_t abi_version;     /* 高字节=major, 低字节=minor；冻结值 0x0100 */
    uint16_t compile_flag;
    uint32_t entry_point;     /* .text 段内字节偏移 */
    uint16_t seg_count;
    uint16_t symtab_count;
    uint32_t strtab_offset;
    uint32_t rel_offset;
    uint32_t stack_size;
    uint32_t rel_count;
    /* 解析后的表 */
    NucSegment* segments;
    NucSymbol* symbols;
    NucReloc* relocs;
    char* strtab;
    uint32_t strtab_size;
    /* 整文件 */
    uint8_t* raw;
    size_t raw_size;
} NucFile;

/* ---------- 构建器 ---------- */
typedef struct NucBuilder NucBuilder;
NucBuilder* nuc_builder_new(void);
/* 添加段：BSS 段 data 传 NULL、datalen 传 0（文件中无数据） */
int  nuc_builder_add_segment(NucBuilder* b, uint32_t type, uint64_t vaddr,
                             uint64_t memsize, uint32_t flags,
                             const uint8_t* data, uint32_t datalen);
int  nuc_builder_add_symbol(NucBuilder* b, const char* name, uint8_t type,
                            uint8_t bind, uint16_t shndx, uint64_t value, uint64_t size);
int  nuc_builder_add_reloc(NucBuilder* b, uint64_t r_offset, uint32_t r_type, uint32_t sym_idx);
void nuc_builder_set_entry(NucBuilder* b, uint32_t entry_point);
void nuc_builder_set_flags(NucBuilder* b, uint16_t flags);
void nuc_builder_set_stack(NucBuilder* b, uint32_t stack_size);
/* 生成文件字节；成功返回 malloc 的缓冲区并写 *out_size，失败返回 NULL（errbuf 填原因） */
uint8_t* nuc_builder_build(NucBuilder* b, size_t* out_size, char* errbuf, size_t errlen);
void nuc_builder_free(NucBuilder* b);

/* ---------- 解析与校验 ---------- */
NucFile* nuc_parse(const uint8_t* buf, size_t len);
void nuc_file_free(NucFile* f);

typedef struct { int code; const char* msg; } NucValidate;
NucValidate nuc_validate(const NucFile* f);   /* 应用 §10 ABI 兼容性规则 */

void nuc_dump(const NucFile* f, FILE* out);    /* 人类可读 dump */

#endif /* NUCFMT_H */
