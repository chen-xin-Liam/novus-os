> 英文版本：[docs/en/ROADMAP.md](docs/en/ROADMAP.md)

# Novus OS 路线图（ROADMAP）

> 文档版本：v1.0（基于 2026-11-27 工作树全量盘点）
> 对应规范：Novus OS V1.6 完整技术规范（冻结正式版）、Debug 接口手册内容版本 V1.1
> 适用平台：x86_64 UEFI；协议：GPLv2；约束：底层全部自研，不引入外部开源项目（Unifont 字形数据为已记录例外）
>
> 本文件是对 `arch/`、`kernel/`、`fs/`、`security/`、`graphics/`、`wm/`、`apps/`、`user/`、`tools/`、`scripts/`、`templates/`、`configs/`、`assets/` 与全部 `docs/`、`.trae/specs/`、`.codebuddy/plans/` 逐目录盘点后的统一路线图。它描述**当前真实代码状态**、**已知缺口**与**分阶段落地计划**，不重复规范正文；所有"已完成"结论以源码、冻结格式规范与构建产物为准。

---

## 0. 阅读规则与权威性

### 0.1 事实优先级（冲突时从上到下）

1. **当前源码与构建结果**（`Makefile`、`configs/novus_defconfig`、冻结格式头文件）
2. **冻结格式规范**（`docs/NUC/NKM/NUP/INS ... V1.0.md`、`docs/Novus-WM 与 IPC 协议规范 V1.0.md`、`SECURITY.md`）
3. `changes.md`、`CONTRIBUTING.md` 中标注的实现状态与限制
4. `README.md` 的第 1–18 部分（含早期规划表述）
5. `.trae/specs/`、`.codebuddy/plans/`（工程过程资料，非规范）

### 0.2 状态标记

| 标记 | 含义 |
|---|---|
| [已完成] | 代码存在，且构建/自检/文档三者一致或仅有文字漂移 |
| [部分] | 主体路径可用，但有明确边界（单连接/只读/降级/原型） |
| [未闭环] | 规范或文档已写，但代码侧缺实现或契约未统一 |
| [未实现] | 无源码/无构建目标，或代码明确返回不支持 |
| [待核验] | 文档之间、文档与代码之间存在冲突，需先核实再排期 |

### 0.3 先读这一节：已知的文档/代码漂移

写路线图和排期前必须先消歧，否则会把"规划"当成"已完成"：

| 项 | 冲突表现 | 本路线图采用口径 |
|---|---|---|
| WM 协议版本 | `nwm_proto.h:2` 注释与 `docs/Novus-WM 与 IPC 协议规范 V1.0.md:108` 写 1.0；`nwm_proto.h:26` 实际为 `0x00020000`（surface redirection / Mini-DWM） | **以代码常量 2.0 为准**；文档标注待勘误（P0-01） |
| 窗口数上限 | 规范文档写 8；`nwm_proto.h:32` 为 `NWM_MAX_WINDOWS 10` | **10** |
| 最低内存 | `README.md:5,125` 为 2 GiB；`CONTRIBUTING.md:5` 为 4 GiB；`novus_defconfig:13` 为 2048 MB | **2 GiB 是引导拒绝线，4 GiB 是推荐工作线** |
| NKM 头长 | README 散文与部分文档写 128B；`security/nkm/nkm.h:63-67`、规范写 208B (`0xD0`) | **208B** |
| 更新包头长 | 内核冻结 128B（`security/novu/novu_format.h:11-15,92`）；`tools/novupkg/src/main.rs:3,20` 用 `HDR=144` | **契约冲突，列为 P0-02** |
| rootfs 目录数 | `rootfs_layout.h:109` 为 78；`fsinfo`/`check_rootfs.py:28` 写 77 | **78 为代码真源**，校验工具待同步 |
| 多显示器 | `changes.md:324` 说"尚未实现"；`:328-331` 说已实现 4 路 GOP 镜像 | **已实现镜像输出，非独立桌面**（见 §3.4） |
| Win32 能力 | `CONTRIBUTING.md:419` 说 GUI/文件/TLS 未实现；`kernel/win32.c`（216 KB）已有 GUI 控件、ws2_32 原型 | **原型级覆盖，能力边界见 §3.8 / §4.3** |
| NUC 栈上限 | `CONTRIBUTING.md:295` 与内核为 64 KiB–4 MiB；`user/cppcc/README.md:71` 写 64–256 KiB | **内核 clamp 64 KiB–4 MiB 为准** |
| M10 状态 | `README.md:494` 该行无 `🆗`；`:475-479` 称"已实现" | **主体已落地，稳定性/收尾进行中** |
| A/B 核 | README 第 2 部分称两阶段必走；`kernel/smp.c:34-36` 默认 `CONFIG_NOVUS_SMP 0`，`novus_defconfig:32` 为 `y` | **默认构建启用；SIPI 失败降级单核** |

---

## 1. 项目现状快照

### 1.1 定位与硬约束

- 目标：从零自研 x86_64 UEFI 桌面操作系统，媲美主流桌面 OS；内核主体 C，用户态 Rust `no_std`，部分模块 Rust FFI。
- 硬约束：不依赖外部开源内核/库；依赖单向、禁止循环依赖；磁盘格式与 ABI 冻结，同主版本向下兼容；统一哈希/签名规则。
- 最低 2 GiB（低于拒绝启动）；2–6 GiB LowMem（减少特效、限制进程、更早 OOM），≥6 GiB Full。
- 单一 syscall 可见 VFS 根 `/`，无 `..` 条目；路径上限 256 B，单次 I/O 上限 4096 B。
- 内置应用是扁平 NUC 镜像、编进内核；`no_std`、无分配器、无第三方依赖；单 blob < 128 KiB。
- `nsh` 是窗口化终端（144×26 字符网格），引擎在 `apps/src/shell/`，35 条内建命令。

### 1.2 真实源码地图（与 README 早期规划不同）

