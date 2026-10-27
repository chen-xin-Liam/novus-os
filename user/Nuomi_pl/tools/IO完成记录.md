# Nuomi 编译器 I/O 支持 —— 完成记录（Route-A M4）

> 2026-10-21 **已完成**。此前卡住的「用户函数调用作参数」经 unicorn 模拟器
> 验证**编译器本来就是对的** —— 是手搓模拟器 x86sim.py 的 bug，与产物无关。
> 最终验证工具：`tools/nucrun.py`（unicorn 2.1.4，QEMU CPU 内核）。

## ✅ 已完成

### 1. `include/novus_syscall.h` —— 可配置 syscall 号表
号从 `novus-live2.iso` 内核镜像**逆向推测**（`NUOMI_SYSCALL_SPEC_CONFIRMED 0`）。
等官方规范公布后**只改这一个文件**，编译器零改动。

已确认证据（内核二进制里挖到的）：
- syscall 分发器存在（`[TRAP] unknown syscall`）
- 服务名表连续 8 项：`yield/write/getpid/ipc_send/ipc_recv/ipc_lookup/ipc_stats/exit`
- 内部函数名带 `sys_` 前缀（`[TRAP] sys_write: user buffer ended early`）
- 入口是**中断门**（trap 打印含 `cs=`，只有中断/异常才有）→ 用 `int 0x80`

### 2. 编译器 I/O：`print` / `println` / `print-int`
- `cg_expr` 新增三个 I/O 原语
- **统一 syscall 跳板** `__nuomi_syscall_trampoline`（32 字节，`int 0x80; ret` + nop 填充），
  所有 syscall 站点都 `call` 它。真机：跳板 → 内核；宿主：检测到跳板即提示需 novus
- **itoa 机器码内嵌**：`NUOMI_ITOA_CODE[178]`（`tools/itoa_gen.py` 生成），
  签名 `int64 __nuomi_itoa(int64 v, char* buf) -> rax=文本地址, rdx=长度`
- **BSS 段**：`NUOMI_BSS_VADDR 0x600000`，64 字节，给 itoa 当可写缓冲区
- **入口改为执行所有顶层表达式**（原来只执行最后一个，多行输出会丢）
  ⚠️ 副作用：单独 `(define ...)` 无 main 时仍报错（与旧行为一致）

### 3. 端到端验证（tools/nucrun.py，unicorn 模拟 novus 内核）
```
=== Nuomi 编译模式 I/O ===
整数:42 / 负数:-17 / 零:0 / 大数:1234567890
表达式:7          ← (+ 3 4) 求值正确
字符串:hello       ← RODATA 字符串
混合:1two3
let:42           ← let 内 (* x y) 正确
fact(10) =3628800  ← 递归用户函数作参数 ✅
sum 0..99 =4950    ← 带循环的用户函数 ✅
```

## 🐛 已修的真 bug（都值得记住）

编译期修复：
1. **x86 ModRM `mod=01` 才是 disp8**，`mod=10` 是 disp32。曾把 `[rbp-8]` 写成 mod=10
   却只给 1 字节位移 → CPU 把后续字节当 disp32 读 → 整条指令错位 → 段错误
2. **REX.B 扩展的是 ModRM.rm 字段**。`mov rsi, rdx` 不需要 REX.B，误加 `49`
   会把 rm 变成 r14 → rsi 取垃圾、write 的 buf 变成 0
3. **print-int 的返回值覆盖**：itoa 返回 `rax=地址, rdx=长度`，曾用 `mov rdx,[r10]`
   覆盖长度字段（字符串头部的 len 是字符串对象用的，itoa 输出是裸文本，没有头部）

验证器（x86sim.py，已删除）里的坑，供写模拟器时参考：
- opcode `01/29`（add/sub r/m64,r64）是 **rm=目标、reg=源**（写反则 `(+ 3 4)`=4）
- x86 寄存器编号 `rax=0 rcx=1 rdx=2 rbx=3 rsp=4 rbp=5 rsi=6 rdi=7`，
  rbp/rsp 必须建模成真寄存器，否则 `[rbp-8]` 读写与 push/pop 位置错位
- ModRM `mod=01` 时 **disp8 在 +3、imm 在 +4**；`mod=10` 是 disp32

## 🛠 工具

| 文件 | 用途 |
|------|------|
| `tools/nucrun.py` | **主力验证器**：unicorn 模拟执行 .nuc，`UC_HOOK_INTR` 截获 `int 0x80` 按号表模拟 write/exit。依赖：`pip install unicorn` |
| `tools/itoa_gen.py` | 生成 NUOMI_ITOA_CODE + 自测（改 itoa 时用） |
| ~~x86sim.py / nucverify.py / trace_nuc.py~~ | **已删除**：手搓模拟器，多 bug 难调，被 unicorn 方案取代 |

用法：
```bash
python tools/nucrun.py io.nuc          # 跑并打印 write 输出
python tools/nucrun.py -v io.nuc       # 段表/符号/重定位/syscall 明细
```

## 宿主 `nucutil run` 的 syscall 模拟结论

试过 3 种方案全部失败：`int3` 异常（TCC 编译的程序里 CRT 吞掉，不触发）、
`ud2` 异常（同样崩）、跳板替换成「寄存器搬运 + call 宿主桩」（独立测试也崩）。
**当前行为是正确且友好的**：检测到 syscall 跳板就明确提示「需在 novus 内核上运行」，
宿主仅执行纯计算部分。真机验证用 qemu + novus 或 tools/nucrun.py。
