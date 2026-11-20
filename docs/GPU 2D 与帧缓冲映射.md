# GPU 2D 与帧缓冲映射

本文记录 NovusDisplay 的 2D 绘制在真实硬件上到底怎么走、帧缓冲为什么按某种缓存属性映射，以及一块核显加一块独显的机器上系统按什么规则选显卡。所有结论都对应仓库里的代码，行号会变，以函数名为准。

## 一、2D 接缝：谁在画像素

`kernel/gpu2d.c` 是全系统唯一的 2D 后端入口，所有填充和拷贝都经过它：

- `gpu2d_fill_span()` —— 一行里的连续像素填充（窗口背景、标题栏、圆角内部）。
- `gpu2d_copy_row()` —— 一行像素拷贝（离屏合成缓冲刷到屏幕、窗口拖动）。
- `gpu2d_init()` —— 决定谁来做这两件事，在 `kmain` 里 `gpu_init()` 之后调用。

调用方只有一处：`graphics/nd_core.c` 的 `nd_span_raw()` 和 blit 路径。也就是说想给某家厂商接一个真正的硬件 2D 引擎，只需要在 `gpu2d.c` 里加一个 `ops` 表，绘制层一行都不用改。

**目前的实现只有软件后端**（`g_backend_soft`）：8 字节对齐时用 64 位成对写、4 连展开，把像素 store 次数压到最低。三家厂商的入口 `intel_init()` / `amd_init()` / `nvidia_init()` 全部返回 `ENODEV`，并在日志里写明缺什么：

| 厂商 | 缺什么才能做硬件 blit |
| --- | --- |
| Intel 核显 | GTT 地址空间 + 图形 ring（提交批缓冲）+ blit 引擎命令流 |
| AMD | 图形 ring + 中断环（IH）+ 显示核心的 blit 包 |
| NVIDIA 独显 | GSP 固件装载 + 通道（channel）协商 + 显存对象分配 |

这不是占位，是事实：没有这些基础设施就没有可以下发的命令流，硬做只能"假装能跑"，结果就是静默花屏。所以入口宁可 `ENODEV`。

## 二、帧缓冲的缓存属性：为什么不是 UC

固件把帧缓冲交给我们时，最省事的映射是 UC（uncacheable）。在核显上 UC 只是慢一点点，因为帧缓冲就在内存里；但在独显上帧缓冲在 VRAM aperture，每一次 32 位像素写都是一次 PCIe 事务，一个 1024×768 的整屏重绘就是约 300 万次事务 —— 桌面会卡到不可用。

正确的属性是 WC（write-combining）：读仍然不缓存，写则先在 CPU 的写合并缓冲里攒满一条 cache line 再一次性发出去。`nd_hal.c` 的 `nd_map_framebuffer()` 因此请求 `MM_MAP_WC`，由 `kernel/mm.c` 的 `mm_map_phys()` 决定怎么兑现：

1. **PAT 路径（首选）**：`CR4.PAT` 已置位且 `IA32_PAT` 是标准布局时，PTE 的 bit12（PAT）选 IA32_PAT 第 4 项，也就是 WC。这是纯页表操作，最干净。
2. **MTRR 路径（兜底）**：`CR4.PAT` 没置位时，用一条可变 MTRR 把帧缓冲物理区间标成 WC，页表保持 cacheable。内存类型是"MTRR 类型 ∩ 页属性"，所以效果一样是 WC。`mm_mtrr_wc_cover()` 的做法是：`IA32_MTRRCAP` 里没有 WC 位或者没有可变 MTRR 就直接放弃；已有 MTRR 已经把区间整段覆盖且类型是 WC 就白拿；只部分重叠就放弃（不去和固件抢地盘）；有空槽才写一条。
3. **UC 降级**：上面两条都不成立，就老老实实 UC，并在串口打一行说明。慢，但正确。

**为什么不在早期直接写 `CR4.PAT`。** 第一版就是这么做的，在 QEMU（TCG）上整个系统在 `mov %rax,%cr4` 上卡死：TCG 在 CPUID 里报 PAT，却拒绝这条写指令，而那一刻 `idt_init()` 还没跑（它在 `sched.c` 里），没有 #GP 处理器可以接手，于是只剩"死循环"和"三重故障重启"两条路。所以现在只在固件**已经**置位 `CR4.PAT` 时才走页表路线，否则退到 MTRR —— 一条写 MSR 的路径，不会因为一个保留位把整台机器停住。

## 三、双显卡机器：选谁

`kernel/gpu.c` 的选卡规则是"**谁在扫描输出，就用谁**"，而不是"优先核显"或者"优先独显"：

1. 从固件拿到帧缓冲物理地址 `fb`。
2. `pci_scan` 列出所有 PCI 设备，先为**每个** class 0x03 的显示控制器打一行日志：厂商、设备、总线、BAR0/BAR2 与窗口大小，窗口包住 `fb` 的那个追加 `(scanout)`。
3. 谁的 BAR 窗口包含 `fb`，谁就是主显卡；`bar[]` 为 0 时改查 `bar64[]`（64 位 BAR 在 32 位字段里只放高位）。
4. 没有任何 BAR 命中才回退到"Intel/AMD 优先"的老规则。

这样一台 Intel 核显 + RTX 5070 的机器上，只要固件把帧缓冲交在 5070 的 aperture 里，系统就会把 5070 认成主卡；反过来把显示器插在主板视频口时，核显的 BAR 会命中，系统认核显。**同一份镜像两块卡都能起**，不需要两份配置。

已知约束：帧缓冲在 4 GiB 以上时，开机 splash 会跳过并打日志（`nd_boot.c` 的 `fb_phys >= 0x100000000` 分支），桌面本身仍然能画 —— 早期映射只覆盖低 4 GiB，这是 M1 阶段的身份映射限制。

## 四、在真机上怎么验证

串口（COM1，115200）会打出全部判断过程，按顺序找这几行：

```
[GPU] controller 8086:7D67 bus=0x0 bar0=... bar0_sz=... bar2=... bar2_sz=... (scanout)
[GPU] controller 10DE:2F04 bus=0x1 ...
[GPU] 显示控制器 vendor=... device=... mmio=... reg0=... (reachable)
[MM] PAT enabled: CR4.PAT set, IA32_PAT has the standard layout, write-combining maps are real
[GPU2D] backend=software (64-bit span stores; no vendor engine wired up)
```

- 两行 `controller` 说明两块卡都被枚举到，`(scanout)` 在谁身上就是谁在出图。
- `[MM] PAT enabled` 表示拿到了最好的那条路；`[MM] WC MTRR programmed` 表示走的 MTRR；`[MM] firmware left CR4.PAT clear` 加 `[MM] no variable MTRR with WC support` 表示降级到 UC（能跑，只是慢）。
- 没有串口线的机器：设置页「系统 → 显示输出 显示控制器」会显示被选中的厂商和分辨率，可以先从这里看选卡对不对。

## 五、还没做的事

- 三家的硬件 2D 引擎（见第一节表格）。要做 Intel 就得先有 GTT + ring，这是一个独立里程碑，不是给 `gpu2d.c` 加几行。
- 显存对象分配、垂直同步、多显示器枚举：现在只有一个输出，`gpu_summary()` 也只报一块。
- WC 在**真实硬件**上的实际吞吐还没有测过：QEMU/TCG 既没有 PAT 也没有可变 MTRR，本地只能在 UC 降级路径上验证"不崩、画面逐像素正常"。真机上的 `[MM]` 那一行是唯一的第一手证据。