```text
arch/x86_64/       UEFI 入口、boot stage3、内核入口、AP 跳板、汇编异常桩
kernel/            调度/系统调用/NUC/内存/中断/网络/存储/USB/PCI/Win32-PE/安装/会话
fs/                VFS、NovusFS(WAL+Merkle)、FAT、ISO9660、ext4、挂载表、rootfs 布局
security/          NKM 签名加载、TLS 1.2/1.3、novu 更新、CA bundle
graphics/          NovusDisplay、nvgui 控件、桌面壳层、字体/Unicode/IME/主题/壁纸
wm/novus-wm/       内核态合成器 + 窗口协议（nwm_proto.h 为协议真源）
apps/              Rust no_std 用户态运行时、9 个 bin、nsh、Nuomi、devmgr、dwm
templates/         GUI 应用 / nsh 命令 / Nuomi 脚本模板
user/              Nuomi_pl 参考实现、cppcc、win32compat、novusd、ssl 证书
tools/             主机端镜像/打包/签名/生成器 + ci.mk / tools.mk
scripts/           自检、QEMU 冒烟、安装/生命周期测试、镜像检查、CI 本地入口
configs/           真正生效的 defconfig（Kconfig 仅声明占位）
```

根目录**不存在** `system/`、`drivers/`、`user/shell/`、`user/micropython/`；驱动在 `kernel/`，shell 在 `apps/src/shell/`。

### 1.3 当前构建与产物

- `make` → `out/NOVUS.EFI`（PE32+，入口 `efi_main`，含 AP trampoline 与 Rust 静态库）
- `make iso` → `out/novus-live.iso`（ISO9660 + Joliet + El Torito + isohybrid；ESP 必须 < 32 MiB）
- `make disk-img` → GPT + ESP + NovusFS 整盘镜像（默认 64 MiB）
- `make tools` → `mkfat/mkiso/mkfsnovus/mkdisk/nkmgen/novupkg/wpkg` 等
- 生成器产物（主题、语言、字体、IME、壁纸包、CA、NKM 测试模块）已提交，改 TOML 后必须重生成

### 1.4 已落地能力总览

| 领域 | 状态 | 关键证据 |
|---|---|---|
| UEFI 引导、启动 splash、日志跨阶段继承 | [已完成] | `arch/x86_64/boot/main.c`、`stage3.c`、`kernel/klog.c`、`graphics/nd_boot.c` |
| A/B 两阶段启动、AP 唤醒 | [部分] | `kernel/smp.c`、`arch/x86_64/kernel/ap_trampoline.S`；多 AP 未做 |
| PMM/MM/堆/每进程页表/COW | [已完成] | `kernel/pmm.c`、`mm.c`、`heap.c`、`vm.c` |
| GDT/IDT/IRQ/LAPIC、抢占调度 | [已完成] | `kernel/idt.c`、`irq.c`、`sched.c`、`trap.c` |
| syscall ABI（1–7/60 + 8–19/40/DWM） | [部分] | `kernel/trap.c` 的 `g_syscalls[]`；CAP 未接入 |
| NUC 加载器（ring3、重定位、隔离） | [部分] | `kernel/nuc.c`；无签名、段 flags 未严格落实 W^X |
| IPC + Novus-WM + Mini-DWM 软件合成 | [部分] | `kernel/ipc.c`、`wm/novus-wm/nwm.c`、`apps/src/bin/dwm.rs` |
| NovusFS（WAL + Merkle + fsck 优先级） | [部分] | `fs/novusfs/*`；WAL 错误仅 WARN，非严格崩溃一致性 |
| VFS 多挂载点、FAT/ISO/ext4 只读 | [部分] | `fs/vfs_mount.c`、`fs/ext4/*`；NTFS 仅枚举值 |
| 自研 TLS 1.2/1.3 + CA + 更新服务 | [部分] | `security/tls/*`、`security/novu/*`；非完整 WebPKI |
| NKM Ed25519 签名加载 | [部分] | `security/nkm/nkm.c`；仅开发公钥、无轮换 |
| 网络栈与 4 种 QEMU 网卡 | [部分] | `kernel/net*.c`；单 TCP、轮询收包、WiFi 仅识别 |
| xHCI 大容量存储、HID、触摸板 | [部分] | `kernel/usb.c`、`hid.c`；BOT 非 UASP，EHCI 不支持 |
| 8+ 内置 GUI 应用、nsh、Nuomi、设备管理器 | [部分] | `apps/src/bin/*`、`apps/src/shell/*`、`apps/src/nuomi.rs` |
| 图形安装向导、多语言、会话分流 | [部分] | `apps/src/bin/setup.rs`、`kernel/install.c`、`kernel/lang.c` |
| Win32 PE32+ 原型 | [部分] | `kernel/pe.c`、`pe32.c`、`wow32.c`、`win32.c` |
| 自检/QEMU 冒烟/基线比对 | [部分] | `scripts/*`；无托管 CI，多人工检查 |

---

## 2. 里程碑全景（M0–M12 与后续）

| 里程碑 | 主题 | 状态 | 尚未闭环的部分 |
|---|---|---|---|
| M0 | 最小 UEFI 引导、A 核 BSP、VGA 文本 | [已完成] | — |
| M1 | 基础内存管理、分页、SMP 唤醒 B 核 | [已完成] | 多 AP 枚举、完整双核语义（规范列为 M4+） |
| M2 | NovusFS 最小版本 + WAL | [已完成]（代码） | `.trae/specs/implement-m2-novusfs-wal/` checklist 未回填 |
| M3 | Merkle 校验、NKM 框架、ISO Live、安装器 | [已完成] | 密钥轮换、防回滚 |
| M4 | 基础调度器、进程创建、基础 syscall | [已完成] | CAP、W^X、`exec`、真并发管道 |
| M5 | NovusDisplay 基础 2D + 控件库 | [已完成]（基础） | 硬件 2D、OpenGL/Vulkan/DX 系列 |
| M6 | Novus-WM + IPC 协议 | [已完成] | 共享内存路径、强鉴权、协议文档升版到 2.0 |
| M7 | cppcc C++98 子集 + NUC 后端 | [部分] | C++11 阶段 2/3、多 TU、通用 `./app.nuc` |
| M8 | 内置系统应用 | [已完成] | 设置项不持久化（FS 只读约束） |
| M9 | Win32 兼容层原型 + Win11 中文桌面 | [部分] | EXE 任意路径、32 位 PE32、DLL/TLS/SEH、完整沙箱 |
| M10 | 全系统整合、镜像打包、Fluent 主题、图形安装向导 | [部分] | NTFS/ext4 安装、USB 集线器/UASP、安装后 ESP 载荷 |
| M11 | 多语言、会话分流、I/O 阶段 1 | [部分] | USB 批量传输完善、多显示器独立桌面、原生显示驱动 |
| M12 | 设备管理器 + 模态属性窗 + devtab | [已完成] | `power_state` 多数驱动为空、休眠阈值无用户入口 |
| 未编号 | 硬件 2D 引擎 / 原生 DP-eDP-HDMI / NTFS+ext4 写 / Mini-DWM 硬件后端 | [未实现] | 见 §5 Phase 3 |

