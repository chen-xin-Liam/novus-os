> 英文版本：[docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md)

# Novus OS 架构文档（ARCHITECTURE）

> 版本：v1.0（基于 2026-11-27 工作树）
> 配套文档：[ROADMAP.md](ROADMAP.md)（状态与规划）、[README.md](README.md)（冻结规范）、[SECURITY.md](SECURITY.md)（安全边界）、[CONTRIBUTING.md](CONTRIBUTING.md)（开发约定）
>
> 本文描述**当前源码的真实架构**：分层、依赖方向、数据流、ABI 边界与关键常量。图中出现的数字均可在源码中查到；规范与代码冲突时以 `CONTRIBUTING.md:7` 的优先级为准。
> 图表使用 Mermaid，可在支持 Mermaid 的 Markdown 阅读器中直接渲染。

---

## 目录

- [1. 设计原则与总体分层](#1-设计原则与总体分层)
- [2. 启动架构](#2-启动架构)
- [3. 内核架构](#3-内核架构)
- [4. 文件系统与存储架构](#4-文件系统与存储架构)
- [5. 图形、窗口与合成架构](#5-图形窗口与合成架构)
- [6. 网络与安全架构](#6-网络与安全架构)
- [7. 用户态架构](#7-用户态架构)
- [8. 构建与镜像架构](#8-构建与镜像架构)
- [9. 端到端数据流](#9-端到端数据流)
- [10. 架构约束速查表](#10-架构约束速查表)

---

## 1. 设计原则与总体分层

### 1.1 硬约束

1. **全部自研**：不引入外部开源内核/库；Unifont 字形数据是已记录例外。
2. **单向依赖**：层与层之间只允许自上而下调用；禁止反向调用与循环依赖。
3. **冻结 ABI**：NUC / NKM / NUP / INS / IPC / Debug 接口的字段与版本冻结，变更须升版本并走评审。
4. **一根 VFS**：系统调用只看到一个 `/`；挂载表在路径前缀切换后端，不形成多棵独立根树；目录中没有 `..` 条目。
5. **用户/内核隔离**：NUC 靠 ring 3 页表隔离（不签名），NKM 是 Ring0 模块（必须 Ed25519 签名）。
6. **故障降级**：用户态异常只终止该进程；多数内核自检失败降级为 WARN 而不是 panic。

### 1.2 总体分层图

```mermaid
flowchart TB
    subgraph APP["用户态（ring 3）"]
        direction TB
        A1["内置应用<br/>fileman / taskman / settings<br/>netman / session / setup / devmgmt / dwm / nsh"]
        A2["用户态运行时<br/>apps/src: ui wm sys sinfo fs<br/>shell nuomi devmgr install"]
        A3["外壳工具链<br/>cppcc / Nuomi_pl / win32compat / novusd"]
        A1 --> A2
        A2 --> A3
    end

    subgraph ABI["系统调用 ABI（唯一分发表 g_syscalls[]）"]
        S1["进程/调度 1-3, 60<br/>IPC 4-7<br/>文件 8-15, 20-24"]
        S2["电源/会话 16-19<br/>更新/HTTPS/控制台 25-27<br/>DWM 28-37"]
        S3["Win32 40 / PE32 41"]
    end

    subgraph KERN["内核态（ring 0）"]
        direction TB
        K1["调度与 trap<br/>sched.c / trap.c / irq.c / idt.c"]
        K2["内存<br/>pmm / mm / heap / vm / vmm"]
        K3["文件系统<br/>VFS + NovusFS(WAL,Merkle)<br/>FAT / ISO9660 / ext4"]
        K4["设备与驱动<br/>PCI / AHCI / NVMe / IDE / USB<br/>网卡 / HID / 电源"]
        K5["图形与窗口<br/>NovusDisplay / nvgui / nwm<br/>Mini-DWM / gpu2d"]
        K6["网络与安全<br/>IP/UDP/TCP/DNS/DHCP/HTTP<br/>TLS / NKM / novu / CA"]
        K1 --- K2
        K1 --- K3
        K1 --- K4
        K1 --- K5
        K1 --- K6
    end

    subgraph HW["硬件 / 固件"]
        H1["UEFI (GOP / Block I/O / 内存表)"]
        H2["x86_64: LAPIC / APIC / 分页 / syscall"]
        H3["存储 / 网卡 / USB / 输入 / 帧缓冲"]
    end

    APP -->|"syscall 指令"| ABI
    ABI --> KERN
    KERN -->|"驱动访问"| HW
    H1 -->|"引导交接"| KERN
```

### 1.3 目录到层的映射

| 层 | 目录 | 说明 |
|---|---|---|
| 引导 | `arch/x86_64/boot/` | `efi_main`、`stage3`、启动菜单、Merkle 校验、负载暂存 |
| 内核 | `kernel/` | 调度、syscall、内存、驱动、网络、Win32/PE、安装、会话 |
| 文件系统 | `fs/` | VFS、NovusFS、FAT、ISO9660、ext4、挂载配置、rootfs 布局 |
| 安全 | `security/` | NKM 签名、TLS 1.2/1.3、novu 更新与 CA bundle |
| 图形 | `graphics/` | NovusDisplay、nvgui、桌面壳层、字体/IME/主题/壁纸 |
| 窗口 | `wm/novus-wm/` | 内核态合成器 + 协议服务端，`nwm_proto.h` 是协议真源 |
| 用户态 | `apps/` | Rust `no_std` 扁平 NUC 镜像，内嵌进内核 |
| 语言/工具 | `user/`、`tools/` | Nuomi、cppcc、novusd、win32compat；主机端镜像与生成器 |
| 验证 | `scripts/`、`tools/ci.mk` | 自检、QEMU 冒烟、基线比对、本地 CI 入口 |

> 注意：根目录没有 `system/`、`drivers/`、`user/shell/`、`user/micropython/`；驱动在 `kernel/`，shell 在 `apps/src/shell/`。

---

## 2. 启动架构

### 2.1 两阶段 A/B 核启动

A 核（BSP）负责固件交接与完整性自检，完成后休眠；B 核（AP）加载主内核并接管全部运行。`CONFIG_NOVUS_A_CORE_DISABLE=y` 可跳过 A 核自检；SIPI 失败降级单核，不 panic。

```mermaid
sequenceDiagram
    autonumber
    participant FW as UEFI 固件
    participant A as A 核 / BSP (boot)
    participant SHM as 共享内存 8 KiB @ 0x7E000
    participant B as B 核 / AP (kernel)
    participant K as 主内核 kmain

    FW->>A: efi_main 交出控制权
    A->>A: 串口/控制台、内存扫描 (<2 GiB 拒绝)
    A->>A: stage3_prepare: 块设备探测 + ESP 打开
    A->>SHM: 写入硬件信息 / 自检状态 / Merkle 根
    A->>A: stage3_menu + stage3_load
    Note over A: Merkle 校验系统分区<br/>暂存 live root / ESP.IMG / NUC / 壁纸
    A->>B: 唤醒 AP (SIPI → ap_trampoline.S)
    A->>FW: ExitBootServices
    B->>K: kernel_entry → kmain()
    K->>SHM: shared_mem_verify() (NVSH v1.1)
    K->>K: PMM → 页表/CR3 → heap → 显示 → 设备
    K->>K: NovusFS/VFS → NKM → 更新/CA
    K->>K: LAPIC → IPC → 调度/IRQ → 任务
    K->>A: 发送休眠指令，A 核 park
    K->>K: sched_start() 调度用户任务
```

### 2.2 共享内存交接区

引导阶段与内核阶段通过固定 8 KiB 共享区传递信息；布局冻结，新字段只能追加到 `0x2F0` reserved 尾部。

| 内容 | 作用 |
|---|---|
| 硬件信息 / 内存映射 | 内核据此初始化 PMM 与页表 |
| 自检状态码 / Merkle 根哈希 | 内核只消费引导阶段的校验结论，不重复读盘 |
| `live_root_base/size` | 引导阶段暂存的 NovusFS 镜像，内核转成 memimg 块设备 |
| `esp_img_base/size/install_flags` | 图形安装器载荷与启动模式 |
| `wpkg_img_base/size` | 壁纸包 |
| NUC demo 镜像地址/大小 | `APPS/DEMO.NUC` 固定槽 |
| 4 KiB 日志环 | `klog_init()` 继承，splash 第一阶段即可显示固件日志 |

### 2.3 `kmain` 初始化顺序（硬约束）

```mermaid
flowchart TD
    S0["shared_mem_verify()"] --> S1["pmm_init + selftest<br/>（页表页来自 PMM）"]
    S1 --> S2["mm_init + selftest<br/>低 4 GiB 恒等映射 + physmap<br/>帧缓冲 UC 映射 + wbinvd"]
    S2 --> S3["heap_init + selftest"]
    S3 --> S4["nd_init / boot splash<br/>不可绑定帧缓冲则停机"]
    S4 --> S5["power / ahci / nvme / usb / ide<br/>blkdev 空闲休眠计时"]
    S5 --> S6["devtab_init + touchpad_probe<br/>（网络栈启动前唯一安全扫 PCI 的时机）"]
    S6 --> S7["ipc_init + selftest<br/>（任务收件箱依赖它）"]
    S7 --> S8["novusfs_selftest / 挂载 live root<br/>字体 / 图标加载 / VFS / ext4 selftest"]
    S8 --> S9["mountcfg_apply()<br/>/novusfs/mounts.json"]
    S9 --> S10["Merkle 结论 / NKM / 更新与 CA"]
    S10 --> S11["lapic_init + smp_wake_ap"]
    S11 --> S12["sched / IRQ / 网络 / 语言 / 会话 / NUC 任务 / WM"]
    S12 --> S13["sched_start()（不返回）"]
```

**顺序不可换的三条理由**（源码注释明确）：

1. `pmm_init` 必须先于 `mm_init`：页表页来自 PMM。
2. NovusFS 自检与 live root 挂载必须在 AP 唤醒之前：避免第二核心同时访问块层与 WAL。
3. `devtab_init` 必须在存储驱动之后、网络栈之前：BAR 探测会写 `0xFFFFFFFF`，不能再干扰传输。

---

## 3. 内核架构

### 3.1 内核子系统依赖图

```mermaid
flowchart TB
    CORE["kernel.h / kputs（VGA + COM1 + klog + fbcon 四路镜像）"]

    subgraph BASE["基础层"]
        PMM["pmm.c<br/>物理页位图"]
        MM["mm.c<br/>分页 / UC-WB / 大页"]
        HEAP["heap.c<br/>页粒度 kmalloc"]
        VM["vm.c / vmm.c<br/>每进程地址空间 / COW"]
        IRQ["idt / gdt / irq / trap<br/>中断与异常"]
    end

    subgraph MID["中间层"]
        SCHED["sched.c<br/>抢占调度 / 任务 / ABI 白名单"]
        IPC["ipc.c<br/>端口 / 队列 / 校验和"]
        BLK["blkdev.c + GPT + mountcfg<br/>块抽象"]
        VFS["fs/vfs.c + vfs_mount.c<br/>单根 + 8 挂载项"]
        NUC["nuc.c<br/>NUC 加载器"]
        NKM["security/nkm<br/>签名模块"]
    end

    subgraph HIGH["服务层"]
        SESS["session.c / sysinfo.c / lang.c"]
        NET["net*.c<br/>IP/UDP/TCP/DNS/DHCP/HTTP/NTP/NCSI"]
        WM["wm/novus-wm<br/>窗口管理 / 合成"]
        DWM["gpu / gpu2d / vidmem / crtc / dwmsys"]
        INSTALL["install.c / power.c"]
        WIN32["pe.c / pe32.c / wow32.c / win32.c"]
    end

    CORE --> BASE
    PMM --> MM --> HEAP --> VM
    MM --> IRQ
    IRQ --> SCHED
    SCHED --> IPC
    BLK --> VFS
    VFS --> NUC
    HEAP --> NKM
    SCHED --> SESS
    VFS --> SESS
    VFS --> INSTALL
    IRQ --> NET
    NET --> SESS
    IPC --> WM
    WM --> DWM
    VFS --> WIN32
    SCHED --> WIN32
```

### 3.2 内存架构

| 区域 | 机制 | 关键常量 / 位置 |
|---|---|---|
| 物理页 | `pmm.c` 固定 256 KiB 位图，约覆盖 8 GiB | `kernel/pmm.c` |
| 内核堆 | `kmalloc` 按 4 KiB 向上取整，最多 256 MiB / 256 条记录 | `kernel/heap.c` |
| 堆窗口 | `0xFFFF810000000000` 起 256 MiB，所有 CR3 共享、supervisor-only | `CONTRIBUTING.md:293` |
| 进程空间 | 每任务独立 CR3；COW；只有 TEXT 可执行，其余 NX | `kernel/vm.c`、`nuc.c` |
| 内核栈 | 每任务 64 KiB ring-0 栈 | `kernel/sched.h` |
| NUC 栈 | 栈顶固定 `0xA0000000`，返回跳板 `0x90000000`，栈由头请求并 clamp 到 64 KiB–4 MiB | `CONTRIBUTING.md:295` |
| 共享区 | 8 KiB @ `0x7E000`，NVSH v1.1 | `kernel/shared_mem.h` |

```mermaid
flowchart LR
    U["用户虚拟地址空间<br/>NUC 段: TEXT(RX) / DATA(NX) / BSS(NX)<br/>栈顶 0xA0000000"]
    K["内核映射（所有 CR3 共享）"]
    P["物理内存"]
    U -->|"CR3 页表"| P
    K -->|"低 4 GiB 恒等映射 + physmap"| P
    K2["共享堆窗口<br/>0xFFFF810000000000<br/>256 MiB"] --> K
    SH["引导共享区 0x7E000<br/>8 KiB"] --> P
```

### 3.3 调度与任务

```mermaid
stateDiagram-v2
    [*] --> Created: sched_create_user_ex / nuc_load / pe32_exec
    Created --> Runnable: 建立 CR3 + 内核栈 + trap frame + 收件箱
    Runnable --> Running: 时间片调度（默认 4 tick）
    Running --> Runnable: 时间片耗尽（LAPIC 定时器抢占）
    Running --> Blocked: ipc_recv 阻塞（syscall RIP 回退 2 字节重放）
    Blocked --> Runnable: 消息到达收件箱
    Running --> Exited: SYS_EXIT / 用户态异常
    Exited --> [*]
    note right of Running
      SCHED_MAX_TASKS = 10
      每个任务独立 CR3
      TASK_ABI_WIN32 只放行 SYS_WIN32 与 SYS_EXIT
    end note
```

### 3.4 系统调用 ABI

统一入口：`syscall` 指令 → `kernel/trap.c` → `g_syscalls[]` → 具体 `sys_*`。`rax` 号、`rdi/rsi/rdx/r10` 传参（第 4 参用 `r10`），负返回值表示错误。内核不直接解引用用户指针，按任务 CR3 逐页复制。

```mermaid
flowchart LR
    U["用户态 apps/src/sys.rs"] -->|"rax=号<br/>rdi/rsi/rdx/r10"| G["syscall 入口<br/>vector 0x100"]
    G --> T["syscall_dispatch(trap_frame_t*)<br/>查 g_syscalls[] + ABI 白名单"]
    T --> R1["调度: yield / getpid / exit"]
    T --> R2["IPC: send / recv / lookup / stats"]
    T --> R3["文件: open read write close stat<br/>opendir readdir mkdir unlink rename<br/>mount umount mounts"]
    T --> R4["系统: power_off reboot session setup<br/>update https console"]
    T --> R5["Win32(40) / PE32(41)"]
    T --> R6["DWM(28-37)<br/>vmem / present / vsync / cursor"]
    T -->|"未知编号每号只报一次后拒绝"| E["错误返回"]
```

**编号表**（`kernel/trap.c:48-76`、`kernel/dwmsys.h`、`kernel/win32.h`）：

| 范围 | 调用 |
|---|---|
| 1–7 | `yield` `write` `getpid` `ipc_send` `ipc_recv` `ipc_lookup` `ipc_stats` |
| 8–15 | `open` `read` `fs_write` `close` `stat` `opendir` `readdir` `mkdir` |
| 16–19 | `power_off` `reboot` `session` `setup` |
| 20–24 | `unlink` `rename` `mount` `umount` `mounts` |
| 25–27 | `update` `https` `console` |
| 28–37 | `vmem_alloc/free/map/grant`、`surface_present`、`wait_vsync`、`set_exclusive`、`set_cursor`、`dwm_query`、`set_mode` |
| 40 / 41 | `win32` / `pe32_exec` |
| 60 | `exit` |

> `write`(2) 是串口日志，不是文件写；文件写是 `fs_write`(10)。编号稀疏且冻结，只追加不复用。

---

## 4. 文件系统与存储架构

### 4.1 VFS 分层与挂载

```mermaid
flowchart TB
    SYS["syscall 层<br/>kernel/trap.c（路径 256B / 单次 I/O 4096B / fd 从 3 起）"]
    VFS["VFS 分派层 fs/vfs.h<br/>lookup / creat / mkdir / unlink / read / write<br/>stat / readdir / sync / rename"]
    MOUNT["挂载表 fs/vfs_mount.c<br/>固定 8 项 · 绝对唯一路径 · / 不可卸载<br/>跨挂载 rename 拒绝"]
    ROOT["vfs_root() 单一根 /<br/>不消除 .. · 目录无 .. 条目"]

    SYS --> VFS --> MOUNT
    MOUNT --> ROOT
    ROOT --> N["NovusFS（可写根）<br/>双 superblock + WAL + Merkle"]
    ROOT --> E["ext4（读 + mkfs + mount）"]
    ROOT --> F["FAT12/16/32（只读，8.3 短名）"]
    ROOT --> I["ISO9660 / Joliet（只读，优先 Joliet）"]
    ROOT --> NT["NTFS：仅枚举值，无后端"]

    CFG["/.novusfs/mounts.json<br/>≤4 KiB · ≤8 条 · 白名单 6 条<br/>启动读取，运行时不回写"] -.-> MOUNT
```

**挂载白名单**（`rootfs_layout.h:138-144`）：`/`、`/disk0/1`、`/media/cdrom0/usb0/usb1`。运行时 `mount`/`umount` 不回写 `mounts.json`，否则下次启动 Merkle 校验失败。

### 4.2 NovusFS 磁盘布局与事务链

```mermaid
flowchart LR
    subgraph DISK["NovusFS 卷（magic NOVS, 版本 0x00030000, 块 4 KiB）"]
        SB1["Superblock 主"]
        WAL["WAL 日志区<br/>64 块环形 · CRC-32 · commit"]
        MR["Merkle 区<br/>64B 节点 · 每块 64 节点"]
        BM["块位图"]
        IN["inode 表<br/>1024 项 · 128B/项"]
        DATA["数据区<br/>7 直 + 单/双间接 · 文件名 56B"]
        SB2["Superblock 备"]
    end
    SB1 --- WAL --- MR --- BM --- IN --- DATA --- SB2
```

```mermaid
sequenceDiagram
    participant U as 用户 write()
    participant V as VFS
    participant N as NovusFS
    participant W as WAL
    participant M as Merkle

    U->>V: SYS_FWRITE(fd, buf, len)
    V->>N: write(fs, ino, off, buf, len)
    N->>W: 写事务记录（魔数 + TXID + 块号 + 数据 + CRC）
    W->>N: commit 标记
    N->>N: 落数据块 + 更新 inode
    N->>M: 重算受影响叶子→根的 SHA-256
    M->>N: 新根写双 superblock + 版本计数
    Note over W: 日志满则推进 tail 到完整事务边界<br/>不截断事务
    Note over N: 挂载时 replay；校验失败只 WARN<br/>（非严格崩溃一致性，见 SECURITY.md）
```

**Merkle 覆盖范围**：bitmap + inode + data；**不覆盖** WAL 区与 Merkle 区自身。

### 4.3 块设备栈

```mermaid
flowchart TB
    V["VFS / NovusFS / ext4"]
    B["blkdev 抽象<br/>统一 512B 扇区 · UEFI bounce"]
    B --> A1["AHCI"]
    B --> A2["NVMe"]
    B --> A3["IDE(PIO)"]
    B --> A4["USB xHCI + BOT<br/>CBW/CSW · READ10/WRITE10"]
    B --> A5["UHCI：仅 HID boot protocol"]
    B --> A6["memimg<br/>live root（引导暂存的 ESP 镜像）"]
    GPT["GPT 分区表"] --> B
    MC["mountcfg + devtab"] --> B
```

---

## 5. 图形、窗口与合成架构

### 5.1 图形栈分层

```mermaid
flowchart TB
    subgraph CLIENT["用户态 GUI 应用（apps/）"]
        UI["ui.rs（Novus Fluent 组件）<br/>page/card/nav/row/button/progress"]
        WMCL["wm.rs（v1.x 显示列表 + v2 surface 消息）"]
        UI --> WMCL
    end

    subgraph IPC["内核 IPC"]
        P["端口 nwm<br/>32B 头 · magic 0x4E495043<br/>FNV-1a 校验 · payload ≤128B<br/>队列 8 · 端口表 16"]
    end

    subgraph SERVER["内核态服务端"]
        NWM["Novus-WM 2.0 (0x00020000)<br/>窗口表 10 · Z序 · 焦点 · 输入路由<br/>CLEAR/SHAPE/TEXT → INVALIDATE 提交"]
        DWM["Mini-DWM<br/>vidmem 对象 · CRTC/VSync<br/>surface redirection / flip / 缩略图"]
        HW2D["gpu2d<br/>CPU 软件后端（唯一可用）<br/>厂商后端返回 ENODEV"]
        NWM --> DWM --> HW2D
    end

    subgraph GFX["NovusDisplay 渲染栈"]
        HAL["nd_hal.c<br/>GOP 帧缓冲（最多 4 输出）<br/>EDID · 镜像 · UC 映射"]
        CORE["nd_core.c<br/>矩形/线/圆/圆角/渐变/blend"]
        COMP["离屏画布 nd_canvas<br/>→ nd_present 整帧呈现<br/>光标合成覆盖层 + damage"]
        TXT["字体/Unicode/IME<br/>114,397 字形 · Ctrl+Space 拼音<br/>Ctrl+Shift+U 码点直输"]
        TH["主题 / 双语 / 壁纸<br/>theme.rs + lang_dict.rs + NWPK"]
        HAL --> CORE --> COMP
        TXT --> CORE
        TH --> CORE
    end

    CLIENT -->|"IPC 消息"| IPC --> SERVER
    SERVER -->|"绘制指令"| GFX
    GFX -->|"帧缓冲"| SCREEN["物理屏幕 / 镜像输出"]
```

### 5.2 GUI 一帧的协议时序

每帧必须以 `CLEAR` 开始，随后若干 `SHAPE`/`TEXT`，最后 `INVALIDATE` 一次性提交；缺少 `INVALIDATE` 会出现半帧闪烁。

```mermaid
sequenceDiagram
    participant App as 应用（如 fileman）
    participant W as WM 端口 nwm
    participant I as 内核 IPC 收件箱
    participant C as 合成器

    App->>W: NWM_REQ_CLEAR (0x010D)
    loop ≤120 图元
        App->>W: NWM_REQ_SHAPE (0x010E)
    end
    loop ≤160 段文字（每段 ≤96B）
        App->>W: NWM_REQ_TEXT (0x010F)
    end
    App->>W: NWM_REQ_INVALIDATE (0x0110)
    W->>C: 从显示列表一次性绘制客户区
    C->>C: 离屏合成 → 回扫等待 → 整帧 present
    Note over W: 事件（MOUSE/KEY/FOCUS/RESIZE/CLOSE）<br/>经收件箱推给窗口属主
    W-->>I: NWM_EV_KEY 等
    I-->>App: 事件驱动下一帧
```

### 5.3 Mini-DWM v2 表面重定向

v2 是**追加**消息，不改 v1.x 语义：老客户端继续用显示列表，v2 客户端用 `SYS_VMEM_ALLOC` 分配显存对象，只把句柄与几何交给 WM，像素不跨 IPC 边界。

```mermaid
flowchart LR
    A["v2 客户端"] -->|"SYS_VMEM_ALLOC"| V["vidmem 对象<br/>（内核/用户共享的显存句柄）"]
    A -->|"SURFACE_CREATE + 句柄"| W["WM / Mini-DWM"]
    A -->|"写入像素"| V
    A -->|"FLIP_PRESENT + 新句柄"| W
    W -->|"零拷贝换前缓冲"| C["CRTC / 合成器"]
    C -->|"present"| FB["帧缓冲"]
    A2["v1.x 客户端"] -->|"CLEAR/SHAPE/TEXT/INVALIDATE"| W
```

---

## 6. 网络与安全架构

### 6.1 网络协议栈

```mermaid
flowchart TB
    APP["用户态：netman / nsh / novusd / Win32 ws2_32"]
    SYS["SYS_HTTPS(26) / sysinfo 网络查询"]
    HTTP["net_http.c（单会话）<br/>HTTP 客户端"]
    TLS["security/tls<br/>TLS 1.3 1-RTT / TLS 1.2 ECDHE+AEAD<br/>X.509 链 · AES-GCM · ChaCha20-Poly1305"]
    L4["net_tcp.c（单连接，无窗口缩放/拥塞控制）<br/>net_dns.c（单事务，固定源端口 5353）"]
    L3["net.c：ARP / IP / UDP / 网关转发<br/>net_dhcp.c / net_ntp.c / net_ncsi.c"]
    DRV["统一 PCI ID 表 net_ids.c"]
    NIC["e1000 / e1000e / I21x / igb /<br/>virtio-net(legacy) / rtl8139 / wifi(仅识别)"]
    POLL["net_tick() 轮询收包<br/>每 tick 最多 8 帧"]

    APP --> SYS --> HTTP --> TLS --> L4 --> L3
    L3 --> DRV --> NIC
    NIC --> POLL --> L3
```

### 6.2 安全边界

```mermaid
flowchart TB
    subgraph TRUST["可信侧"]
        FW["UEFI 引导 + Merkle 校验（A 核）"]
        KR["内核 Ring0<br/>页表 / syscall / IPC / 块层"]
        NKM["NKM 模块<br/>SHA-256(清零 hash/sig) + Ed25519"]
        UPD["更新包<br/>128B 冻结头 + SHA-256 + Ed25519"]
    end

    subgraph UNTRUST["不可信侧"]
        NUC["NUC 用户程序<br/>无签名 · ring3 页表隔离"]
        W32["Win32 EXE<br/>ABI 白名单：只放行 40/60"]
        IPC_C["普通 IPC 客户端<br/>（当前无强端口鉴权）"]
    end

    FW -->|"完整性结论写入共享内存"| KR
    NKM -->|"签名通过才加载"| KR
    UPD -->|"8 步校验 → 快照 → 原子替换"| KR
    KR -->|"syscall 边界 + 用户指针逐页复制"| NUC
    KR -->|"白名单 thunk"| W32
    KR <-->|"端口通信（非安全边界）"| IPC_C

    NOTE["规范目标但未落地：<br/>CAP 128 位模型 · Secure Boot<br/>端口强鉴权 · 密钥轮换 · 硬件 TRNG"]
    NOTE -.-> KR
```

> 安全口径真源是 [SECURITY.md](SECURITY.md)。不要把 CAP、Secure Boot、完整 WebPKI 当作已实现能力。

### 6.3 更新体系

```mermaid
flowchart LR
    SRV["updatesrv<br/>HTTP/HTTPS · /v1/updates<br/>五级优先通道"]
    PKG["novupkg 生成<br/>.novu 全量 / .hotpth / .npth / .apth"]
    Q["novu_queue<br/>五级优先队列"]
    V["novu_verify<br/>SHA-256 + Ed25519 + 8 步校验"]
    A["novu_apply<br/>快照 → 原子替换 → 逆序回滚 → Merkle 重建"]
    J["novu_service<br/>状态机 + 持久 journal"]
    UI["nsh update / novusd / 设置更新中心"]
    UI --> Q --> V --> A --> J
    SRV --> Q
    PKG --> SRV
```

---

## 7. 用户态架构

### 7.1 内置应用与运行时

`apps/` 是 Rust `no_std`、无分配器、无第三方依赖环境：固定数组/栈缓冲，`panic` 只打印 COM1 并 `exit(3)`；扁平镜像链接到 `0x400000`，`objcopy -O binary` → `xxd -i` → 内核符号 `k_app_<name>`。

```mermaid
flowchart TB
    subgraph BINS["apps/src/bin/（内嵌 NUC blob）"]
        B1["fileman / taskman / settings / netman"]
        B2["session / setup / nsh / devmgmt / dwm"]
        B3["novusd（来自 user/novusd）"]
    end

    subgraph LIB["apps/src/ 共享库"]
        L1["sys.rs：裸 syscall 封装"]
        L2["fs.rs：文件 syscall 客户端"]
        L3["sinfo.rs：sysinfo 服务（端口 sys）<br/>任务/文件/统计/网络/语言/主题/显示"]
        L4["wm.rs + ipc.rs：WM 协议客户端"]
        L5["ui.rs + theme.rs + lang.rs + unicode_data.rs"]
        L6["shell/：mod cmds parse path update crypto"]
        L7["nuomi.rs / devmgr/ / dwm/ / install.rs"]
    end

    BINS --> LIB
    LIB -->|"SYS_*"| K["内核 syscall"]
    LIB -->|"IPC 端口 sys"| S["kernel/sysinfo.c"]
    LIB -->|"IPC 端口 nwm"| W["Novus-WM"]
```

**内置应用注册链**（新增应用需改 4 处）：

```mermaid
flowchart LR
    A["apps/src/bin/name.rs"] --> B["apps/Cargo.toml [[bin]]"]
    B --> C["Makefile APPS_NAMES<br/>→ out/apps/name.bin → k_app_name"]
    C --> D["kernel/apps.c g_apps[] 末尾追加<br/>（旧索引被复用，不能插中间）"]
    D --> E["graphics/nd_demo.c<br/>开始菜单/磁贴/任务栏"]
```

### 7.2 `nsh` 终端

```mermaid
flowchart LR
    IN["键盘事件 NWM_EV_KEY"] --> SH["shell/mod.rs<br/>行编辑 · 历史 64 · 变量 32 · 别名 16"]
    SH --> PA["parse.rs<br/>分词 · 引号 · glob(*/? 按码点)<br/>重定向 > >> < · 管道 | · $(...)"]
    PA --> EX["cmds.rs BUILTINS（35 条）<br/>内联执行，无 fork/exec"]
    EX --> OUT["out.write()（唯一输出通道）<br/>SINK → 窗口显示列表 / 捕获缓冲"]
    EX --> SPAWN["sinfo::spawn_name()<br/>启动外部内置应用"]
    EX --> FS["fs.rs → SYS_OPEN/READ/FWRITE"]
    EX --> NM["nuomi 指令 → apps/src/nuomi.rs"]
```

**35 条内建命令**：`ls cd pwd cat echo mkdir rmdir rm cp mv stat env export unset clear help history true false whoami which touch wc head tail tree alias drives mount umount nuomi update novusd run install`。
`dir`/`copy`/`del` 兼容别名默认关闭。管道是顺序捕获（≤8 段，捕获缓冲 4 KiB），不是并发进程；参数上限 40，单参数 256 B。

### 7.3 多语言执行路径

```mermaid
flowchart TB
    subgraph HOST["主机端工具链"]
        CPP["cppcc（Rust C++ 子集编译器）<br/>预处理→词法→语法→语义→AST→codegen"]
        CPP --> N1["NUC V1.0（x86_64 原生机器码）"]
        CPP --> N2["Win64 PE32+"]
        CPP --> N3["Linux ELF64"]
        NUOMI_C["Nuomi_pl C 参考实现<br/>带 GC · nuomi nuc 生成 NUC"]
    end

    subgraph DEVICE["设备端"]
        FIXED["boot-stage 固定读取<br/>APPS/DEMO.NUC → 共享内存 → nuc_load()"]
        EXEC["通用 ./app.nuc exec（尚未实现）"]
        NSH_NUOMI["nsh 内置 nuomi.rs<br/>no_std 无堆 bump arena 256 KiB"]
    end

    N1 --> FIXED
    N1 -.-> EXEC
    NUOMI_C -.-> N1
    NSH_NUOMI -->|"nuomi script.nuomi"| SH["shell 行编辑"]
```

---

## 8. 构建与镜像架构

### 8.1 构建流水线

```mermaid
flowchart TB
    CFG["configs/novus_defconfig<br/>（Kconfig 仅声明占位，无 menuconfig）"]
    SRC["C 源（kernel/fs/security/graphics/wm）<br/>Rust 内核静态库（x86_64-pc-windows-msvc）<br/>Rust 应用（x86_64-unknown-none）"]
    GEN["生成器（产物已提交）<br/>themegen / langgen / fontgen / imegen<br/>wpgen / cabundle / nkmgen"]
    CFG --> MK["顶层 Makefile<br/>SRC / ASM_SRC / INC 显式列举"]
    SRC --> MK
    GEN --> MK
    MK --> EFI["out/NOVUS.EFI<br/>PE32+ subsystem 10 · 入口 efi_main"]
    MK --> APPS["out/apps/&lt;name&gt;.bin<br/>→ build/apps_blobs.h"]
    MK --> TOOLS["主机工具<br/>mkfat mkiso mkfsnovus mkdisk nkmgen novupkg wpkg"]

    EFI --> IMG["out/novus.img（8 MiB NovusFS）"]
    APPS --> IMG
    TOOLS --> IMG
    IMG --> ESP["out/esp.img（31.5 MiB FAT）<br/>out/esp-tpl.img（16 MiB 安装模板）"]
    ESP --> ISO["out/novus-live.iso<br/>ISO9660 + Joliet + El Torito + isohybrid"]
    IMG --> DISK["out/novus-disk.img（64 MiB）<br/>GPT + ESP + NovusFS"]
    ISO --> CHECK["make check-rootfs<br/>主/Joliet 树 + 8.3 名 + 字体 + 元数据"]
    DISK --> CHECK
```

> El Torito 扇区计数只有 16 位（单位 512 B），启动项必须 < 32 MiB，故 live ESP 取 31.5 MiB。

### 8.2 验证矩阵

```mermaid
flowchart LR
    subgraph HOST["主机侧"]
        T1["display / irq / sched / dwm / memory selftest"]
        T2["novupkg selftest · nkmgen selftest<br/>updatesrv selftest · cppcc cargo test"]
        T3["check_rootfs.py · inspect_img.py<br/>themegen/langgen --check"]
    end
    subgraph QEMU["QEMU / OVMF"]
        Q1["qemu-smoke.sh + VNC 基线比对<br/>（固定 127.0.0.1:5901）"]
        Q2["ci_smoke.py（300s 无头启动）"]
        Q3["run-install-test.sh（EN=1 / FLOW=desktop）"]
        Q4["wm-churn.sh（20 轮应用生命周期）"]
        Q5["qtest.sh（IDE/AHCI/NVMe/USB）"]
    end
    HOST --> CI["make pipeline<br/>check → check-lint → build → smoke"]
    QEMU --> CI
    CI --> OUT["out/ci/report.*"]
```

---

## 9. 端到端数据流

### 9.1 终端按键到屏幕像素

```mermaid
sequenceDiagram
    participant HW as 键盘/鼠标
    participant IRQ as IRQ / nd_input
    participant W as Novus-WM
    participant SH as nsh 收件箱
    participant SHL as shell 引擎
    participant K as 内核 syscall
    participant C as 合成器

    HW->>IRQ: 按键扫描码（含 Ctrl/Alt）
    IRQ->>W: 输入事件（IME 先处理组合/候选）
    W->>SH: NWM_EV_KEY（Unicode 码点）
    SH->>SHL: 行编辑 → parse → cmds
    SHL->>K: 内建命令直接调 syscall
    K-->>SHL: 结果写入 out.write()
    SHL->>W: CLEAR / SHAPE / TEXT… / INVALIDATE
    W->>C: 从显示列表绘制客户区
    C->>C: 离屏合成 → 回扫等待 → 整帧 present
    C-->>HW: 屏幕像素更新
```

### 9.2 文件打开到磁盘 I/O

```mermaid
sequenceDiagram
    participant A as 应用 fs.rs
    participant T as kernel/trap.c
    participant V as VFS + 挂载表
    participant N as NovusFS
    participant B as blkdev
    participant D as AHCI/NVMe/USB

    A->>T: SYS_OPEN(path, flags)（用户指针逐页复制）
    T->>V: 按最长前缀选挂载项（≤256B）
    V->>N: lookup 逐组件解析（无 .. 穿越）
    N->>B: 读 superblock/inode/数据块
    B->>D: 512B 扇区 I/O（UEFI bounce 兼容）
    D-->>B: 数据
    B-->>N: 块
    N-->>V: inode
    V-->>T: fd（从 3 起）
    T-->>A: fd
    Note over A,D: 后续 read/fs_write 单次上限 4096B<br/>写路径经 WAL + Merkle 更新
```

---

## 10. 架构约束速查表

| 约束 | 值 | 真源 |
|---|---|---|
| 任务数 / 内核栈 / 时间片 | 10 / 64 KiB / 4 tick | `kernel/sched.h` |
| 路径 / 单次 I/O / fd 起点 | 256 B / 4096 B / 3 | `kernel/trap.c:78-80` |
| 挂载项 / 挂载白名单 | 8 / 6 条 | `fs/vfs_mount.h:17`、`rootfs_layout.h:138-144` |
| NovusFS 块 / inode / 文件名 | 4 KiB / 1024×128 B / 56 B | `fs/novusfs/novusfs.h` |
| WAL / Merkle | 64 块 / 64 B 节点、64 节点每块 | `fs/novusfs/novusfs.h` |
| IPC 头 / 载荷 / 队列 / 端口 | 32 B / ≤128 B / 8 / 16 | `kernel/ipc.h` |
| WM 版本 / 窗口 / 图元 / 文字 | `0x00020000` / 10 / 120 / 160 | `wm/novus-wm/nwm_proto.h:26-42` |
| GOP 输出 / 字符字形 | 4 / 114,397 | `graphics/nd_hal.c`、`nd_font_uni.c` |
| 应用 blob / 栈 | <128 KiB / 64 KiB–4 MiB | `CONTRIBUTING.md:360,369` |
| nsh 历史 / 变量 / 别名 / 管道 | 64 / 32 / 16 / ≤8 段 | `apps/src/shell/mod.rs` |
| Nuomi 源码 / arena / 递归 | 6 KiB / 256 KiB / 约 500 层 | `apps/src/nuomi.rs` |
| live ESP 上限 | <32 MiB（取 31.5 MiB） | `tools/tools.mk` |

### 已知边界（架构层面）

```mermaid
flowchart LR
    subgraph LIM["当前实现边界（详见 ROADMAP.md §4）"]
        L1["SMP：A/B 双核，多 AP 未实现"]
        L2["网络：单 TCP 会话、DNS 单事务、收包轮询"]
        L3["图形：CPU 2D、无 vblank 中断、多输出仅镜像"]
        L4["存储：IDE PIO、NVMe 队列深 4、USB BOT 非 UASP"]
        L5["安全：CAP/Secure Boot/端口鉴权/密钥轮换未落地"]
        L6["执行：通用 ./app.nuc 未实现，仅固定 APPS/DEMO.NUC"]
        L7["Win32：原型，无磁盘 DLL / 32 位 PE32 / 真实沙箱"]
    end
```

> 架构图描述"是什么"，不描述"计划是什么"。能力缺口与演进顺序见 [ROADMAP.md](ROADMAP.md)。
