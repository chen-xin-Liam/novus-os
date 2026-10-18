# NUC 可执行二进制格式规范
> 文档版本：V1.0（对应 Novus OS V1.6-FinalSpec）
> 适用对象：`cppcc` 编译器开发者、内核 NUC 加载器开发者、应用开发者
> 平台：x86_64 UEFI
> 状态：**格式冻结**，主版本变更需走 ABI 变更评审流程

---

## 1. 概述

NUC（Novus Universal Code）是 Novus OS 原生可执行文件格式，扩展名 `.nuc`。
设计目标：

- 极简、紧凑、适合裸机内核直接加载；
- 支持静态链接与动态链接（系统 SDK）；
- 支持位置无关代码（PIC）；
- 加载器在内核 Ring0 内置，无用户态外部依赖；
- 与系统调用 ABI（`rax` 调用号 / `rdi,rsi,rdx,rcx,r8,r9` 参数）完全对齐。

与 ELF 的差异：不使用 Program Header / Section Header 分离设计；段表与符号表集中存放，加载器只需一次顺序扫描。

---

## 2. 文件整体布局

一个 `.nuc` 文件由以下区域**顺序拼接**而成（无空洞，按 4 字节对齐）：

```
偏移 0x0000 ┌──────────────────────────┐
            │  NUC Header (nuc_hdr_t)  │   固定 48 字节
偏移 0x0030 ├──────────────────────────┤
            │  Segment Table           │   seg_count × nuc_segment_t (32B/条)
偏移 ...    ├──────────────────────────┤
            │  Symbol Table            │   sym_count × nuc_sym_t (24B/条)
偏移 ...    ├──────────────────────────┤
            │  Relocation Table         │   rel_count × nuc_rel_t (16B/条)
偏移 ...    ├──────────────────────────┤
            │  String Table            │   字符串池，'\0' 分隔
偏移 ...    ├──────────────────────────┤
            │  段数据（.text/.data）    │   各段原始字节，按 4KiB 页对齐填充
            └──────────────────────────┘
```

> 布局铁律：Header → Segment Table → Symbol Table → Relocation Table → String Table → 段数据。
> 加载器严格按此顺序解析，无需二次跳表。

---

## 3. NUC Header（文件头）

固定长度 **48 字节**，位于文件偏移 `0x0000`，按 4 字节对齐。

> 修订说明（2026-10-18）：本节原文的 `reserved` 写作 `uint64_t[4]`（32 字节），
> 与「总大小 48 字节」、§3.2 的偏移表、§2 的布局图以及 §9.2 的「读取前 48 字节」
> 互相矛盾。现按**偏移表与既有实现**统一为 48 字节：`reserved` 取 16 字节，
> 字段偏移与 ABI 主版本均不变。内核加载器（`kernel/nuc.c`）与 mochi 的
> builder/parser 原本就按 48 字节布局读写，因此这是文档勘误，不是 ABI 变更。

### 3.1 结构体定义

```c
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];         // 0x00  "NUC!" = 4E 55 43 21
    uint16_t abi_version;       // 0x04  bit[0..7]=minor, bit[8..15]=major
    uint16_t compile_flag;      // 0x06  编译标志位
    uint32_t entry_point;       // 0x08  .text 段内字节偏移（见 §9）
    uint16_t seg_count;         // 0x0C  段数量
    uint16_t symtab_count;      // 0x0E  符号数量（用于加载器校验）
    uint32_t strtab_offset;    // 0x10  字符串表在文件中的偏移
    uint32_t rel_offset;        // 0x14  重定位表在文件中的偏移
    uint32_t stack_size;        // 0x18  用户栈大小（字节）
    uint32_t rel_count;         // 0x1C  重定位条目数量
    uint32_t reserved[4];       // 0x20  预留 16 字节，必须为 0
} nuc_hdr_t;                     // 总大小 0x30 = 48 字节
```

### 3.2 字段逐字节定义