---

## 3. 已完成能力详表

### 3.1 启动、内存、进程

- **引导**：`efi_main` → 串口/控制台 → 内存自检（<2 GiB 拒绝）→ `stage3_prepare/menu/load/finish` → `ExitBootServices`；固件 FAT 失败时回退内置 FAT32 读取器（`stage3.c:294-297`）。
- **启动画面**：固件日志经共享内存 4 KiB 环传入 splash 的半透明日志面板；内核继承后追加到 32 KiB 环（`kernel/klog.c`、`graphics/nd_boot.c`）。
- **内存**：`pmm.c` 固定 256 KiB 位图（约 8 GiB）；`kmalloc` 页粒度、256 MiB 共享堆窗口；`vm.c` 每进程 CR3/COW；`mm.c` 修复真机花屏（帧缓冲统一 UC 映射 + `wbinvd`）。
- **调度**：`SCHED_MAX_TASKS=10`、每任务 64 KiB ring-0 栈、默认 4 tick 时间片；`sched_create_user_ex()` 支持 ABI 白名单；用户态异常只终止该进程。
- **syscall**：`rax` 调用号、`rdi/rsi/rdx/r10` 传参；8 `open`、9 `read`、10 `fs_write`、11 `close`、12 `stat`、13 `opendir`、14 `readdir`、15 `mkdir`、16 `power_off`、17 `reboot`、18 `session`、19 `SYS_SETUP`、40 `SYS_WIN32`、60 `SYS_EXIT`；`ipc_recv` 依赖 RIP 回退 2 字节重放。

### 3.2 文件系统与存储

- NovusFS V3（`0x00030000`）：4 KiB 块、双 superblock、1024 inode、56 B 文件名、7 直 + 单/双间接；WAL 64 块 CRC-32 + commit + replay；Merkle 64 B 节点/每块 64 节点，覆盖 bitmap+inode+data。
- VFS 单根 + 8 项挂载表；6 条 `mounts.json` 白名单；FAT12/16/32、ISO9660/Joliet、ext4 只读后端；rootfs 布局 78 目录（唯一真源 `rootfs_layout.h`）。
- 块层统一 512 B 扇区；AHCI / NVMe / IDE(PIO) / USB BOT / GPT / devtab 已接入；`qtest.sh` 覆盖四类磁盘。
- 已修复真机显示/存储相关问题：帧缓冲 WB/UC 别名、壁纸 JPEG 解码偏色（改为程序化渐变）、`SHARED_MEM` 缺失字段导致的链接失败。

### 3.3 安全与更新

- NKM V1.0：208 B 头、SHA-256（`[0x50,0xB0)` 置零）、Ed25519 签摘要、最多 4 根公钥/16 模块、仅 `R_X86_64_64` 重定位。
- 自研 TLS 1.3 1-RTT / TLS 1.2 ECDHE+AEAD、X.509 链校验、AES-GCM/ChaCha20-Poly1305、P-256/X25519/RSA、CSPRNG。
- novu 更新：128 B 冻结头、四魔数、三类补丁（`.hotpth/.npth/.apth`）+ 全量 `.novu`、五级优先队列、8 步校验、快照+原子替换+逆序回滚+Merkle 重建、持久 journal；`updatesrv` 提供 HTTP/可选 HTTPS。
- NUC 不签名，靠页表隔离；Win32 ABI 白名单（`SYS_WIN32`/`SYS_EXIT` 除外全部拒绝）。

### 3.4 图形、窗口与输入

- 帧缓冲 HAL：最多 4 路 GOP 输出、EDID 首选时序、主屏 + 其余镜像；离屏合成 → 整帧呈现；光标合成覆盖层；VGA `0x3DA` 回扫探测 + 定时器兜底。
- Unicode 全量点阵：114,397 字形、3.5 MB `nd_font_data.bin`、稀疏页索引；严格 UTF-8；`Ctrl+Shift+U` 码点直输；`Ctrl+Space` 拼音 IME（单音节、≤6 候选）；TTF 轮廓字体（仅 glyf）。
- 主题令牌（浅色/深色/跟随时间）、中英双语即时切换与持久化、壁纸包 `NWPK` v1（当前实际使用渐变回退）。
- nvgui 固定池 192 控件；WM 协议 2.0（surface redirection / Mini-DWM），10 窗、120 图元/160 文字、帧尾 `INVALIDATE` 提交；事件路由与前台权重。

### 3.5 网络与外设

- ARP/IP/UDP/网关转发；单 TCP 客户端；DNS 单事务 A 查询（固定源端口 5353）；DHCP；HTTP + TLS；NTP；NCSI 联网检测。
- 网卡：e1000/e1000e、I21x、igb（真机 82576+）、virtio-net（legacy）、rtl8139 均跑通 DHCP/DNS/NCSI；统一 PCI ID 身份表。
- USB：xHCI 1.1（命令/事件环、slot/地址、BOT CBW/CSW、READ10/WRITE10）、UHCI HID boot protocol、HID/触摸板；`sysinfo` 暴露设备树。

### 3.6 用户态、语言与应用

- `apps/src/bin/`：`fileman`、`taskman`、`settings`、`netman`、`session`、`setup`、`nsh`、`devmgmt`、`dwm`；另有 `novusd`（来自 `user/novusd`）。
- `nsh`：35 条内建命令（`ls cd pwd cat echo mkdir rmdir rm cp mv stat env export unset clear help history true false whoami which touch wc head tail tree alias drives mount umount nuomi update novusd run install`）；管道、重定向、`$(...)`、变量、glob、历史、别名、`; && ||`。
- Nuomi：`apps/src/nuomi.rs` 是 `no_std` 无堆移植（256 KiB arena / 6 KiB 源码 / 约 500 层递归）；`user/Nuomi_pl/` 是带 GC 的宿主 C 参考实现与 NUC 工具链。
- cppcc：主机端 Rust C++ 子集编译器，NUC V1.0 / Win64 PE32+ / Linux ELF64 三后端；阶段 2（C++11 核心）进行中。
- novusd：6 阶段交付（检测核心、Rust 平台层、CLI、隔离、更新、E2E）；设备内能力降级（文件事件/进程追踪/审计/沙盒/argv）。
- Win32：PE32+ 映射到 `0x50000000`，DIR64 重定位、按名 IAT、32 B syscall thunk；首批 15 个 API + 后续 GUI/ws2_32 原型；注册表持久化。

