# Novus OS 术语表（GLOSSARY）

> 版本：v1.0（2026-11-27）
> 配套：[ARCHITECTURE.md](ARCHITECTURE.md)、[ABI.md](ABI.md)、[COMPATIBILITY.md](COMPATIBILITY.md)、[BUILD.md](BUILD.md)
>
> 本表解释项目中**自创的名字、缩写、模块名与文件前缀**。它是给新人看的"黑话词典"：遇到一个不认识的 `nd_*`、`nuc_load`、`INVALIDATE`、`六件套` 时来这里查。通用计算机术语（如页表、中断、CRC）只在与本项目有特殊含义时才列出。
>
> 标注约定：**[已实现]** = 源码可用；**[规范目标]** = 文档写了但代码未落地；**[原型]** = 子集可用。

---

## 目录

- [1. 项目与产品名](#1-项目与产品名)
- [2. 文件格式与容器](#2-文件格式与容器)
- [3. 启动、内核与内存](#3-启动内核与内存)
- [4. 图形、窗口与输入](#4-图形窗口与输入)
- [5. 文件系统与存储](#5-文件系统与存储)
- [6. 用户态、应用与语言](#6-用户态应用与语言)
- [7. 网络与安全](#7-网络与安全)
- [8. 构建、镜像与验证](#8-构建镜像与验证)
- [9. 文件名前缀速查](#9-文件名前缀速查)
- [10. 最容易搞混的术语](#10-最容易搞混的术语)

---

## 1. 项目与产品名

| 术语 | 全称 / 含义 | 说明 |
|---|---|---|
| **Novus OS** | 项目名 | x86_64 UEFI 桌面操作系统，GPLv2，全部底层自研，不依赖外部开源项目 |
| **V1.6-FinalSpec** | 当前规范版本 | 冻结正式版，19 部分；对应当前主分支 |
| **M0～M12** | 开发里程碑编号 | M0 引导 → M12 设备管理器/Mini-DWM；**不等于发布版本号** |
| **NovusFS** | Novus File System | 自研可写文件系统：双超级块 + WAL + Merkle；磁盘 magic `NOVS`，版本 `0x00030000` |
| **NovusDisplay / nd** | 自研图形栈 | 帧缓冲 HAL → 2D 软件渲染 → nvgui 控件 → 桌面壳层；代码前缀 `nd_*` |
| **Novus-WM / nwm** | 窗口管理器 | 内核态合成器 + IPC 协议服务端；端口 `"nwm"`，协议真源是 `nwm_proto.h` |
| **Novus Fluent** | UI 设计语言 | 语义令牌（浅色/深色/跟随时间），由 `theme.toml` 生成 C/Rust 两侧 |
| **NovusDefender / novusd** | 内置安全守护 | 源码 `user/novusd`；EICAR 签名检测、扫描、隔离、增量更新、目录快照监控 |
| **nsh** | Novus Shell | 窗口化终端，144×26 字符网格；引擎在 `apps/src/shell/`；35 条内建命令 |
| **nvgui** | Novus GUI | 用户态控件库，固定池 192 个控件（窗口/按钮/文本框/滑块/列表/树/菜单） |
| **Mini-DWM** | 迷你桌面窗口管理器 | 表面重定向 + 软件合成 + 显示 syscall；当前只有 CPU 后端 |
| **Nuomi（糯米）** | 项目自创 Lisp 风格语言 | 两条实现线：宿主 C 参考实现（`user/Nuomi_pl`）与设备端 `no_std` 内置解释器 |
| **cppcc** | C++ 子集编译器 | 主机端 Rust 工具，输出 NUC / PE32+ / ELF64 三目标；不是完整 C++11 |
| **Mochi** | 另一门 Lisp 风格语言 | 规范存在，但当前工作树**没有** `user/mochi` 子模块 **[规范目标]** |
| **MicroPython** | 用户态 Python 运行时 | 只在 README 路线图中，无源码、无构建目标 **[规范目标]** |
| **nuc_spawn_blob** | 内核加载内置应用的入口 | 把内嵌扁平镜像映射到 `0x400000` 并创建任务；镜像上限 128 KiB |

---

## 2. 文件格式与容器

| 术语 | 含义 | 关键点 |
|---|---|---|
| **NUC** | 原生可执行格式（`.nuc`） | 48 B 头、magic `NUC!`、ABI `0x0100`；装的是 x86_64 原生机器码，**无独立虚拟机/字节码解释器** |
| **NKM** | 内核模块格式 | 208 B 头、magic `NKM!`、Ed25519 签名；Ring0 模块，必须签名 |
| **NUP** | 应用安装包格式 | V1.0 冻结，64 B 头、magic `NUP!` **[已实现规范，消费端待核对]** |
| **INS** | 快捷方式文件格式 | V1.0 冻结，INI 风格 UTF-8 文本，路径 `/ProgramData/NovusApps/*.ins` |
| **novu** | 更新包体系 | `.novu` 全量包 + `.hotpth`/`.npth`/`.apth` 三类补丁；五级优先通道；128 B 冻结头 |
| **NWPK** | 壁纸包 | `WALLPAPERS.PKG`，40 B 索引项 + 裸 JPEG；引导阶段读入保留页 |
| **NVSH** | 共享内存布局版本 | v1.1，8 KiB @ `0x7E000`，引导 ↔ 内核交接区 |
| **ESP** | EFI System Partition | FAT16/32 分区，只支持 8.3 短名；live ESP 31.5 MiB（受 El Torito 32 MiB 上限约束） |
| **El Torito** | CD 启动规范 | no-emulation EFI 启动项；扇区计数 16 位 ⇒ 启动项必须 < 32 MiB |
| **isohybrid** | ISO 可当磁盘写 U 盘 | 同一镜像可写 U 盘并被固件识别启动 |
| **Joliet** | ISO9660 长名扩展 | 优先于普通 ISO 树；FAT 侧仍只有 8.3 短名 |
| **PE32+ / COFF / ELF64** | 外部目标格式 | cppcc 可输出 PE32+（Win64）与 ELF64（Linux）；内核 Rust 静态库用 msvc target 产 COFF 供 mingw 链接 |
| **APPS/DEMO.NUC** | boot-stage 固定 demo 槽 | 当前唯一自动执行的 NUC 路径；通用 `./app.nuc` exec 尚未实现 |

---

## 3. 启动、内核与内存

| 术语 | 含义 | 说明 |
|---|---|---|
| **A 核 / BSP** | 引导处理器 | 开机第一核，做内存扫描、硬件自检、Merkle 校验，然后休眠；可用 `CONFIG_NOVUS_A_CORE_DISABLE` 跳过 |
| **B 核 / AP** | 应用处理器 | 由 A 核唤醒，加载主内核并接管全部运行；SIPI 失败降级单核 |
| **SIPI** | Startup IPI | 唤醒 AP 的处理器间中断；失败不 panic，继续单核 |
| **AP trampoline** | AP 启动跳板 | 16 位裸二进制，< 4 KiB、无重定位；末 24 字节是 CR3/AP 栈/`ap_main` 三个 64 位槽 |
| **efi_main** | UEFI 入口 | `NOVUS.EFI` 的 PE32+ 入口函数 |
| **kmain** | 内核主初始化 | 固定顺序：shared-mem → PMM → MM → heap → 显示 → 存储 → devtab → IPC → FS → NKM → SMP → 调度 |
| **kputs** | 统一日志入口 | 同时输出到 VGA、COM1、`klog` 环与可选的 framebuffer 控制台；带锁 |
| **klog** | 日志环 | 引导阶段 4 KiB（共享内存尾部）+ 内核阶段 32 KiB；启动 splash 直接读前者 |
| **共享内存交接区** | 引导 ↔ 内核的固定块 | 8 KiB @ `0x7E000`，传递硬件信息、自检状态、Merkle 根、live root、壁纸包、安装载荷 |
| **PMM** | 物理内存管理器 | 固定 256 KiB 位图，约覆盖 8 GiB；DMA 缓冲须显式走 PMM |
| **MM** | 内存管理（分页） | 低 4 GiB 恒等映射 + physmap；帧缓冲统一 UC 映射 |
| **VM / vmm** | 虚拟内存 / 虚拟页分配 | 每任务独立 CR3；`vm.c` 支持 COW 与共享映射 |
| **kmalloc / 内核堆** | 页粒度分配器 | 4 KiB 向上取整，最多 256 MiB / 256 条记录；**不是小对象 slab** |
| **LowMem / Full** | 内存模式 | 2–6 GiB 缩减特效与进程数、更早 OOM；≥6 GiB 全功能 |
| **OOM** | 内存耗尽处理 | 按优先级回收用户进程，保护内核；LowMem 下更早触发 |
| **IDT / GDT / IRQ / LAPIC** | 中断体系 | IDT 处理异常与 syscall（向量 `0x100`），LAPIC 定时器驱动时间片 |
| **CR3** | 页表基址寄存器 | 每任务一个，实现进程隔离 |
| **NX / W^X** | 内存保护 | NUC 只有 TEXT 可执行；W^X 完整落实仍在路线图中 |
| **time slice / tick** | 调度时间片 | 默认 4 tick，`SCHED_MAX_TASKS=10`，每任务 64 KiB ring-0 栈 |
| **stamp** | 构建标记文件 | 记录 flag 串或源文件变化；`apps.stamp` 只跟踪应用源的四类路径 |

---

## 4. 图形、窗口与输入

| 术语 | 含义 | 说明 |
|---|---|---|
| **`nd_*`** | NovusDisplay 代码前缀 | `nd_hal.c`（帧缓冲 HAL）、`nd_core.c`（2D 渲染）、`nd_font_*`（字体）、`nd_ime.*`（输入法）、`nd_boot.c`（启动画面） |
| **帧缓冲 HAL** | 显示硬件抽象 | 枚举最多 4 路 GOP 输出，第一个主屏，其余镜像；EDID 读首选时序 |
| **合成缓冲 / nd_canvas** | 离屏画布 | 所有绘制先落到离屏缓冲，`nd_present()` 整帧呈现，避免半帧闪烁 |
| **damage** | 脏区 | 光标移动与窗口重绘只记录变化矩形，减少拷贝量 |
| **回扫等待** | 防撕裂 | 探测 VGA 状态口 `0x3DA`，有界等待（约 0.4 ms）；无硬件 flip/vblank 中断 |
| **显示列表** | GUI 客户端的绘制指令序列 | `CLEAR` → `SHAPE`/`TEXT` → `INVALIDATE`；≤120 图元 / ≤160 文字 |
| **INVALIDATE** | 帧提交消息 | **必须**在帧尾发送，否则半帧状态被提前绘制，出现闪烁 |
| **surface redirection** | 表面重定向 | WM v2：客户端用 vidmem 句柄代替像素传 IPC，实现零拷贝 flip |
| **flip / FLIP_PRESENT** | 前后缓冲切换 | v2 客户端提交新表面句柄，合成器换前缓冲 |
| **vidmem / CRTC / VSync** | 显存对象 / 显示控制器 / 垂直同步 | Mini-DWM 的显示 syscall 抽象；当前只有软件后端 |
| **nwm** | WM 协议端口 | `"nwm"`；`NWM_PID_SHELL=0` 表示桌面自身（本地内联分发，无收件箱） |
| **IPC** | 进程间通信 | 内核辅助：32 B 头、magic `0x4E495043`、FNV-1a、载荷 ≤128 B、队列 8、端口 16 |
| **IME** | 输入法 | 拼音单音节输入，≤6 候选；`Ctrl+Space` 切中英，`Ctrl+Shift+U` 码点直输 |
| **ND_KEY_BASE** | 输入事件键码基址 | `0x110000`；字符键直接携带 Unicode 码点，避免与控制键冲突 **[已实现 ABI]** |
| **backing store** | 控件池 | nvgui 固定 192 个控件，无动态分配 |
| **语义令牌** | UI 颜色角色 | `ui::bg()`、`card_bg()`、`row_sel()`、`text()`、`dim()`、`accent()`、`line()`；不写字面色值 |
| **壁纸回退渐变** | 当前壁纸实现 | JPEG 解码器有工具链相关 UB，桌面改用程序化竖向渐变 |

---

## 5. 文件系统与存储

| 术语 | 含义 | 说明 |
|---|---|---|
| **VFS** | 虚拟文件系统 | 单一 syscall 可见根 `vfs_root()`；挂载表按路径前缀切换后端 |
| **挂载表** | 8 项固定表 | 路径绝对且唯一；`/` 不可卸载；跨挂载 `rename` 拒绝 |
| **rootfs 布局** | 标准根目录树 | 唯一真源 `fs/novusfs/rootfs_layout.h`，78 个目录 **[已实现]** |
| **mounts.json** | 挂载配置 | `/.novusfs/mounts.json` ≤4 KiB / ≤8 条 / 白名单 6 条；运行时 `mount` 不回写（保护 Merkle） |
| **.. 穿越消除** | 路径规范化 | 本项目**不做**；目录实现也不新增 `..` 条目 |
| **WAL** | 预写日志 | 64 块环形，逐记录 CRC-32 + commit 标记，挂载时 replay；错误只 WARN |
| **Merkle** | 完整性树 | 64 B SHA-256 节点，每块 64 个；覆盖 bitmap+inode+data，不含 WAL 与自身；根写双 superblock |
| **双超级块** | 原子根更新 | 主备副本 + 版本计数器；事务失败回滚旧副本 |
| **fsck 优先级** | 修复顺序 | 超级块 → WAL → inode 隔离 |
| **blkdev** | 块设备抽象 | 统一 512 B 扇区；4Kn 设备走整块 bounce buffer |
| **devtab** | 设备表 | `devtab_refresh()` 生成，设备管理器只观测它，不自行扫硬件 |
| **devmgmt / devmgr** | 设备管理器 | 应用 `devmgmt`，内核模块 `apps/src/devmgr/`，模态属性窗 |
| **BOT** | USB 大容量存储协议 | Bulk-Only Transport（CBW/CSW、READ10/WRITE10）；非 UASP |
| **xHCI / UHCI / EHCI** | USB 控制器 | xHCI 1.1 完整路径；UHCI 仅 HID boot；EHCI-only 高速设备不支持 |
| **power_state** | 磁盘电源状态查询 | AHCI 主动置空、USB 未实现、IDE 在 QEMU 拿不到数据 **[部分实现]** |

---

## 6. 用户态、应用与语言

| 术语 | 含义 | 说明 |
|---|---|---|
| **apps/** | 内置应用源码 | Rust `no_std` 无分配器；扁平 NUC 镜像编进内核 |
| **g_apps[]** | 应用注册表 | 内核 `kernel/apps.c`；**只能在末尾追加**，索引被旧版 sysinfo 与客户端复用 |
| **apps_launch_name()** | 按名启动 | 新代码应使用的方式，避免依赖数组索引 |
| **bootstrap 应用** | 特殊启动应用 | 只有 `session` 与 `setup`；其它应用必须已登录，同名实例唯一 |
| **sinfo / sysinfo** | 系统信息服务 | 端口 `"sys"`，提供任务、文件、统计、网络、语言、主题、显示等查询 |
| **nsh 内建命令** | 35 条命令 | `ls cd pwd cat echo ... nuomi update novusd run install`；注册于 `cmds.rs` 的 `BUILTINS` |
| **顺序捕获管道** | nsh 管道实现 | 无 fork/exec，管道把上一阶段输出整体捕获后交给下一阶段，≤8 段、缓冲 4 KiB **[已知限制]** |
| **cmd 兼容别名** | DOS 风格别名 | `dir`/`copy`/`del`，默认关闭，不要依赖它们写新脚本 |
| **bump arena** | Nuomi 设备端内存模型 | 256 KiB 对象区（其中 128 KiB 环境可回收），无 GC；用完报 `out of memory` |
| **MAX_DEPTH** | Nuomi 递归上限 | 1500 层求值，约 500 层函数调用，超出报深度错误而非撞栈 |
| **三目标后端** | cppcc 输出 | NUC V1.0 / Win64 PE32+ / Linux ELF64；由 `src/target/` 分发层统一接入 |
| **C ABI 库** | cppcc 的 C 接口 | `cppcc-cabi`，稳定头文件供 C 调用方使用 |
| **novusd 命令通道** | 文件式守护协议 | 经 `/temp/novusd/req.bin`、`rsp.bin` 传递（内核 spawn 无 argv） |
| **6 阶段（novusd）** | 移植交付阶段 | 检测核心、Rust 平台层、CLI、隔离、更新、E2E；设备内 5 项能力降级 **[原型]** |
| **模板（templates/）** | 三条扩展路径 | GUI 应用 / nsh 命令 / Nuomi 脚本骨架 |

---

## 7. 网络与安全

| 术语 | 含义 | 说明 |
|---|---|---|
| **NCSI** | 网络连接状态指示 | 项目自有的联网检测：抓 `connecttest.txt`，区分无链路/DNS/TCP/HTTP/超时 |
| **DNS 单事务** | 当前 DNS 实现 | 固定源端口 5353，一次只处理一个 A 记录查询 **[已实现限制]** |
| **单 TCP 会话** | 当前 TCP 实现 | 一条连接、无窗口缩放/拥塞控制、1 s 重传 3 次；无监听端 |
| **CAP** | 能力权限模型 | 规范定义 128 位掩码 `cap_check()`，但实际未落地 **[规范目标]**；不要依赖它做授权 |
| **NKM 哈希规则** | 签名摘要规则 | SHA-256 覆盖整文件但把 `[0x50,0xB0)`（hash+sig）置零；Ed25519 签 32 B 摘要 **[已实现]** |
| **开发公钥** | 当前签名根 | NKM 只有一把开发公钥，更新开发公钥硬编码；无运行时轮换 **[已知限制]** |
| **NKM_FLAG_ALLOW_UNSIGNED** | 调试开关 | 仅管理员本次会话有效是规范目标；当前无完整管理员鉴权，不得持久化信任 |
| **六件套** | ABI 变更必改清单 | 规范文档、内核实现、宿主工具、测试夹具、版本号、兼容性说明；缺一不可 |
| **ABI BREAK** | 不兼容变更 | OS 级重大事件，必须高亮标记、走 RFC、升主版本（见 [CHANGELOG.md](CHANGELOG.md)） |
| **WebPKI** | 公钥基础设施 | 当前 TLS 不是完整 WebPKI：无 CRL/OCSP、名称约束、keyUsage 全检 **[部分实现]** |
| **fail-closed** | 安全默认 | CA 根集合为空时拒绝连接，不放行 |
| **EICAR** | 反病毒测试标记 | novusd 用它做签名检测自测，不是真实恶意样本 |
| **显式拒绝** | 安全设计原则 | 未实现特性必须给诊断，不能"语法接受但语义不支持" |

---

## 8. 构建、镜像与验证

| 术语 | 含义 | 说明 |
|---|---|---|
| **defconfig** | 默认配置 | `configs/novus_defconfig` 是唯一真正被 Makefile `-include` 的配置源 |
| **Kconfig** | 配置声明 | 顶层只是占位文件；**没有解析器、没有 `make menuconfig`** |
| **`y` / `n`** | 配置值 | 只转成 C 宏 `1`/`0`；`depends on` 不自动校验 |
| **CROSS** | 交叉工具链前缀 | 默认 `x86_64-w64-mingw32-` |
| **E2E** | 端到端测试构建标志 | `E2E=1` 编入测试根证书与 HTTPS 探针；所有相关 make 调用必须一致 |
| **生成物** | 提交入库的派生文件 | 主题/语言/字体/IME/壁纸/CA/NKM 测试模块；改源 TOML 后须重生成，`--check` 验漂移 |
| **mkfat / mkiso / mkfsnovus / mkdisk** | 主机镜像工具 | 自研；`mkfsnovus` 与 `mkdisk` 直接编译内核 FS/GPT 源码，避免第二实现漂移 |
| **nkmgen / novupkg** | 签名与打包工具 | Rust 主机工具；`novupkg` 生成 `.novu`/补丁包 |
| **novus.img / esp.img / iso / disk.img** | 四类镜像 | 8 MiB 系统盘 / 31.5 MiB live ESP / live ISO / 64 MiB GPT 整盘 |
| **qemu-smoke** | QEMU 冒烟 | VNC 截图与 `scripts/baselines/qemu/{disk,no-root}` 基线比对；VNC 仅监听 127.0.0.1:5901 |
| **基线** | 截图回归基准 | 缺失或不匹配即失败；普通回归禁用 `--update-baseline`，只有 UI 有意变更才更新 |
| **wm-churn** | 应用生命周期测试 | 20 轮关闭/重启应用，检查 tick 递增、无 double free、无 "no runnable task" |
| **OVMF** | 开源 UEFI 固件 | QEMU 启动必需；测试用临时 VARS 与 `-snapshot` |
| **check-rootfs** | 镜像校验目标 | 校验 ISO 主/Joliet 树、ESP 8.3 名、系统盘目录树与字体 |
| **ci.mk 入口** | 本地 CI | `make check` / `check-lint` / `smoke` / `pipeline`；**无托管 CI** |
| **机器特定路径** | 换机需调整 | 多个脚本含 `/home/dev/...` 或固定 OVMF 路径，不要提交个人路径 |
| **降级（WARN）** | 故障策略 | 轻微错误不 panic，继续启动但功能受限；PR 中必须说明出现过哪些 WARN |

---

## 9. 文件名前缀速查

| 前缀 | 所属子系统 | 例子 |
|---|---|---|
| `nd_` | NovusDisplay 图形 | `nd_hal.c`、`nd_core.c`、`nd_boot.c`、`nd_font_uni.c` |
| `nwm_` | Novus-WM 协议 | `nwm_proto.h`、`nwm.c` |
| `nuc_` | NUC 加载器 | `nuc.c`、`nuc.h`、`nuc_spawn_blob()` |
| `nkm_` | NKM 内核模块 | `nkm.c`、`nkm.h`、`nkm_test_module.h` |
| `novu_` | 更新体系 | `novu_verify.c`、`novu_apply.c`、`novu_queue.c` |
| `net_` | 网络 | `net_e1000.c`、`net_dns.c`、`net_ncsi.c` |
| `vfs_` | 虚拟文件系统 | `vfs.c`、`vfs_mount.c` |
| `novusfs_` | NovusFS | `novusfs_mount()`、`novusfs_selftest()` |
| `blkdev_` | 块设备层 | `blkdev_register()`、`blkdev_power_state()` |
| `devtab_` | 设备表 | `devtab_refresh()`、`devtab_init()` |
| `sys_` | syscall 实现 | `sys_fs_open()`、`sys_ipc_send()` |
| `cmd_` | shell 内建命令 | `cmd_ls()`、`cmd_nuomi()`、`BUILTINS` |
| `k*` | 内核通用设施 | `kmain()`、`kputs()`、`klog.c` |
| `g_*` | 全局变量 | `g_syscalls[]`、`g_apps[]`、`g_unknown_calls` |

---

## 10. 最容易搞混的术语

> 新人最常踩的坑：名字相近但含义完全不同。

| 容易混 | 区别 |
|---|---|
| **`write(2)` vs `fs_write(10)`** | 编号 2 是串口日志输出，**不是**写文件；写文件是编号 10 |
| **NUC vs NKM** | NUC 是用户程序（不签名，页表隔离）；NKM 是 Ring0 模块（必须 Ed25519 签名） |
| **NovusDisplay vs nvgui** | `nd_*` 是内核图形栈/渲染器；`nvgui` 是用户态控件库 |
| **Novus-WM vs Mini-DWM** | Novus-WM 是协议服务端与窗口管理；Mini-DWM 是 v2 表面重定向与合成器；两者共存，WM 仍是回退 |
| **nwm vs nsh** | `nwm` 是窗口管理器端口；`nsh` 是终端应用 |
| **nsh vs sinfo/sysinfo** | `nsh` 是 shell；`sinfo` 是 shell/应用查询内核信息的 IPC 客户端 |
| **devmgmt vs devmgr vs devtab** | `devmgmt` 是应用；`apps/src/devmgr/` 是应用内模块；`devtab` 是内核设备表 |
| **Nuomi（设备端）vs Nuomi_pl（宿主）** | 前者是 `no_std` 无堆解释器（无 JSON/HTTP/进程/窗口原语）；后者是带 GC 的 C 参考实现与 NUC 工具链 |
| **cppcc vs nucgen** | `cppcc` 是编译器前端+codegen；`nucgen` 是其 NUC 序列化后端 |
| **E2E=1 vs 生产构建** | 测试锚只应在 E2E 构建中；两者对象不可混用，切换前 `make clean` |
| **M0～M12 vs V1.6** | 里程碑是开发阶段编号，不是发布版本号；规范版本是 V1.6-FinalSpec |
| **共享日志环 vs klog** | 引导阶段 4 KiB 环在共享内存尾部，splash 直接读；内核 `klog_init()` 继承后追加到 32 KiB 环 |
| **WARN vs 失败** | 本项目多数自检失败只打 WARN 继续启动；WARN 不等于测试通过，PR 必须说明 |
| **规范目标 vs 已实现** | CAP、Secure Boot、硬件 TRNG、MicroPython、通用 exec 都只是规范/路线图；见 [COMPATIBILITY.md](COMPATIBILITY.md) §11 |

---

## 11. 查词建议

1. 看到 `foo_bar()` 不知道属于哪层：先用 [§9 前缀表](#9-文件名前缀速查) 定位子系统。
2. 看到缩写（NUC/NKM/NCSI/…）：查 [§2](#2-文件格式与容器) 与 [§7](#7-网络与安全)。
3. 不确定某功能是否真的实现：查 [COMPATIBILITY.md](COMPATIBILITY.md) §11 已知不兼容清单与 [ROADMAP.md](ROADMAP.md) §0.3 漂移清单。
4. 本文只解释"名字是什么"；"怎么工作"看 [ARCHITECTURE.md](ARCHITECTURE.md)，"怎么构建"看 [BUILD.md](BUILD.md)。