| 偏移 | 大小 | 字段 | 取值规则 |
|---|---|---|---|
| 0x00 | 4 | magic | 固定 `4E 55 43 21`（"NUC!"），不匹配拒绝加载 |
| 0x04 | 2 | abi_version | 高字节=major，低字节=minor。当前冻结值：`0x0100`（major=1, minor=0） |
| 0x06 | 2 | compile_flag | 位掩码，见 §3.3 |
| 0x08 | 4 | entry_point | `.text` 段内字节偏移，见 §9 |
| 0x0C | 2 | seg_count | 段表条目数，范围 1~16 |
| 0x0E | 2 | symtab_count | 符号表条目数，0 表示无符号（纯静态） |
| 0x10 | 4 | strtab_offset | 字符串表文件偏移，对齐到 4 字节 |
| 0x14 | 4 | rel_offset | 重定位表文件偏移，对齐到 4 字节 |
| 0x18 | 4 | stack_size | 用户栈字节数，默认 8MiB（`0x800000`），LowMem 4MiB |
| 0x1C | 4 | rel_count | 重定位条目数，0 表示无需重定位（纯静态可直接运行） |
| 0x20 | 16 | reserved | 必须全 0，未来扩展用；头部到 `0x30` 结束 |

### 3.3 compile_flag 位定义

| 位 | 名称 | 含义 |
|---|---|---|
| bit0 | `NUC_FLAG_64BIT` | 0=32位，1=x86_64。当前内核仅支持 1 |
| bit1 | `NUC_FLAG_PIC` | 位置无关代码（PIC），动态链接时必须置 1 |
| bit2 | `NUC_FLAG_STATIC` | 1=静态链接（无外部符号），0=动态链接（依赖系统 SDK） |
| bit3 | `NUC_FLAG_STACKPROT` | 1=启用栈保护（栈金丝雀），0=关闭 |
| bit4 | `NUC_FLAG_DEBUG` | 1=包含调试符号，0=发布版（stripped） |
| bit5~15 | 保留 | 必须为 0 |

---

## 4. Segment Table（段表）

紧随 Header 之后，共 `seg_count` 条。每条 **32 字节**，packed。

### 4.1 结构体

```c
typedef struct __attribute__((packed)) {
    uint32_t type;          // 0x00  段类型
    uint32_t file_offset;   // 0x04  该段数据在 .nuc 文件中的偏移
    uint64_t vaddr;         // 0x08  加载到用户空间的虚拟起始地址
    uint64_t size;          // 0x10  段在内存中的大小（字节）
    uint32_t flags;         // 0x18  段权限位
    uint32_t reserved;      // 0x1C  必须为 0
} nuc_segment_t;             // 32 字节
```

### 4.2 段类型

| type 值 | 名称 | 说明 |
|---|---|---|
| 0 | `NUC_SEG_TEXT` | 代码段，可执行、只读 |
| 1 | `NUC_SEG_DATA` | 数据段，可读写 |
| 2 | `NUC_SEG_BSS` | 未初始化数据段，**文件中不占空间**（file_offset=0），加载时清零 |
| 3 | `NUC_SEG_RODATA` | 只读数据段（字符串常量、常量表） |

### 4.3 flags 权限位

| 位 | 含义 |
|---|---|
| bit0 | 可读（`VM_READ`） |
| bit1 | 可写（`VM_WRITE`） |
| bit2 | 可执行（`VM_EXEC`） |
| bit3 | 用户态可见（`VM_USER`，必置） |

典型组合（均已包含必置的 bit3）：
- TEXT：`0b1101`（`0x0D`，R+X+USER）
- DATA：`0b1011`（`0x0B`，R+W+USER）
- RODATA：`0b1001`（`0x09`，R+USER）
- BSS：`0b1011`（`0x0B`，R+W+USER，文件无数据）

> 注意：旧稿的 `0b0101` / `0b0011` / `0b0001` 没有置 bit3，与「`VM_USER` 必置」
> 冲突，不能直接用作段 flags。加载器应按 bit3 校验，缺位即 `NOVUS_EINVAL`。

### 4.4 加载规则

1. 加载器按 `file_offset` 把段数据读入内存 `vaddr`；
2. BSS 段：不读文件，按 `size` 清零；
3. 映射时按 4KiB 页边界向上取整（`size` 字段本身可以是任意字节数）；
4. TEXT 段必须可读可执行；DATA 段不可执行（W^X 原则）。

