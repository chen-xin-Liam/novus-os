# cppcc

`cppcc` 是 Novus OS 的 C++98 子集编译器，使用 Rust 编写，在构建主机运行。它直接将带语义类型标注的 AST 降为 x86_64 原生机器码，再经目标无关的 `CodeSections`（代码/符号/重定位）交由三个后端之一写出：NUC V1.0 `.nuc`、Linux 静态 ELF64、Win64 PE32+。内置函数统一调用 `__plat_write`/`__plat_yield`/`__plat_exit` 平台桩，由 `platform.rs` 按目标填入具体实现，因此三个目标都是可运行产物。

## 构建

主机需安装 Rust stable/Cargo。独立构建：

```sh
cargo build --release
```

在仓库根目录可运行 `make cppcc`，生成 `build/tools/cppcc`（Windows 主机为 `cppcc.exe`）。`CONFIG_USER_CPPCC=y` 时，顶层 `make` 也会构建该工具。构建 smoke 样例：

```sh
cargo run --release -- examples/hello.cc -o hello.nuc
# 或在仓库根目录：
make cppcc-demo
```

`make cppcc-demo` 的产物为 `out/cppcc/hello.nuc`。`make run` 会将样例复制到 `esp/DEMO.NUC` 并调用 `scripts/run.sh`。

## 用法

```sh
cppcc hello.cc -o hello.nuc
cppcc -I include hello.cc -o hello.nuc
cppcc --target elf64 hello.cc -o hello.elf
cppcc --target pe32+ hello.cc -o hello.exe
cppcc --check hello.cc
cppcc --dump-ast hello.cc --check
```

`--target` 取 `nuc`（默认）、`elf64`（别名 `elf`）、`pe32+`（别名 `pe`/`pe64`）。

默认内置头文件 `<novus/syscall.h>` 声明：

```cpp
long novus_write(const char* bytes, unsigned long length);
void novus_yield();
void novus_exit(int status);
```

示例 `examples/hello.cc` 使用 `novus_write` 输出文本。三个目标各自把它落到自己的系统调用/API：NUC 走内核 `SYS_WRITE=2`（单次写入内核截断到 128 字节，长文本需调用方分段），Linux 走 `write(1)`，Windows 走 `GetStdHandle`+`WriteFile`。

## 支持范围

- 单翻译单元；非成员函数声明/定义、全局与局部变量、`struct` POD 字段、指针和一维定长数组。
- `void/bool/char/short/int/long/long long` 及受限的 `signed/unsigned/const`；x86_64 LP64 布局。
- 代码块、表达式语句、`if/else`、`while`、`for`、`break`、`continue`、`return`。
- 整数与字符/字符串常量；一元、后缀、算术、移位、关系、相等、位运算、逻辑、三目、赋值和复合赋值；函数调用、数组下标、`.`/`->` 成员访问。
- 对象式 `#define`、本地 `#include`、`#pragma once`、`#ifdef/#ifndef/#else/#endif`、`#undef`、行注释和块注释。
- syscall builtin `novus_write/yield/exit`；合成入口要求 `int main()` 或 `int main(void)`。

编译器输出静态 x86_64 镜像，最多六个函数参数、只做直接函数调用和镜像内 `R_X86_64_32`/`R_X86_64_PC32` 重定位：

- NUC：TEXT/可选 RODATA/DATA/BSS 段；写出前校验表顺序、字段范围、段权限、入口、符号和重定位。
- ELF64：静态 `ET_EXEC`，`0x400000` 起，3 个 `PT_LOAD`（代码 R|X、只读 R、数据 R|W 且 `memsz` 含 BSS），无 `PT_INTERP`/`PT_DYNAMIC`（动态链接后端未建）。
- PE32+：COFF 头 + `PE32+` 可选头（`0x20B`）、5 段（`.text`/`.rdata`/`.data`/`.bss`/`.idata`）、`.idata` 里一条 `kernel32.dll` 导入目录（`GetStdHandle`/`WriteFile`/`ExitProcess`），固定基址 `0x400000`、不写 `.reloc`、`DllCharacteristics=0`。

## 已知限制

- 不是完整 C++98：无类、继承、虚函数、模板、命名空间、函数重载、引用、异常、RTTI、`new/delete`、STL、浮点、枚举、联合体、位域、变长/多维数组、可变参数和函数指针。
- 仅支持单个变量声明器；无逗号表达式、C/C++ 显式 cast、`sizeof`、局部结构体定义和聚合初始化。结构体定义需在顶层，之后可声明局部/全局 POD 对象。
- 预处理不支持函数式宏、`#if/#elif` 表达式、`#pragma`（除 `once`）及系统头文件。
- 仅支持单翻译单元、最多六个参数；无动态链接、跨单元链接、SDK 外部符号、PIC、栈保护、C++ 运行时或标准库。
- 不支持局部 `static`、聚合初始化、结构体按值传参/返回、全局非字面量初始化、间接函数调用。全局指针初始化限字符串或全局对象地址。
- NUC 使用固定低地址布局；不生成预留的 `R_X86_64_64`。shell 通用 `./app.nuc` 执行命令尚未实现；当前集成通过 boot-stage 固定读取 `APPS/DEMO.NUC`。
- 当前 `kernel/nuc.c` 按规范映射 TEXT 为可执行、非 BSS 数据为 NX，但现有实现仍给这些页写权限，尚未按段 flags 落实 W^X；header 的 8 MiB 栈值也会被 loader 限制到 64–256 KiB。

错误诊断使用 `文件:行:列: error: 消息`。运行单元与格式测试：

```sh
cargo test
```

测试和 QEMU 实际运行记录见 `docs/cppcc 构建与端到端验证.md`。