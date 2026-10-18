# M7 cppcc 最小编译器实施方案

状态：M7 已实现。本文件原为实施方案，现已按实际后端修订；范围和使用说明以 `user/cppcc/README.md`、`docs/M7 cppcc 实现说明.md` 为准。

## 1. 目标与约束

在 `user/cppcc/` 实现一个 Rust 编写的主机端工具，将限定的 C++98 单翻译单元编译为可由当前 `kernel/nuc.c` 加载执行的 x86-64 NUC 文件。工具在 Windows/Linux 构建机运行，不进入内核或用户镜像。

- 不依赖 LLVM、外部汇编器或大型第三方 crate；首版可仅用 Rust 标准库。
- 编译链：预处理 → 词法分析 → 语法分析 → 语义检查 → 带类型 AST → x86_64 codegen → nucgen。
- 输出严格遵守 NUC V1.0 冻结格式：48 字节头、32 字节段表项、24 字节符号表项、16 字节重定位表项；只输出加载器支持的 `R_X86_64_32` 和 `R_X86_64_PC32`；入口按 `text.vaddr + entry_point` 计算；ABI 主版本为 1。
- 首版只做静态单文件链接，不生成外部符号导入、不改变 NUC 格式、不依赖应用运行时库。

## 2. 总体架构

```text
CLI / driver
  → preprocess
  → lexer
  → parser → AST
  → sema
  → x86_64 codegen（直接遍历 AST）
  → nucgen → app.nuc
```

建议 crate 内模块职责如下：

| 模块 | 职责与边界 |
|---|---|
| `driver` | 解析输入文件、`-o`、`-I`、AST dump 选项；协调流水线；报告文件与行列号；区分编译错误和 I/O 错误。 |
| `preprocess` | 处理对象式 `#define`、`#include` 和最小头文件保护指令；维护 include 搜索路径、宏表、包含栈与源位置映射；检测递归包含。 |
| `lexer` | 将预处理文本转为带源位置的 token；处理标识符、关键字、整数/字符/字符串字面量、运算符和注释。 |
| `parser` | 递归下降语法分析；输出 AST；错误时尝试在分号、右花括号等边界恢复，以便一次报告多个语法错误。 |
| `ast` | 表示类型、声明、表达式、语句和源位置；不承载代码生成状态。 |
| `sema` | 建立作用域和符号表；检查类型、声明/定义匹配、返回路径、控制流跳转、数组/指针运算以及不支持特性；拒绝任何未解析外部引用。 |
| `x86_64` | 直接遍历语义标注 AST；分配 rbp 栈帧，手写 x86_64 指令编码，生成局部标签分支和 NUC relocation 记录。 |
| `nucgen` | 汇总 codegen 的段/符号/重定位数据并序列化 NUC；生成 Header、表、字符串和页对齐段数据；复核所有边界与规范不变量。 |

后端使用 rbp 基址栈槽和少量固定暂存寄存器，不做寄存器分配优化。指令编码器只覆盖本子集实际需要的 x86-64 指令；未实现语法/codegen 行为必须报错，不得静默输出错误机器码。

CLI：`cppcc input.cpp -o output.nuc [-I include-dir] [--dump-ast]`；`--check` 只运行前端检查。NUC 完整生成与自检成功后才写入输出路径。

## 3. C++98 子集精确范围

这是面向裸机应用的“小型 C++ 风格子集”，不是完整 C++98。所有未列入支持范围的语法必须产生带位置的诊断。

### 3.1 支持