---

## 5. Symbol Table（符号表）

紧接段表之后，共 `symtab_count` 条。每条 **24 字节**。

### 5.1 结构体

```c
typedef struct __attribute__((packed)) {
    uint32_t name_off;      // 0x00  符号名在 String Table 中的偏移
    uint8_t  type;          // 0x04  符号类型
    uint8_t  bind;          // 0x05  绑定属性
    uint16_t shndx;         // 0x06  所属段索引（本地符号有效，外部符号为 0）
    uint64_t value;         // 0x08  符号值（本地=段内偏移；外部=0）
    uint64_t size;          // 0x10  符号大小（函数/对象字节数）
} nuc_sym_t;                  // 24 字节
```

### 5.2 type 取值

| type | 名称 | 说明 |
|---|---|---|
| 0 | `NUC_SYM_NOTYPE` | 未指定 |
| 1 | `NUC_SYM_FUNC` | 函数符号 |
| 2 | `NUC_SYM_OBJECT` | 数据对象（变量） |
| 3 | `NUC_SYM_FILE` | 源文件名（调试用） |

### 5.3 bind 取值

| bind | 名称 | 说明 |
|---|---|---|
| 0 | `NUC_BIND_LOCAL` | 本地符号，仅本文件可见 |
| 1 | `NUC_BIND_GLOBAL` | 全局符号，可被其他模块引用 |
| 2 | `NUC_BIND_EXTERN` | 外部符号（依赖系统 SDK 提供，加载时需解析） |

外部符号（bind=2）必须在重定位表中被引用；系统 SDK 必须提供对应实现。

---

## 6. String Table（字符串表）

位于 Header 中 `strtab_offset` 指定位置。

- 所有字符串以 `\0` 结尾；
- 第一个字节固定为 `\0`（即偏移 0 是空字符串）；
- 符号名、段名、外部依赖名均存于此；
- 加载时不复制，仅通过偏移引用。

示例：
```
0x0000: 00                    ; 空串
0x0001: "main" 00
0x0006: "printf" 00
0x000D: "sys_gfx_create_surface" 00
```

---

## 7. Relocation Table（重定位表）

紧接符号表之后（偏移由 Header 的 `rel_offset` 指定），共 `rel_count` 条。每条 **16 字节**。

### 7.1 结构体

```c
typedef struct __attribute__((packed)) {
    uint64_t r_offset;     // 0x00  需要重定位位置的虚拟地址
    uint32_t r_type;        // 0x08  重定位类型
    uint32_t sym_idx;       // 0x0C  引用的符号索引（符号表下标，从 0 开始）
} nuc_rel_t;                // 16 字节
```

### 7.2 重定位类型（冻结）

| r_type | 名称 | 计算公式 | 说明 |
|---|---|---|---|
| 1 | `R_X86_64_32` | `*((uint32_t*)r_offset) = sym_value` | 32 位绝对地址（小模型） |
| 2 | `R_X86_64_PC32` | `*((int32_t*)r_offset) = sym_value - r_offset - 4` | 32 位相对地址（call/jmp） |
| 3 | `R_X86_64_64` | `*((uint64_t*)r_offset) = sym_value` | 64 位绝对地址（动态链接/PIC） |

> 当前冻结支持 1、2 两种；类型 3 预留给未来 PIC/动态链接扩展，编译器可生成但加载器在 V1.0 阶段对未实现类型直接报错 `NOVUS_EINVAL`。

### 7.3 重定位解析流程

1. 加载器遍历重定位表；
2. 对每条，取 `sym_idx` 查符号表；
3. 若符号为 `BIND_EXTERN`（bind=2），调用 `nuc_resolve_sym(name)` 向系统 SDK 查询；
4. 若符号为 `BIND_LOCAL`/`GLOBAL`，`sym_value` = 所属段 `vaddr + value`；
5. 按 `r_type` 公式写入目标位置；
6. 任何外部符号解析失败 → 加载失败，返回 `NOVUS_ENOTFOUND`。

---

## 8. 段数据区

文件末尾区域，存放 TEXT / DATA / RODATA 的原始字节。

