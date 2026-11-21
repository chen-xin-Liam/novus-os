# M7 cppcc 实现说明

## 1. 实现架构

`cppcc` 是 host-native Rust 工具，不链接进内核，也不依赖 LLVM 或外部汇编器。当前流水线为：

```text
driver → preprocess → lexer → parser/AST → sema → x86_64 AST codegen
      → CodeSections（目标无关代码/符号/重定位）
      → platform::install（追加该目标的平台桩）
      → nucgen | elf64 | pe32plus
```

后端直接遍历经过语义检查的 AST：表达式已标注类型，符号作用域已验证。`CodeSections` 承担了"目标无关 IR + 符号 + 重定位"这一层的职责，因此仍未建独立的 `ir.rs`：它记录 `text/rodata/data/bss`、符号表与 PC32/ABS32 重定位，是三个后端共用的唯一中间表示。`x86_64.rs` 使用手写指令编码，包含栈帧/局部槽分配、算术与比较、分支标签、直接调用；函数调用遵守 SysV x86_64 参数寄存器与 16 字节栈对齐。

当前三个后端：

| 目标 | 模块 | 产物 | 平台桩实现 |
| --- | --- | --- | --- |
| NUC V1.0 | `nucgen.rs` | `.nuc` | `rax=2/1/60` + `syscall` |
| Linux ELF64 | `elf64.rs` | 静态 `ET_EXEC`，3 个 `PT_LOAD` | `write(1)/sched_yield(24)/exit(60)` |
| Win64 PE32+ | `pe32plus.rs` | `IMAGE_FILE_MACHINE_AMD64`、导入表 + IAT | `GetStdHandle`/`WriteFile`/`ExitProcess` |

内置函数不再自行发系统调用，而是统一调用 `__plat_write`/`__plat_yield`/`__plat_exit` 三个符号，由 `platform.rs` 按目标追加桩代码并登记符号和重定位——这是"目标无关"能成立的关键，也让三个后端都成为可运行路径而非摆设。Rust 侧对外暴露 `cppcc::compile_source(source, target)` 与 `cppcc::emit(unit, target)`（图 1 中的"Rust 编译 API"）。

`preprocess.rs` 保留源文件行列映射并处理受限预处理；`lexer.rs` 生成带位置 token；`parser.rs` 递归下降构造 AST 并在语法错误后恢复；`sema.rs` 收集函数、全局变量、结构体和作用域符号，报告类型/return/void 错误；`nucgen.rs` 序列化并自校验 NUC 文件。

## 2. 语言子集

支持范围以 [`user/cppcc/README.md`](../user/cppcc/README.md) 为准，核心包括：

- 单翻译单元中的函数原型/定义、全局/局部变量、POD `struct`、指针和一维定长数组。
- 整数、字符、UTF-8 窄字符串，局部/全局标量常量初始化。
- 块、表达式、`if/else`、`while`、`for`、`break/continue`、`return`。
- 一元/后缀、算术、比较、位移、位运算、逻辑、三目、赋值/复合赋值、直接调用、下标和成员访问。
- 对象式 `#define`、本地 include、`#pragma once` 和头文件保护条件、注释剥离。
- 内置 `novus_write/yield/exit`，不生成外部 SDK 符号。

不支持项包括完整 C++ 面向对象/模板/异常/标准库、浮点、函数指针、间接调用、聚合初始化、结构体按值传递、局部静态、跨翻译单元和动态链接。函数参数/调用最多六个；全局指针初始化仅限字符串或已定义全局对象地址。`novus_write` 单次最多受内核 128 字节 syscall 上限约束，不自动分块。

## 3. NUC 对应关系

`nucgen` 严格按冻结规范顺序写出 Header → Segment Table → Symbol Table → Relocation Table → String Table → 段数据，不更改 NUC ABI：