### 3.7 工具与验证

- 主机工具：`mkfat/mkiso/mkfsnovus/mkdisk/nkmgen/novupkg/wpkg/fontgen/langgen/themegen/imegen/wpgen/cabundle/updatesrv/tlstest`。
- 自检：`display/irq/sched/dwm/memory` selftest；`qemu-smoke.sh` + VNC 基线比对；`ci_smoke.py`；`run-install-test.sh`；`wm-churn.sh`；`check_rootfs.py`；`inspect_img.py`。
- 本地 CI 入口：`make check` / `check-lint` / `smoke` / `pipeline`（`tools/ci.mk`）；无托管 CI。

---

## 4. 差距分析（Gap Analysis）

> 每条给出**证据**与**影响**。优先级定义：P0=发布前必须；P1=下一里程碑；P2=中期；P3=长期愿景。

### 4.1 正确性与一致性

| ID | 缺口与证据 | 影响 | 优先级 |
|---|---|---|---|
| G-C01 | 文档/代码漂移 12 项（§0.3）；`changes.md:324` 与 `:328-331` 自相矛盾 | 把规划当已完成，验收标准失真 | **P0** |
| G-C02 | 更新包头 128B（内核）vs 144B（`novupkg`） | 跨端契约不一致，更新包可能被拒 | **P0** |
| G-C03 | `rootfs_layout.h:109`=78 vs `fsinfo`/`check_rootfs.py:28`=77 | 镜像校验与真实布局不匹配 | **P0** |
| G-C04 | 脚本硬编码 `/home/dev/*` 与固定 OVMF 路径（`qtest.sh:3`、`ci_common.py:19` 等） | 换机即失效，无法上 CI | **P0** |
| G-C05 | JPEG 壁纸解码器内核侧输出错误（平均≈0x2C），已改渐变回退（`changes.md:227-235`） | 照片壁纸功能实际不可用 | P1 |
| G-C06 | `nsh` 的 QEMU 交互验证未做（`changes.md:213`）；安装后关机/重启未端到端验证（存储文档） | 交互路径缺回归 | P1 |
| G-C07 | 设置页更改不持久化（FS 只读约束，`M8 内置系统应用.md:112-113`） | 用户体验不完整 | P1 |
| G-C08 | `Kconfig` 占位、`CONFIG_USER_MOCHI=y` 但 `user/mochi` 不存在、部分选项仅用于 banner | 配置系统与源码不一致 | P1 |
| G-C09 | M2 spec checklist 未回填、`.codebuddy/plans/...f58dfc52(未完成)` 仍有 pending 的"完成 AB 核部分功能" | 过程资料与实现状态脱节 | P1 |
| G-C10 | `qtest.sh`/`rebuild-kernel.sh` 用 `|| true` 吞失败；基线需人工核对 | 回归可能假绿 | P1 |

### 4.2 安全

| ID | 缺口与证据 | 影响 | 优先级 |
|---|---|---|---|
| G-S01 | CAP 128 位模型只有规范，`g_syscalls[]` 无 `cap_check`（`SECURITY.md:137-153`、`CONTRIBUTING.md:313`） | 无法做最小权限授权 | **P0/P1** |
| G-S02 | IPC 端口无强鉴权，普通用户态可调用 IPC（`SECURITY.md:64-69,149`） | WM/GUI 不是安全边界 | P1 |
| G-S03 | NKM 仅一把开发公钥、无运行时导入/轮换/吊销（`nkm.h:17`、`SECURITY.md:114`） | 无法生产部署密钥治理 | P1 |
| G-S04 | 无 Secure Boot / 签名固件链（`SECURITY.md:48`） | 固件层攻击无防护 | P2 |
| G-S05 | TLS 非完整 WebPKI：无 CRL/OCSP、名称约束、keyUsage 全检、客户端证书、HTTP/2、QUIC、0-RTT（`SECURITY.md:123`；更新规范:557-571） | 不能声称与任意公网更新服务安全通信 | P1 |
| G-S06 | CSPRNG 有确定性回退、无硬件 TRNG（`SECURITY.md:124,152`） | 密钥材料质量受限 | P2 |
| G-S07 | NUC 段 flags 未落实 W^X（`cppcc README.md:71`）；`nuc.c` 给非 TEXT 段写权限 | 代码注入面 | P1 |
| G-S08 | Merkle 不覆盖 WAL/自身、WAL replay 非严格 gate，非完整防回滚/崩溃一致性（`SECURITY.md:50`） | 掉电一致性保证有限 | P2 |
| G-S09 | Win32 非完整沙箱：无完整进程隔离/DLL 安全加载/SEH/线程/文件权限（`SECURITY.md:83-89`） | 不可运行不可信 EXE | P2 |
| G-S10 | 更新中心：智能时段调度、灰度放量、兼容拉黑、服务端上报未实现（更新规范:439-579） | 管理体系缺闭环 | P1/P2 |
| G-S11 | 开发/测试公钥硬编码：`apps/src/shell/update.rs:15-16`、`novu_verify.h:30-32`、`NKM_SEED` | 出货前必须替换 | **P0** |

### 4.3 功能缺口