- 各段在文件中按段表顺序连续排列；
- 段与段之间按 **4KiB 页对齐**填充 0，方便页表映射；
- BSS 段不占文件空间；
- 段数据内容直接由编译器生成，加载器不做解释。

---

## 9. 入口点与启动流程

### 9.1 入口地址计算公式（冻结）

```
entry_rip = text_segment.vaddr + nuc_hdr.entry_point
```

- `entry_point` 是 **.text 段内字节偏移**，不是文件偏移，也不是虚拟地址；
- 加载器找到 `type == NUC_SEG_TEXT` 的段，取其 `vaddr`，加上 `entry_point`，即为程序第一条指令的虚拟地址。

### 9.2 内核加载完整流程（冻结）

```
1. sys_nuc_exec(path) 被调用
2. VFS 打开文件，读取前 48 字节（Header）
3. 校验 magic == "NUC!"，否则 NOVUS_ENOTFOUND
4. 校验 abi_version 主版本号 == 1，否则 NOVUS_EINVAL
5. 校验 compile_flag bit0 == 1（64位），否则 NOVUS_EINVAL
6. 分配用户栈（大小 = hdr.stack_size）
7. 遍历段表，vm_map 映射各段；BSS 清零
8. 遍历重定位表，解析符号并修补地址
9. 设置用户态页表、栈指针、返回地址
10. 计算 entry_rip = text.vaddr + hdr.entry_point
11. 上下文切换到用户态，跳转 entry_rip
```

---

## 10. ABI 兼容性规则

| 情况 | 结果 |
|---|---|
| `abi_version` 主版本号 ≠ 1 | 拒绝加载（`NOVUS_EINVAL`） |
| `abi_version` 次版本号 > 当前内核次版本 | 向前兼容，允许加载（新字段内核不认识时忽略） |
| `abi_version` 次版本号 < 当前内核次版本 | 向后兼容，允许加载 |
| `compile_flag bit0` ≠ 1（非64位） | 拒绝加载 |
| `compile_flag bit2`（静态）且存在 EXTERN 符号 | 拒绝加载（自相矛盾） |

---

## 11. 完整示例：一个最小 .nuc 文件

下面是一个打印 "Hello" 的最小程序。所有偏移都按 §2 的顺序和 48 字节头重新推导，
可以直接用 §9 的公式验算（小端序）。

### 11.1 布局推导

| 区域 | 起始偏移 | 大小 | 结束偏移 |
|---|---|---|---|
| Header | `0x0000` | `0x30` | `0x0030` |
| Segment Table（2 条） | `0x0030` | `2 × 32 = 0x40` | `0x0070` |
| Symbol Table（1 条） | `0x0070` | `1 × 24 = 0x18` | `0x0088` |
| Relocation Table（1 条） | `0x0088` | `1 × 16 = 0x10` | `0x0098` |
| String Table | `0x0098` | `6` | `0x009E` |
| 填充到段数据 | `0x009E` | — | `0x0100` |
| `.text` | `0x0100` | `0x200` | `0x0300` |
| 页对齐填充 | `0x0300` | — | `0x1000` |
| `.data` | `0x1000` | `0x100` | `0x1100` |

因此 `rel_offset = 0x88`、`strtab_offset = 0x98`，且重定位表位于字符串表**之前**，
满足 §2 的布局铁律。

### 11.2 Header（48 字节）

```
偏移  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F
0000: 4E 55 43 21 00 01 05 00 10 00 00 00 02 00 01 00   ; magic, abi=0x0100(1.0), flag=0x0005(64bit+static), entry=0x10, seg=2, sym=1
0010: 98 00 00 00 88 00 00 00 00 00 80 00 01 00 00 00   ; strtab=0x98, rel=0x88, stack=8MiB, rel_count=1
0020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ; reserved[16]，必须全 0
```

注意：`compile_flag = 0x0005` 只置了 bit0（64 位）和 bit2（静态），
bit3（栈保护）未置；旧稿注释里的 "stackprot" 是错的。示例程序不含栈金丝雀，
所以这里保持 `0x0005`。

### 11.3 Segment Table（2 段 × 32B）

