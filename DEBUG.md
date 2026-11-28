# Novus OS 蓝屏停止码参考（DEBUG）

> 适用版本：当前工作树（`kernel/bsod.c` / `kernel/bsod.h`）
> 停止界面：`kernel/bsod.c`。异常入口：`kernel/trap.c: trap_exception()`。
> 本文与源码中的 `BSOD_BC_*` 常量一一对应；新增停止码时必须同步修改两处。

---

## 1. 停止码对应表

| 停止码 | 名称 | 触发条件 | 含义 | 常见原因 | 排查建议 |
|---|---|---|---|---|---|
| `0x0000001E` | `KMODE_EXCEPTION_NOT_HANDLED` | 内核态异常向量未落入其他专用映射（默认分支） | 内核态出现未处理异常 | 空指针解引用、越界访问、非法指令（#UD=6）、通用保护错误（#GP=13） | 看 `RIP` 定位指令；对照 `ERROR_CODE`/通用寄存器确认参数；查日志尾部最后一条 `[TRAP]` |
| `0x00000050` | `PAGE_FAULT_IN_NONPAGED_AREA` | x86 异常向量 14（#PF），ring 0 | 访问了内核无法映射的线性地址 | 释放后使用、错误指针、页表被破坏、用户指针未走 `user_copy()` | `CR2` 是出错地址，`RIP` 是出错指令；若 CR2 接近 0 通常为空指针 |
| `0x0000000A` | `IRQL_NOT_LESS_OR_EQUAL` | 向量 8（#DF）、12（#SS）、17（#AC）、19（#XM） | 在高 IRQL/异常上下文中发生非法访问或嵌套异常 | 中断上下文调用了会睡眠的函数、栈破坏、 doubling fault 前的首次异常被掩盖 | 先看向量区分双重故障与栈错误；日志中通常还有第一次 `[TRAP]`；检查中断处理路径 |
| `0x00000124` | `WHEA_UNCORRECTABLE_ERROR` | 向量 18（#MC） | CPU 报告不可纠正的机器检查异常 | 硬件故障、内存位翻转、微码问题、错误的 MTRR/PAT 配置 | 结合串口日志的 MCE 信息；在 QEMU 中检查是否注入了机器检查 |
| `0x000000C2` | `BAD_POOL_CALLER` | 内核 panic 着陆点（`novus_rs_panic_hook`，`kernel/pmm.c`） | 内核组件 panic 或内存池耗尽 | Rust 静态库 panic、堆分配失败后继续运行 | 屏幕日志区显示原始 `[PANIC]` 消息；用 COM1 得到完整文本 |
| `0x000000E3` | `MANUAL_STOP` | 键盘手动序列 `Tab×5 + Shift×2` | 人为触发的调试停止画面 | 调试/演示 | 无故障含义；按 `Ctrl+Alt+Esc` 直接重启 |

### 向量 → 停止码速查

| 向量 | x86 名称 | 停止码 |
|---|---|---|
| 6 | #UD invalid opcode | `0x0000001E` |
| 8 | #DF double fault | `0x0000000A` |
| 12 | #SS stack fault | `0x0000000A` |
| 13 | #GP general protection | `0x0000001E` |
| 14 | #PF page fault | `0x00000050` |
| 17 | #AC alignment check | `0x0000000A` |
| 18 | #MC machine check | `0x00000124` |
| 19 | #XM SIMD exception | `0x0000000A` |
| 其他（0–31） | — | `0x0000001E` |

---

## 2. 屏幕字段说明

| 字段 | 来源 | 解读 |
|---|---|---|
| `STOP 0xXXXXXXXX` | 上表映射 | Windows 风格 8 位十六进制停止码 |
| `VECTOR` | `trap_frame_t.vector` | 触发异常的 x86 向量；手动触发为 0 |
| `ERROR_CODE` | `trap_frame_t.errcode` | CPU 提供的错误码，#PF 时 bit0=P、bit1=W/R、bit2=U/S |
| `CR2` | 控制寄存器 CR2 | 缺页异常访问的线性地址；其他异常时仅作参考 |
| `RIP` / `CS` | 陷阱帧 | 出错指令地址与代码段；`CS & 3 == 0` 表示 ring 0 |
| `RSP` / `RFLAGS` / `SS` | 陷阱帧 | 出错时的栈、标志和栈段 |
| `RAX…R15` | 陷阱帧 | 通用寄存器快照，用于还原调用参数 |
| `PID` / `TASK` | `sched_current()` | 故障发生时正在运行的用户任务；内核启动阶段可能为空 |
| `KERNEL LOG (tail)` | `klog_snapshot()`（32 KiB 环形） | 最近日志，按行裁剪到屏幕宽度；超出屏幕的旧行被省略 |

蓝屏画面在关中断状态下直接写入显示表面并一次性呈现，不经过调度器与窗口管理器，因此原有桌面和所有窗口会立即冻结。串口（COM1，115200 8N1）与启动盘日志仍会继续收到 `kputs()` 输出，可用来获取比屏幕更长的历史。

---

## 3. 触发方式

### 自动触发

- ring 0 异常：`kernel/trap.c` 的 `trap_exception()` 判定 `CS & 3 == 0` 后调用 `bsod_fault()`。
- 内核 panic：`kernel/pmm.c` 的 `novus_rs_panic_hook()` 调用 `bsod_manual(0x000000C2)`。
- 用户态程序异常**不会**蓝屏：只终止出错任务，内核继续运行。

### 手动触发（正常运行时）

连续按 **5 次 Tab**，紧接着连续按 **2 次 Shift**（左右 Shift 均可）。要求：

- 两次按键之间不能混入其他按键；混入任意其他按键会重置计数。
- 整个序列必须在约 3 秒内完成，否则由 `bsod_hotkey_tick()`（在 `kernel/sched.c` 每个调度 tick 调用）重置。
- 触发后显示停止码 `0x000000E3`。

### 重启

- 蓝屏状态下：`Ctrl+Alt+Esc`，直接进入硬件复位（跳过磁盘停转，避免关中断时阻塞）。
- 正常运行中：`Ctrl+Alt+Esc` 同样走硬件复位。热键来自定时器中断（IF=0），不能在那里执行常规 `power_reboot()` 的刷盘/停盘流程。
- 蓝屏是全屏独占的，其他按键和鼠标均被忽略。

---

## 4. 实现位置

| 文件 | 作用 |
|---|---|
| `kernel/bsod.h` | 停止码常量、热键扫描码、模块接口 |
| `kernel/bsod.c` | 快照、向量映射、全屏绘制、日志排版、热键状态机、8042 轮询与重启 |
| `kernel/trap.c` | ring 0 异常接入 `bsod_fault()` |
| `kernel/pmm.c` | Rust panic 接入 `bsod_manual()` |
| `graphics/nd_input.c` | PS/2 原始扫描码钩子（Shift/Alt/Ctrl 不会产生普通按键事件，必须在此拦截） |
| `kernel/hid_boot.c` | USB HID 启动协议键盘的热键映射 |
| `kernel/sched.c` | 每 tick 调用 `bsod_hotkey_tick()` 做序列超时 |
| `Makefile` | 把 `kernel/bsod.c` 加入 `SRC` |

> 术语说明：项目文档中 A 核 = BSP（启动守护）、B 核/P 核 = AP（主内核）。当前内核执行体实际仍在 BSP 上运行，本实现按"最快路径"在真实执行内核的那一颗核上接管显示，不改动 SMP 架构。