- 翻译单元：单个 `.cpp` 主文件及其文本包含的头文件；函数声明/定义、全局变量声明/定义、前置声明；同一程序内函数不得重载。名字采用 C 风格未修饰符号名，限定在全局命名空间。
- 基本类型：`void`（仅返回类型）、`bool`、`char`、`short`、`int`、`long`、`long long` 及其 `unsigned` 变体；固定宽度按 x86-64 LP64 目标规则：`char=8`、`short=16`、`int=32`、`long/long long=64`、指针=64 位。`char` 为有符号 8 位。支持 `const` 限定，但不支持 volatile。
- 函数：普通非成员函数、原型、值参数、指针参数；`main` 的首版签名限定为 `int main()` 或 `int main(void)`。单翻译单元内定义的函数可互相调用；未定义函数一律报错，内建 Novus syscall 名称除外。
- 变量：全局静态存储、函数局部自动变量、块作用域、标量初始化、常量全局初始化；支持 `static` 内部链接和 `extern` 声明，但不允许最终留下未解析的 `extern` 定义。全局零初始化进入 BSS，带初值数据进入 DATA，常量字符串和常量对象进入 RODATA。
- 语句：复合语句、空语句、表达式语句、变量声明、`if/else`、`while`、`for`、`break`、`continue`、`return`。`for` 支持可选初始化/条件/递增表达式，初始化可为单个变量声明或表达式。
- 表达式：整数/字符/布尔/字符串字面量、变量、函数调用、数组下标、结构体 `.`/`->`、一元 `+ - ! ~ * & ++ --`、后缀 `++ --`、乘除余、加减、移位、关系、相等、按位与/异或/或、逻辑与/或、赋值及对应复合赋值。支持一维固定长度数组及指针加减；短路求值适用于 `&&` 和 `||`。
- 结构体：仅 `struct` POD 数据成员；成员按 LP64 目标自然对齐，结构体大小按最大成员对齐向上补齐；支持局部/全局对象、成员访问和取成员地址。不支持方法、构造/析构、继承、虚函数或位域。
- 字符串：窄字符串字面量按 UTF-8 字节存储并自动追加 `\0`；支持常用 C 转义（`\\`、`\"`、`\n`、`\r`、`\t`、`\0`、十六进制/八进制转义）。字符串对象不可修改。字符字面量限单字节值。
- 注释：`//` 行注释和 `/* ... */` 块注释；未闭合块注释是词法错误。
- 预处理：对象式宏 `#define NAME replacement-token-sequence`；`#include "file"` 和 `#include <file>`，当前文件目录优先于 `-I` 目录；支持 `#ifdef`、`#ifndef`、`#else`、`#endif` 以兼容头文件保护，支持 `#undef`。宏展开限制递归深度并保留展开前后源位置。

### 3.2 明确不支持

- 浮点类型/运算、复数、向量、SIMD、变长数组、动态数组、位域、联合体、枚举。
- 类、访问控制、成员函数、构造/析构、继承、多态、虚函数、模板、命名空间、运算符重载、函数重载、默认参数、引用、异常、RTTI、`new/delete`、成员指针。
- 函数指针、可变参数、递归宏、函数式宏、`#if/#elif` 表达式、`#pragma`、`#error`、`#line`、`#include_next`、系统标准库和 STL。
- C++ 名字修饰、跨翻译单元链接、动态链接、SDK 外部符号解析、PIC、栈保护、调试信息、C++ 静态构造器、标准库/CRT 自动链接。
- 内联汇编。硬件/内核接口必须通过编译器提供的受控内建函数调用。

## 4. NUC 输出与当前加载器适配

### 4.1 固定布局

`nucgen` 以小端序序列化以下区域，顺序和尺寸不可偏离 NUC 规范 §14：

1. Header：48 字节，`magic="NUC!"`、`abi_version=0x0100`、`compile_flag` 至少置 64-bit 与 static（`0x0005`）、reserved 16 字节全零。
2. Segment Table：每段 32 字节，最多 16 段；只输出非空段，BSS 的 `file_offset=0`。虚拟地址页对齐，TEXT 从 `0x400000` 开始，后续段按页布置。
3. Symbol Table：每项 24 字节；`shndx` 是段表索引，`value` 是段内偏移；仅 LOCAL/GLOBAL，不生成 EXTERN。
4. Relocation Table：每项 16 字节；`rel_offset` 紧随符号表，`r_offset` 为被修补字段的虚拟地址。只用类型 1 (`R_X86_64_32`) 和类型 2 (`R_X86_64_PC32`)。
5. String Table：首字节为 `\0`，符号名以 `\0` 结束，`strtab_offset` 指向表首。
6. 段数据：TEXT/RODATA/DATA 文件偏移按 4 KiB 对齐，段间零填充；BSS 不占文件空间。所有偏移、长度和表计数写入前做整数溢出及范围检查。

建议只在实际使用时产生 TEXT、RODATA、DATA、BSS 段；段权限按规范填 TEXT `0x0D`、RODATA `0x09`、DATA/BSS `0x0B`。`entry_point` 是 `_start` 在 TEXT 内的字节偏移，不是文件偏移或 VA。编译结果必须自校验：魔数、版本、段数、表边界、字符串偏移、文件数据范围、符号段索引、重定位类型/位置和入口均符合规范。