| 字段/段 | 输出规则 |
|---|---|
| Header | 48 字节；magic `NUC!`；ABI `0x0100`；compile flag `0x0005`（64-bit + static）；reserved 16 字节全零；stack size `0x00800000`；entry_point 为 `_start` 的 TEXT 内偏移。 |
| Segment Table | 32 字节/项；最多 16 段；TEXT `0x0D`、RODATA `0x09`、DATA/BSS `0x0B`，均置 USER bit；段 VA 页对齐。 |
| Symbol Table | 24 字节/项；本地/全局函数与对象引用 String Table；value 是段内偏移，shndx 是段表索引。 |
| Relocation Table | 16 字节/项；PC32 (2) 用于直接函数调用，ABS32 (1) 用于低地址符号地址；目标位置记为虚拟地址；不生成类型 3 或 EXTERN。 |
| String Table | 首字节 NUL，符号名以 NUL 结尾；紧随重定位表。 |
| 段数据 | TEXT/RODATA/DATA 的文件偏移按 4 KiB 对齐；BSS 的 file_offset 为 0 且不写载荷。 |

固定 VA 为 TEXT `0x400000`、RODATA `0x600000`、DATA `0x700000`、BSS `0x800000`。入口按 `text.vaddr + entry_point` 计算。代码生成后会检查表边界、segment flags、符号/重定位索引、relocation place、文件范围与段虚拟地址不重叠。格式细节仍以 [`NUC V1.0 规范`](NUC%20可执行二进制格式规范%20V1.0.md) 为唯一基准。

生成器始终写规范段权限，但当前 `kernel/nuc.c` 映射代码页时也保留写权限、映射其他非 BSS 段时同样可写；实际 loader 尚未落实段 flags 所表达的 W^X/RODATA 保护。Header 按规范写 8 MiB stack_size，loader 当前会 clamp 到 64–256 KiB。

合成 `_start` 先把 `rsp` 对齐到 16 字节（`and rsp, -16`，Win64 与 SysV 都要求调用点 16 字节对齐，这也是 PE 目标能在 Windows 上跑起来的前提之一），`call main` 后把返回值移到 `rdi`，再 `call __plat_exit`。三个 builtin 都只是"拼参数 + 调平台桩"：`novus_write` 把 `buf/len` 放进 `rdi/rsi` 后调 `__plat_write`，`puts` 在内联 strlen 后分两次调用（正文 + 栈上换行字节），`putchar` 在栈上放单字节后调用。NUC 桩里 `write` 用 `rax=2, rdi=1, rsi=buffer, rdx=len`，`yield` 用 1，`exit` 用 60。

## 4. 加载与运行

boot-stage `stage3.c` 从 ESP 固定读取 `APPS/DEMO.NUC` 到 reserved pages，写入 shared memory；`kernel/kmain.c` 在 scheduler 初始化后调用 `nuc_load`。构建集成见 [`cppcc 构建与端到端验证`](cppcc%20构建与端到端验证.md)。该路径用于 M7 smoke 程序；通用 shell `./app.nuc` exec、文件系统中任意 NUC 的动态启动仍未实现。

## 5. 验证状态

测试代码覆盖冻结 Header/段表 flags/表偏移、符号和两种重定位、Hello syscall 程序，以及 `for`/`if`/函数调用样例。错误路径测试覆盖非法语法和行列诊断。

2026-11-21 三后端的实机结果（同一份 `examples/hello.cc`）：

| 目标 | 验证方式 | 结果 |
| --- | --- | --- |
| NUC | 内核 `CONFIG_USER_CPPCC` 启动 smoke（ESP 根目录 `DEMO.NUC`） | 通过，串口 `[USER] Hello, Novus!`，demo 任务退出码 0 |
| Linux ELF64 | 在 Ubuntu x86_64 上 `chmod +x` 直接执行 | 通过，输出 `Hello, Novus!`，退出码 0 |
| Win64 PE32+ | 在 Windows 11 x86_64 上直接运行（另用 `LoadLibraryEx` 验证镜像可被内核映射） | 通过，输出 `Hello, Novus!`，退出码 0；`return 42` 样例退出码 42 |

PE 目标曾长期被 Windows 以 `ERROR_BAD_EXE_FORMAT (193)` 拒绝，根因有两条，都已修掉：段表里 `VirtualSize == 0` 的空段（`.data`/`.bss` 在没有数据时）会让加载器直接判格式非法；`_start` 的 `sub rsp, 8` 使调用点未对齐 16 字节，导致 `ExitProcess` 与 `WriteFile` 在 x64 上异常。写输出也从 `WriteConsoleA` 换成 `WriteFile`，这样控制台句柄与重定向管道都能写。