| ID | 缺口与证据 | 影响 | 优先级 |
|---|---|---|---|
| G-F01 | 通用 `./app.nuc` exec 未实现；仅 boot-stage 固定 `APPS/DEMO.NUC`（`README.md:212,239`） | 无法运行任意 NUC | **P0/P1** |
| G-F02 | `nsh` 缺 `grep/find/sed/awk/sort/kill/ps/date/uname/man` 等；`help <name>` 无详情（`cmds.rs:876,1496-1532`） | 终端实用性不足 | P1 |
| G-F03 | 管道与 `$(...)` 顺序捕获，非真并发；无 fork/exec（`终端与命令解释器设计.md:21,123-131`） | 性能与语义受限 | P2 |
| G-F04 | 文件 API 不支持 seek/truncate/unlink 与 owner/rwx；跨后端 rename 缺失（`用户会话与文件API.md:20,30`） | 应用能力受限 | P1 |
| G-F05 | 安装器只支持 NovusFS；NTFS/ext4 仅 ABI 占位；不支持 USB 集线器/UAS/热插拔/USB3 流；安装后 ESP 无 `ESP.IMG`（`图形安装向导...md:239-247`） | 目标磁盘选择受限 | P1 |
| G-F06 | novusd 设备侧 5 项能力降级，缺内核文件事件/审计源/强制沙盒（`NOVUS_PORTING.md:32-51`） | 主动防护弱 | P1/P2 |
| G-F07 | cppcc 阶段 2/3 未完：类/继承/虚表/模板/多 TU/constexpr/异常/RTTI/new/浮点/STL（`C++11 支持矩阵.md:35-45`） | C++ 生态受限 | P2 |
| G-F08 | Win32：磁盘 EXE、PE32 32 位、16 位 COM/NE/LE、序号/延迟导入、真实 DLL、TLS、多线程、SEH 未实现（`M9 Win32 兼容层原型.md:32-45,620`） | 原型边界 | P2/P3 |
| G-F09 | Nuomi 宿主实现缺 HTTP/进程（非 Windows）、ZIP、SHA-1、并发下载、GUI 窗口；宏/模块/异常未实现（`Nuomi_pl/README.md:225-236`） | 语言生态受限 | P2/P3 |
| G-F10 | MicroPython 仅 README 路线图，无源码/构建目标；Mochi 子模块缺失（`CONTRIBUTING.md:51,114,421`） | 规划未落地 | P3 |
| G-F11 | C++98 STL/异常测试移植：122 个导入 API 仅约 20 个实现，85 个 `msvcrt.dll` 全缺（`C++98 测试程序移植说明.md:13-15`） | 兼容性非常有限 | P3 |
| G-F12 | Debug 手册声明的 `log_print`/`panic_handler`/运行时 assert/日志持久化均不存在（`Debug 接口手册 V1.0.md:612-619`） | 调试/可靠性能力缺口 | P1 |

### 4.4 硬件与性能

| ID | 缺口与证据 | 影响 | 优先级 |
|---|---|---|---|
| G-H01 | 3 家厂商 GPU 2D 入口全返回 `ENODEV`；仅 CPU 后端（`GPU 2D 与帧缓冲映射.md:15-23`、`gpu2d.h:6-15`、`novus_defconfig:94`） | 无硬件加速 | P2 |
| G-H02 | Mini-DWM 硬件后端未实现（`Kconfig:229-236`） | 合成靠 CPU | P2 |
| G-H03 | 无硬件 flip/vblank 中断，仅 VGA 探测 + 800 spins 有界等待（`nd_hal.c:57-59`、`changes.md:244-247`） | 撕裂未根治 | P2 |
| G-H04 | 多输出仅镜像，非独立桌面；原生 DP/eDP/HDMI 精确区分属后续里程碑（`多语言、会话与 M11 输入输出.md:123-124`） | 多显示器体验有限 | P2 |
| G-H05 | 存储能力缺：AHCI NCQ/多 slot/IRQ 完成、NVMe PRP list 与更深队列（现 4）、IDE DMA（现 PIO）、USB BOT reset recovery、ATAPI、HDD `power_state`（存储驱动文档:62-69） | 吞吐/兼容/休眠受限 | P2 |
| G-H06 | 网卡缺：igc(I225/I226)、r8168/8125、vmxnet3/ne2k_pci/pcnet、virtio 1.0-only；igb 在 QEMU 下因不置 LU 收不到包（`changes.md:54,81-82`） | 真机网卡覆盖有限 | P2 |
| G-H07 | WiFi 仅识别命名，无固件加载/802.11/WPA2 关联（`net_wifi.c:78`） | 无线不可用 | P2/P3 |
| G-H08 | USB EHCI-only 高速设备不支持，xHCI 磁盘仅 BOT 非 UASP（`uhci.h:8-11`、`usb.c:2103-2105`） | 老设备/高速存储受限 | P2 |
| G-H09 | 网络单 TCP 会话、无窗口缩放/选项/拥塞控制、1s 重传 3 次、DNS 单事务、收包轮询（`net_tcp.c:2-10`、`net.h:130-131`、`net.c:522-524`） | 不能作为通用网络栈 | P2 |
| G-H10 | SMP 默认启用但只有 A/B 两核，SIPI 失败降级单核；多 AP 未实现（`smp.c:34-36,184-192`） | 多核扩展受限 | P2 |
| G-H11 | WC 真机吞吐未测、帧预算未系统化（`GPU 2D 文档:66-70`） | 性能基线缺失 | P2 |
| G-H12 | 无 OpenGL/OpenCL/Vulkan/DX11/DX12 实现（README:313 为规范宣称；代码中零命中） | 3D/高性能图形不可用 | P3 |

---

## 5. 分阶段路线图

> 阶段不是发布时间承诺，而是依赖顺序。每阶段设"退出条件"，未满足不得进入下一阶段。
>
> 任务表的"证据/现状"列引用 §4 的缺口 ID 或源码位置；"验收"列是可以 executed 的检查，不写形容词。

### Phase 0 — 基线加固与发布前收口（P0）

**退出条件**：文档与代码口径一致；更新契约统一；硬编码密钥/路径清零；所有必跑检查可在干净环境执行。

| ID | 任务 | 证据/现状 | 交付物与验收 | 依赖 |
|---|---|---|---|---|
| P0-01 | 文档勘误：WM 2.0/10 窗、最低内存、NKM 208B、目录数 78、多显示器、Win32 边界、NUC 栈、M10 状态 | §0.3、G-C01 | 提交勘误 PR；`README/docs/changes` 与代码常量逐项对齐；在 `CONTRIBUTING.md` 注明优先级规则 | — |
| P0-02 | 统一更新包头契约（128B vs 144B） | G-C02 | 内核/`novupkg`/规范三方同一 `HDR`；端到端打一个 `.novu` 包并 `SYS_UPDATE` 校验通过 | — |
| P0-03 | 根除硬编码开发密钥与测试锚 | G-S11 | 生产密钥从证书/密钥存储加载；`apps/src/shell/update.rs`、`novu_verify.h`、`NKM_SEED` 无生产使用；E2E 证书与生产 CA 隔离 | P0-02 |
| P0-04 | 同步 rootfs 目录计数与校验工具 | G-C03 | `fsinfo`、`check_rootfs.py`、mkfat/mkiso 与 `rootfs_layout.h` 一致；`make check-rootfs` 通过 | — |
| P0-05 | 脚本去个人路径，统一可移植入口 | G-C04 | `qtest.sh`/`scripts/*` 默认从环境变量或仓库相对路径取 OVMF/QEMU；`CONTRIBUTING.md:242-249` 清单清空 | — |
| P0-06 | 确定 JPEG 壁纸解码器去留 | G-C05 | 要么修好工具链 UB 并用 QEMU 截图验证照片壁纸，要么删除死代码与资源；不得留"静默回退" | — |
| P0-07 | 补关键交互回归 | G-C06 | QEMU 中打开 `nsh` 执行 `nuomi -e`、安装后关机/重启、VNC 建账户/开始菜单各一条自动化或半自动脚本 | P0-05 |
| P0-08 | 配置系统清理 | G-C08 | 移除/补齐 `user/mochi` 悬空配置；无代码开关的选项转成宏或标注 banner-only；`Kconfig` 与 defconfig 一一对应 | — |
| P0-09 | 过程资料收口 | G-C09 | 回填 M2 spec checklist；处理唯一 pending 的 AB 核 todo（完成或明确关闭） | — |
| P0-10 | 回归门禁去"假绿" | G-C10 | 移除 `|| true` 吞失败；基线更新需显式 flag 且在 PR 中附人工核验记录 | P0-05 |

