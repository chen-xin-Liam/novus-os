# cppcc

`cppcc` 是 Novus OS 的主机端 C++11 编译库与工具集，使用 Rust 编写。前端完成预处理、词法、语法和语义分析后，生成目标无关对象模型；后端可输出 NUC V1.0 镜像、Windows x64 PE32+ 可执行或 Linux x64 ELF64 动态可执行。Rust 公共 API 与 C ABI 静态库均可供宿主复用。完整 C++11（STL、异常、RTTI、模板元编程）是分阶段终态，当前支持范围见 `docs/C++11 支持矩阵.md`。

## 构建

主机需安装 Rust stable/Cargo。独立构建：

```sh
cargo build --release
```

在仓库根目录运行 `make cppcc-tools`，生成 `build/tools/cppcc`、`build/tools/cppcc_to_win64`、`build/tools/cppcc_to_linux` 与 `build/tools/libcppcc_cabi.a`。`CONFIG_USER_CPPCC_MULTITARGET=y` 时，顶层 `make` 也会构建这些产物。构建 smoke 样例：

```sh
cargo run --release -- examples/hello.cc -o hello.nuc
# 或在仓库根目录：
make cppcc-demo
```

`make cppcc-demo` 的产物为 `out/cppcc/hello.nuc`。`make run` 会将样例复制到 `esp/APPS/DEMO.NUC` 并调用 `scripts/run.sh`。

多目标工具：

```sh
cppcc_to_win64 examples/hello.cc -o hello.exe
cppcc_to_linux examples/hello.cc -o hello.elf
chmod +x hello.elf && ./hello.elf
```

C 宿主可包含 `user/cppcc-cabi/include/cppcc.h` 并链接 `libcppcc_cabi.a`，调用 `cppcc_compile_file()` 或 `cppcc_compile_source()`。

## 用法

```sh
cppcc hello.cc -o hello.nuc
cppcc -I include hello.cc -o hello.nuc
cppcc --check hello.cc
cppcc --dump-ast hello.cc --check
```

默认内置头文件 `<novus/syscall.h>` 声明：

```cpp
long novus_write(const char* bytes, unsigned long length);
void novus_yield();
void novus_exit(int status);
```

示例 `examples/hello.cc` 使用 `novus_write` 调用 `SYS_WRITE=2` 输出文本。`novus_write` 当前发起单次 syscall；内核将一次写入截断到 128 字节，较长文本需要调用方自行分段。

## 支持范围

- 单翻译单元；非成员函数声明/定义、全局与局部变量、`struct` POD 字段、指针和一维定长数组。
- `void/bool/char/short/int/long/long long` 及受限的 `signed/unsigned/const`；x86_64 LP64 布局。
- 代码块、表达式语句、`if/else`、`while`、`for`、`break`、`continue`、`return`。
- 整数与字符/字符串常量；一元、后缀、算术、移位、关系、相等、位运算、逻辑、三目、赋值和复合赋值；函数调用、数组下标、`.`/`->` 成员访问。
- 对象式 `#define`、本地 `#include`、`#pragma once`、`#ifdef/#ifndef/#else/#endif`、`#undef`、行注释和块注释。
- syscall builtin `novus_write/yield/exit`；合成入口要求 `int main()` 或 `int main(void)`。

编译器输出静态 x86_64 NUC，包含 TEXT/可选 RODATA、DATA、BSS 段，最多六个函数参数，支持直接函数调用和镜像内 `R_X86_64_32`/`R_X86_64_PC32` 重定位。每次生成后会校验 NUC 表顺序、字段范围、段权限、入口、符号和重定位。

## 已知限制

- 尚未达到完整 C++11：类、继承、虚函数、模板元编程、异常、RTTI、`new/delete`、STL、浮点、联合体、位域、多维数组、可变参数和函数指针仍在分阶段路线图中。
- 已支持的 C++11 骨架：`namespace`、`using` 类型别名、`enum class`、`static_assert`、`auto`、引用、`nullptr`、范围 for（含本次修复的数组类型与游标递增）。
- Win64 后端把内建输出映射到 kernel32 导入函数；`puts`/`putchar` 在该目标上给出明确诊断。Linux 后端生成 PT_INTERP/dynamic 结构，当前不解析外部 PLT 符号。
- 仍为单翻译单元；NUC 目标最多六个参数，Win64 目标最多四个参数；无跨单元链接、PIC、栈保护、完整 C++ 运行时或标准库。
- 不支持局部 `static`、聚合初始化、结构体按值传参/返回、全局非字面量初始化、间接函数调用。全局指针初始化限字符串或全局对象地址。
- NUC 使用固定低地址布局；不生成预留的 `R_X86_64_64`。shell 通用 `./app.nuc` 执行命令尚未实现；当前集成通过 boot-stage 固定读取 `APPS/DEMO.NUC`。
- 当前 `kernel/nuc.c` 按规范映射 TEXT 为可执行、非 BSS 数据为 NX，但现有实现仍给这些页写权限，尚未按段 flags 落实 W^X；header 的 8 MiB 栈值也会被 loader 限制到 64–256 KiB。

错误诊断使用 `文件:行:列: error: 消息`。运行单元与格式测试：

```sh
cargo test
```

测试和 QEMU 实际运行记录见 `docs/cppcc 构建与端到端验证.md`。