```
.text 段:
  type=0, file_offset=0x100, vaddr=0x400000, size=0x200, flags=0x0D (R+X+USER)
.data 段:
  type=1, file_offset=0x1000, vaddr=0x600000, size=0x100, flags=0x0B (R+W+USER)
```

### 11.4 Symbol Table（1 条）

```
  name_off=0x01, type=1(FUNC), bind=0(LOCAL), shndx=0, value=0x20, size=0x20
  ; 对应字符串 "main"；value 是 .text 段内偏移，实际地址 = 0x400000 + 0x20 = 0x400020
```

### 11.5 Relocation Table（1 条）

```
  r_offset=0x400010, r_type=2(PC32), sym_idx=0
  ; 入口 _start（text+0x10）处的 call 指令，目标为 main（text+0x20）
  ; PC32 写入值 = 0x400020 - 0x400010 - 4 = 0x0C
```

### 11.6 字符串表

```
  0x00: 00
  0x01: 'm' 'a' 'i' 'n' 00
```

加载后：
- text 段映射到 `0x400000`
- `entry_rip = 0x400000 + 0x10 = 0x400010`（入口 `_start`）
- `_start` 的 `call` 经 PC32 重定位指向 `0x400020`（`main`）

---

## 12. 错误码（加载器相关）

| 错误码 | 含义 |
|---|---|
| `NOVUS_OK (0)` | 加载成功 |
| `NOVUS_ENOTFOUND (-3)` | magic 错误或外部符号未解析 |
| `NOVUS_EINVAL (-1)` | 头部字段非法（版本/位宽/段数越界） |
| `NOVUS_ENOMEM (-2)` | 段映射/栈分配失败 |
| `NOVUS_EIO (-6)` | 文件读取失败 |
| `NOVUS_EOVERFLOW (-11)` | 段大小/偏移越界，文件损坏 |

---

## 13. 与 NKM 内核模块格式的关系

NUC 与 NKM 都是可重定位二进制，但**两者格式不同**，不可混用：

| 维度 | NUC（用户态） | NKM（内核模块） |
|---|---|---|
| 魔数 | `NUC!` | NKM 专用魔数 |
| 加载位置 | 用户虚拟地址空间 | 内核地址空间 |
| 权限 | 用户态页表 | 内核页表 |
| 签名 | 无（普通程序） | 必须 Ed25519 签名 |
| 入口 | `entry_point` 用户态函数 | `init()` 模块初始化函数 |
| 卸载 | 进程退出自动回收 | 需 `nkm_unload` |

---

## 14. 文档冻结声明

本规范为 NUC 二进制格式唯一基准文档。编译器 `cppcc` 输出、内核加载器实现、调试工具（`nucdump`）必须严格遵循：

- Header 48 字节布局；
- 段表 32 字节、符号表 24 字节、重定位表 16 字节；
- 重定位类型仅 `R_X86_64_32` / `R_X86_64_PC32`；
- 入口计算公式 `text.vaddr + entry_point`；
- ABI 主版本 = 1。

任何字段偏移、大小、语义变更必须提交 ABI 变更 RFC，并升版本号。

### 14.1 修订记录

| 日期 | 修订 | ABI 影响 |
|---|---|---|
| 2026-10-18 | §3.1 的 `reserved` 由 `uint64_t[4]` 更正为 `uint32_t[4]`（16 字节），与 48 字节头、§3.2 偏移表、§9.2 及内核/mochi 实现统一；§4.3 典型 flags 补上必置的 `VM_USER`；§11 示例按 48 字节头重算全部偏移 | 无：字段偏移和二进制布局不变，仅修正文档自相矛盾 |

> 决策记录：Header 长度以 **48 字节**为准（`0x30`）。依据是 §2 布局图、§3.2
> 偏移表、§9.2「读取前 48 字节」、§14 冻结声明，以及 `kernel/nuc.c` 和 mochi
> 现有 builder/parser/产物。`reserved` 固定 16 字节，不得再改回 32 字节；
> 未来若确需扩展，必须走 ABI 变更 RFC 并升主版本。

文档状态：✅ **冻结，可作为 M4（NUC 加载器）与 M7（cppcc 编译器）的实现基准。**