### Phase 1 — 功能闭环与开发者体验（P1）

**退出条件**：任意 NUC 可从 shell 启动；终端具备日常可用性；更新/安装/安全守护形成闭环；调试接口落地。

| ID | 任务 | 证据/现状 | 交付物与验收 | 依赖 |
|---|---|---|---|---|
| P1-01 | 通用 `./app.nuc` exec | G-F01 | `nsh` 可从 VFS 任意路径加载 NUC；`cppcc-demo` 产物不再只能占 `APPS/DEMO.NUC`；保留 boot-stage 固定槽作为回退 | P0-02 |
| P1-02 | NUC 段保护 W^X | G-S07 | loader 按 segment flags 设置页权限（TEXT RX、非 TEXT 无 X）；`nucdump`/selftest 校验 RW 段不可执行 | — |
| P1-03 | `nsh` 常用命令补齐 | G-F02 | 至少 `grep/find/sort/head/tail/wc/date/uname/ps/kill/sleep/printf/test/exit`；`help <name>` 输出参数说明；新增命令走 `templates/shell-command/` | P1-01 |
| P1-04 | shell 交互增强 | G-F03 | Tab 补全、Ctrl+U/K、历史增量搜索；别名包持久化到用户配置；评估并发管道可行性 | P1-03 |
| P1-05 | 文件 API 补齐 | G-F04 | `seek/truncate/unlink` 与 owner/rwx 模型设计落地；ext4 适配器支持 rename；权限不能只靠登录门禁 | P1-01 |
| P1-06 | CAP 最小权限模型落地 | G-S01 | `SYS_CAP`/`cap_check()` 进入 `g_syscalls[]` 与用户头；进程默认最小权限；系统应用显式授权；IPC 端口可基于 CAP 鉴权 | P1-05 |
| P1-07 | IPC 端口强鉴权 | G-S02 | 端口注册/查找带能力校验；普通应用不能监听或发往系统端口；WM 作为唯一 `"nwm"` 服务端 | P1-06 |
| P1-08 | NKM 多根密钥与轮换 | G-S03 | `key_id` 或等效机制、运行时导入/吊销接口（管理员会话授权）；禁止持久化 `NKM_FLAG_ALLOW_UNSIGNED` | P0-03 |
| P1-09 | TLS/WebPKI 关键补齐 | G-S05 | CRL/OCSP（硬失败策略）、名称约束、路径长度、keyUsage 全检；`SYS_HTTPS` 公网主机名路径端到端验证 | P0-03 |
| P1-10 | 更新中心策略闭环 | G-S10 | 智能空闲时段/免打扰调度、失败梯度重试与兼容拉黑、服务端百分比灰度与上报；设置页加入详情/历史/策略子页 | P0-02、P1-09 |
| P1-11 | 安装器能力补齐 | G-F05 | NTFS/ext4 写目标（或明确删除"规划中"选项）；USB 集线器/UASP/热插拔/USB3 流；安装后 ESP 载荷可用 | P2 硬件驱动前置 |
| P1-12 | novusd 设备内能力恢复 | G-F06 | 内核文件事件通知或回调、进程/系统调用审计事件源、内核强制沙盒、命令 argv 通道；5 项降级逐一关闭 | P1-06 |
| P1-13 | 调试与日志接口落地 | G-F12 | `log_print`/分级过滤/`panic_handler`/运行时 assert/日志落盘；更新 Debug 手册 12.0 差异声明 | — |
| P1-14 | 设置项持久化 | G-C07 | 在可写 NovusFS 上定义用户配置区，主题/语言/壁纸/快捷键可持久化；不破坏 Merkle | P1-05 |
| P1-15 | 图形安装器剩余体验 | §2 M10/M11 | A 核开关/内存模式/显示输出卡片与真实内核设置联动；安装后首次建账户流程回归 | P0-07 |

### Phase 2 — 安全强化与可靠性（P1/P2）

**退出条件**：能明确界定"可用于生产"的安全边界；掉电/崩溃行为有可验证保证。

| ID | 任务 | 证据/现状 | 交付物与验收 | 依赖 |
|---|---|---|---|---|
| P2-01 | Secure Boot / 签名固件链 | G-S04 | 固件→引导程序→内核→模块的信任链；未签名阶段失败有 VGA 报错；不破坏现有 no-emulation ISO 启动 | P1-08 |
| P2-02 | CSPRNG / 硬件 TRNG | G-S06 | 移除确定性回退或仅在自检环境显式启用；接入可信熵源并在 `SECURITY.md` 更新表述 | — |
| P2-03 | Merkle/WAL 崩溃一致性 | G-S08 | WAL 重放成为 superblock 刷新的严格前置；防回滚计数；fsck 三优先级自动化用例 | — |
| P2-04 | Win32 沙箱加固 | G-S09 | 完整进程隔离、DLL 安全加载、SEH、线程与文件权限模型；不可信 EXE 默认拒绝 | P1-06 |
| P2-05 | 用户态异常报告持久化 | §4.2 | 非法访问/除零/非法指令报告写入 NovusFS 日志分区并显示 GUI 弹窗 | P1-13 |
| P2-06 | 无人值守回归矩阵 | G-C10 | `make pipeline` 覆盖 display/irq/dwm/rootfs/smoke/install/wm-churn；失败即非零退出，产物写入 `out/ci/` | P0-05、P0-10 |