### 4.2 代码布局与重定位

首版使用固定低地址静态映像，所有段位于 4 GiB 以下。`nucgen` 先完成函数/对象布局，再为内部地址生成 fixup：函数调用和 PC 相对地址使用 `R_X86_64_PC32`，可由 32 位立即数表示的段内绝对地址使用 `R_X86_64_32`。重定位符号必须是本映像内符号。严禁生成规范预留但 V1.0 加载器不支持的 `R_X86_64_64`（类型 3）。

为避免错误链接，`sema` 必须拒绝未定义函数/全局变量和不支持的跨单元依赖。静态 compile flag 与 EXTERN 符号不能同时出现。

### 4.3 当前内核行为边界

`kernel/nuc.c` 当前按 48/32/24/16 字节结构读取表，校验 ABI 主版本为 1，支持类型 1/2 的镜像内重定位，并按 `text.vaddr + entry_point` 启动；它没有 NUC 外部 SDK 符号解析。因此 M7 首版必须是自包含静态镜像。

还需注意：当前加载器对 TEXT 以外的非 BSS 段统一按可写、不可执行映射，TEXT 也同时带写权限；它没有完整执行规范段 flags 的 W^X 校验。`nucgen` 仍输出规范 flags，但不得把加载器尚未实施的只读保护当作安全保证。Header 的 `stack_size` 按要求写规范默认值 8 MiB；当前 `kernel/nuc.c` 实际将用户栈限制在 64–256 KiB。

## 5. 系统调用与最小运行时

### 5.1 syscall ABI

内建 syscall 以 `syscall` 指令发起，返回值在 `rax`：`SYS_YIELD=1`、`SYS_WRITE=2`、`SYS_EXIT=60`。`novus_write` 设置 `rax=2`、`rdi=1`、`rsi=用户缓冲区地址`、`rdx=字节数`。当前 `kernel/trap.c` 实际读取 `rsi`/`rdx`，单次输出长度截到 128 字节，并返回成功写入长度；cppcc 当前不自动分块长写入。

寄存器约定需区分普通函数 ABI 与 syscall ABI：普通 x86-64 函数前六个整数参数使用 `rdi/rsi/rdx/rcx/r8/r9`；但执行 `syscall` 会由 CPU 覆盖 `rcx` 保存返回 RIP，当前内核对第 4 个及后续 syscall 参数使用 `r10/r8/r9`。因此用户需求中“syscall 参数包含 rcx”与硬件及当前 trap 实现不兼容。首版 write 只有三个参数，不受影响；任何多于三个参数的 syscall wrapper 必须按 `r10` 起始的实际内核 ABI 实现，不能把 `rcx` 当 syscall 参数寄存器。

应用能力接口当前由 `apps/src/sys.rs` 的裸 syscall wrapper 和 `apps/src/ipc.rs`、`sinfo.rs`、`wm.rs` 提供；M8 Rust 应用通过 IPC 使用 `sys` 与 `nwm` 服务。`lib/novuslib-rs/` 是内核侧 `no_std` 算法/FFI 库，不是现成的用户态 C++ SDK，也不应链接进 cppcc 产物。

### 5.2 编译器内建接口

提供编译器随附头文件 `<novus/syscall.h>`，首版公开窄接口：

```cpp
long novus_write(const char* bytes, unsigned long length);
void novus_yield();
void novus_exit(int status);
```

这些符号由 `sema` 作为保留内建函数识别，x86_64 后端直接生成 syscall 序列：参数按普通函数 ABI 从调用者传入，发出 `0F 05`。当前 `novus_write` 只发起一次 syscall；内核单次 `SYS_WRITE` 最多处理 128 字节，自动分块尚未实现。`novus_exit` 发起 60 号调用，若意外返回则陷入不可达循环；`novus_yield` 发起 1 号调用。

这些 intrinsic 不产生外部 NUC 符号或 EXTERN 重定位。头文件由 cppcc 自带 include 目录提供，调用者不必链接 libc、libstdc++、Rust 应用库或独立运行时。

### 5.3 `_start` 与 `main`

编译器合成入口 `_start`，要求存在 `int main()` 或 `int main(void)`：

1. NUC Header 的 `entry_point` 指向 TEXT 中合成 `_start`。
2. 当前 loader 在初始栈顶放入返回 trampoline 地址，并以 `rsp = stack_top - 8` 进入程序；该位置满足函数入口 `rsp % 16 == 8`。
3. `_start` 调整栈对齐后按普通 x86-64 调用约定调用 `main`。
4. `main` 返回后 `_start` 恢复栈，将 `rax` 返回值放入 `rdi`，发起 `SYS_EXIT=60`；若 syscall 意外返回则执行 `hlt`。loader 提供的 return trampoline 不作为正常退出路径。

当前后端保留 `_start` 名称并合成入口；用户定义同名函数会被拒绝。`main` 若显式调用 `novus_exit` 则不会返回。

## 6. 三步实施里程碑

### 第一步：前端

- 建立 `user/cppcc/` Rust 2021 Cargo binary crate、CLI、源位置诊断和模块骨架；主机 native target 编译。
- 实现本方案所列预处理、词法、递归下降解析、AST、作用域与类型语义检查。
- 先用 AST dump 和 diagnostics 验收；至少覆盖成功样例、错误位置、宏/include/头文件保护、作用域/类型错误及所有明确拒绝的语法。
- 本阶段不生成可执行文件。

### 第二步：x86_64 codegen + nucgen

- 直接从语义标注 AST 生成 x86_64 指令，覆盖基本块标签、条件/循环跳转、地址计算、调用和 syscall intrinsic。
- 实现简单栈槽式 x86-64 后端、函数/全局布局、fixup 与 NUC serializer/自校验器。
- 生成静态 `hello.nuc`：`_start → main → novus_write("Hello, Novus!\n") → 返回 0`；再覆盖循环、分支、函数调用、全局/局部变量、结构体和数组。
- 本阶段验收二进制表字段、段数据、入口、符号与重定位公式；不改 NUC 规范或内核加载器。

### 第三步：集成验证

- 在 Makefile/`tools/tools.mk` 增加主机端 cppcc 构建与示例 NUC 目标，输出集中放在 `build/` 或 `out/`，不把宿主产物写进应用 crate。
- 使用现有 QEMU/ISO 启动流程加载并运行生成的 `.nuc`；通过串口确认 `[USER] Hello, Novus!` 和正常退出，验证入口、栈、重定位、syscall 与 trampoline。
- 分别在 Windows 与 Linux native host 构建 cppcc；运行同一测试样例并比较 NUC 字节，确保确定性输出。对 malformed input 和篡改表项验证编译器自检/内核拒绝路径。
- 记录后续扩展需求（更多 syscall、跨文件链接、GUI/IPC 头文件）但不纳入 M7 完成条件。

## 7. 测试策略

- **前端单元测试**：预处理宏展开顺序、include 搜索/循环、头文件保护、token 边界、运算符优先级、语法恢复、作用域遮蔽、类型兼容、数组/指针/结构体布局及错误诊断位置。
- **codegen 测试**：表达式求值、短路逻辑、分支/循环标签、break/continue 目标、返回路径、调用参数/对齐、栈槽和 syscall lowering。
- **NUC 格式测试**：48/32/24/16 字节精确长度；字段小端序；abi `0x0100`、flags、reserved；段权限/对齐/文件偏移；String Table 首 NUL；BSS `file_offset=0`；入口公式；PC32/ABS32 公式；静态镜像无 EXTERN；拒绝类型 3；截断/溢出边界。
- **端到端测试**：构建输出可被 loader 加载；`Hello, Novus!` smoke test；`main` 返回码由 `_start` 传给 SYS_EXIT；长于 128 字节的写入目前不自动分块，按限制验证单次写入行为。
- **负向/回归测试**：未定义符号、错误原型、无 main、重复定义、非法重定位、越界字符串/段、错误 ABI 主版本，以及不支持的语法均应失败且不留下有效的半成品 `.nuc`。

## 8. M7 完成判据

一个 C++98 子集样例能经完整前端与 AST 驱动的 x86_64 codegen 编译为符合冻结规范的确定性 NUC；该文件被现有 NUC loader 加载后，`_start` 正确调用 `main`，经 SYS_WRITE 输出内容，并以 SYS_EXIT 正常结束。编译器本身可在 Windows/Linux 主机端构建，且没有 LLVM、大型依赖或 NUC ABI 变更。