### Phase 3 — 硬件、图形与性能（P2）

**退出条件**：真机（非 QEMU）达到可用帧率与显示正确性；主要硬件类别有驱动覆盖。

| ID | 任务 | 证据/现状 | 交付物与验收 | 依赖 |
|---|---|---|---|---|
| P3-01 | 厂商 GPU 2D 后端 | G-H01 | Intel/AMD/NVIDIA 至少一家实现 GTT/ring/blit 并注册到 `gpu2d`；软件后端保留回退 | — |
| P3-02 | Mini-DWM 硬件合成后端 | G-H02 | `CONFIG_NOVUS_DWM_HW=y` 可用；软件/硬件路径截图一致 | P3-01 |
| P3-03 | 真 vblank / 硬件 flip | G-H03 | 使用 vblank 中断或 flip 链；去除定时器探测路径； tearing 有可测指标 | P3-01 |
| P3-04 | 多显示器独立桌面 | G-H04 | 每输出独立帧缓冲/工作区；镜像模式保留为选项；EDID 与 DP/eDP/HDMI 精确识别 | P3-01 |
| P3-05 | 存储驱动能力补齐 | G-H05 | AHCI NCQ/IRQ 完成、NVMe PRP/更深队列、IDE DMA、BOT reset recovery、ATAPI、各驱动 `power_state` 与用户休眠阈值 | — |
| P3-06 | 网卡覆盖扩展 | G-H06 | igc(I225/I226)、r8168/8125、vmxnet3/ne2k/pcnet、virtio 1.0；igb 在无 LU 平台改用可行判定 | — |
| P3-07 | 无线上线 | G-H07 | 固件加载、802.11 关联、WPA2、扫描/连接 UI；先明确自研边界 | P2-02 |
| P3-08 | USB 3 / UASP / EHCI | G-H08 | UASP、集线器、USB 3 流、EHCI-only 高速设备支持或明确拒绝路径 | — |
| P3-09 | 网络栈通用化 | G-H09 | 多 TCP 连接、窗口缩放、拥塞控制、监听端、DNS 并发查询、中断收包；保留 poll 兼容路径 | — |
| P3-10 | 多 AP SMP | G-H10 | 枚举并唤醒全部 AP、拓扑感知、负载均衡；失败降级策略保留 | — |
| P3-11 | 性能基线与剖析 | G-H11 | 真机 WC 吞吐、帧预算（图元/文字/呈现）、启动时间、内存占用形成基线数据并纳入回归 | P3-01、P2-06 |

### Phase 4 — 语言、兼容层与生态（P2/P3）

**退出条件**：开发者可以不读内核就能写出并分发应用；语言/兼容层边界清晰且文档化。

| ID | 任务 | 证据/现状 | 交付物与验收 | 依赖 |
|---|---|---|---|---|
| P4-01 | cppcc C++11 阶段 2 | G-F07 | 类成员/构造析构/访问控制、继承与虚表、模板实例化、完整重载解析、多 TU 链接、尾置返回、`constexpr` | P1-02 |
| P4-02 | cppcc 阶段 3 | G-F07 | 异常/RTTI、`new/delete`、浮点、最小 STL 兼容头与运行时；Win64 CRT 与 glibc PLT 完整支持 | P4-01 |
| P4-03 | NUC dump/调试工具 | `cppcc 构建与端到端验证.md:17` | `nucdump` 可对任意 NUC Dump 头/段/符号/重定位；纳入 cppcc 测试 | P1-01 |
| P4-04 | Nuomi 宿主能力补齐 | G-F09 | HTTP/进程/ZIP/SHA-1/并发下载/GUI 窗口；宏/模块/异常处理；注意自研边界 | P2-04 |
| P4-05 | Win32 兼容层扩展 | G-F08 | 磁盘 EXE 任意路径、PE32 32 位执行器、真实 DLL 映射、TLS/SEH、更多 API；每批 API 同步 thunk/白名单/注册表布局 | P2-04 |
| P4-06 | C++98 移植兼容补齐 | G-F11 | 122 个导入 API 的实现优先级表；`msvcrt.dll` 85 个逐项落地或明确不支持；异常处理嵌套与 `catch` 类型匹配 | P4-01 |
| P4-07 | MicroPython 运行时 | G-F10 | 先开 Issue 确定自研边界与 NUC 打包方式，再实现最小运行时 + GUI API | P1-05 |
| P4-08 | Mochi 工具链 | G-F10 | 补 `user/mochi` 子模块或正式移除配置；规范与实现二选一对齐 | — |
| P4-09 | 应用分发（NUP/INS）闭环 | §3 NUP/INS 规范冻结 | 核对消费端：安装包安装、快捷方式创建/解析是否已实现；缺口补齐并做端到端安装/卸载测试 | P1-01 |
| P4-10 | 模板与 SDK | `templates/README.md` | 新增 IPC/网络/更新/主题模板；每帧绘制预算、限制表与调试手段随代码更新 | P4-01 |

### Phase 5 — 长期愿景（P3）

| ID | 任务 | 说明 |
|---|---|---|
| P5-01 | 3D/高性能图形 API | OpenGL/OpenCL/Vulkan/DX11/DX12 子集；必须先有 P3-01/P3-02 的硬件通道 |
| P5-02 | 完整 OOM 与优先级回收 | 按进程优先级回收、用户弹窗提示，LowMem/Full 模式策略可配置 |
| P5-03 | 多用户与文件权限 | 在 P1-05 的 owner/rwx 上建立用户/组与会话隔离 |
| P5-04 | 应用商店与签名分发 | NUP 安装包 + 签名验证 + 灰度/回滚与更新体系打通 |
| P5-05 | 完整崩溃一致性文件系统 | WAL 严格 gate + Merkle 防回滚 + fsck 自动修复 + 日志持久化 |
| P5-06 | 平台化 CI 与发版 | 托管 CI、多机矩阵、可复现构建、发布分支与安全回溯 |

---

## 6. 冻结边界（改动必须走 ABI 评审）

| 规范 | 版本/冻结点 | 变更约束 |
|---|---|---|
| NUC 可执行格式 | V1.0，48 B 头、magic `NUC!`、ABI `0x0100` | 字段/语义变化需升 ABI 主版本并评审；layout 顺序 Header→Segment→Symbol→Reloc→String→Data 不变 |
| NKM 内核模块 | V1.0，头 208 B (`0xD0`)、SHA-256 清零 `[0x50,0xB0)`、Ed25519 签摘要 | 同步 `nkm.c` 静态断言、`nkmgen`、规范与测试夹具 |
| NUP 安装包 / INS 快捷方式 | V1.0，状态冻结；64 B 头 / INI 风格 UTF-8 | 任何扩展必须升 Version |
| Novus-WM IPC / 内核 IPC | 代码 `NWM_PROTO_VERSION=0x00020000`；kernel IPC magic `0x4E495043`，32 B 头、载荷 ≤128 B | 消息号/载荷布局不得私改；共享内存路径只能追加消息类型 |
| Debug 接口 | 文件名 V1.0、内容 V1.1，接口基线冻结 | 参数级与错误码级冻结；实现差异必须登记而非静默改变 |
| 更新补丁体系 | 头 128 B、四魔数、三类补丁 + `.novu`、五级通道永久冻结 | 与 `novupkg` 头长冲突先修契约（P0-02），不是改冻结定义 |
| 根目录布局 / 挂载 | 唯一真源 `fs/novusfs/rootfs_layout.h`；`user_mount_t` 96 B | 改布局要同步三套镜像与 `check_rootfs` |
| 共享内存 | 8 KiB @ `0x7E000`，NVSH v1.1，静态断言 | 新字段只能追加到 `0x2F0` reserved 尾部 |
| 系统调用 | `g_syscalls[]` 稀疏编号冻结 | 不复用/不重排；新增只追加；用户头文件同步 |

**ABI 变更 PR 必含**：规范文档、内核实现、宿主工具、测试夹具、版本号、兼容性说明六件套。

---

## 7. 测试与质量体系演进

| 阶段 | 目标 | 具体动作 |
|---|---|---|
| Phase 0 | 可复现 | 去硬编码路径；移除吞失败的 `|| true`；固定 OVMF/QEMU 通过参数注入；`make check-rootfs` 全绿 |
| Phase 1 | 可覆盖 | 新增 `nsh`/Nuomi 交互、安装后关机重启、`CAP` 拒绝用例、更新包头一致性用例；生成器 `--check` 纳入门禁 |
| Phase 2 | 可信任 | `make pipeline`（check → lint → build → smoke）成为合并门禁；截图基线人工核验流程化；安全修复先加"错误输入被拒绝"回归 |
| Phase 3 | 可量化 | 真机帧率/吞吐/启动时间基线；硬件矩阵（GPU/NIC/存储）自动枚举与结果 JSON |
| Phase 4 | 可发布 | 托管 CI、洁净重建、发布分支、安全修复回溯策略（或明确不承诺，见 `SECURITY.md:10-14`） |

现有回归入口（保持可用）：`display-selftest.sh`、`irq-selftest.sh`、`dwm-selftest.sh`、`qemu-smoke.sh`、`qemu_smoke_test.py`、`ci_smoke.py`、`run-install-test.sh`、`wm-churn.sh`、`check_rootfs.py`、`inspect_img.py`、`novupkg selftest`、`nkmgen selftest`、`tools/updatesrv/selftest.py`、`cargo test`（cppcc）。

---

## 8. 风险登记册

| 风险 | 触发条件 | 缓解 |
|---|---|---|
| 冻结 ABI 冲突 | 新功能要改 NUC/NKM/IPC 字段 | 优先追加字段/新消息号；必须改动则走 §6 六件套评审 |
| 工具链 UB | JPEG 解码器类问题（同代码不同编译环境结果不同） | 用宿主桩对拍 + 内核侧输出校验；不能用 QEMU 通过代替真机 |
| 真机花屏/卡顿 | 帧缓冲缓存属性、WB/UC 别名 | 坚持 `MM_MAP_UC` + `wbinvd`；真机验证不可由 QEMU 代替 |
| 更新包无法应用 | 128B/144B 头冲突、测试根污染生产 CA | P0-02/P0-03 前置；E2E 与生产 CA 物理隔离 |
| 文档过时误导实现 | README 保留早期规划（system/、drivers/、MicroPython） | 以 `CONTRIBUTING.md:7` 的优先级规则为准；每次实现变更同步勘误 |
| 安全承诺过度 | 把 CAP/Secure Boot/WebPKI 当成已实现 | `SECURITY.md` 是安全口径真源；PR 不得放宽检查 |
| 基线漂移 | 截图基线被随意 `--update-baseline` | 普通回归禁用；更新需人工逐张核验并记录 |
| 硬件厂商封闭 | GPU/无线固件无文档 | 软件后端/降级路径长期保留，不"假装能跑" |
| 资源上限突破 | blob >128 KiB、栈 >4 MiB、显示列表超额 | 构建期与运行期断言；`apps.stamp` 与 `nwm_proto.h` 常量为门禁 |
| 无分配器约束 | 在 `apps/` 引入 `Vec/String/Box` | 模板与 Code Review 检查；保持固定数组/栈缓冲 |

---

## 9. 本路线图的维护规则

1. **状态变更必须带证据**：从 [未实现]→[未闭环]→[部分]→[已完成] 的每次移动，PR 中必须附源码路径、构建/自检命令与输出片段。
2. **每完成一个阶段**，更新 §2 里程碑表、§4 缺口表（删除或降级）、§5 任务表（标记完成日期），并在 `changes.md` 追加"主题 → 路径级要点 → 验证"条目。
3. **不得把规范文本当实现**：任何"已实现"声明都要能在源码中找到函数/配置/构建目标；找不到就标 [未闭环] 或 [待核验]。
4. **P0 是发布阻塞项**：P0 未清零前不发版；P1/P2 按依赖顺序推进，允许并行但不得跳过退出条件。
5. **冻结格式变更不走路线图捷径**：必须先过 §6 的 ABI 评审与本文件版本升级。
6. **安全口径以 `SECURITY.md` 为准**：本路线图不得把"规范目标"写成"当前能力"。
7. **文档漂移清单（§0.3）随勘误收敛**：每一项关闭时记录"以哪份文件为准"，避免反复争论。

---

## 10. 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-11-27 | 首次全量盘点：覆盖全部源码目录、31 份 `docs/`、SECURITY/CONTRIBUTING/README/changes、`.trae/specs/` 与 `.codebuddy/plans/`；建立 M0–M12 状态表、12 项文档漂移清单、40+ 条差距条目与 P0–P5 分阶段任务。 |
