# GPU 硬件加速路线图（Intel Iris Xe / Gen12）

目标：把 `kernel/gpu2d.c` 的软件后端之外，真正接一条 Intel 核显的 2D 提交路径，让窗口合成、dock 动画、壁纸渐显这些整屏操作走 GPU，而不是 CPU 逐像素写显存。

现状（起点事实）：

- 绘制层只经过 `kernel/gpu2d.c` 的两个入口：`gpu2d_fill_span()`、`gpu2d_copy_row()`，所以接硬件只需在 `gpu2d.c` 里加一张 `ops` 表，`graphics/` 一行都不用改。
- `kernel/gpu.c` 已能枚举 PCI class 03h、选出"谁在扫描输出"的控制器、记录 MMIO / aperture / stolen 窗口。
- 帧缓冲目前走固件留下的线性 GOP framebuffer，通过 `mm_map_phys(..., MM_MAP_WC)` 映射；核显机器上它落在 iGPU 的 aperture 里，因此 GPU 本身能看到这块内存 —— 这是硬件 blit 成立的前提。
- 缺的是三层基础设施：**可用的 MMIO 映射**、**GTT（GPU 页表）**、**命令提交（ring + 批缓冲）**。
- 测试机：i5-1335U，Iris Xe 80EU（Raptor Lake-P，Gen12 LP）。QEMU 里没有 Intel 显卡，所有硬件路径只能靠真机 + 屏上状态行验证。

## 阶段与验收

### P0 事实采集（无代码风险）

- [ ] 真机上确认 `[GPU] controller … (scanout)` 命中的是 `8086:A7xx`。
- [ ] 记录 MMIO BAR0 物理地址（高于 4 GiB 时现路径会报 `has no reachable BAR 0`）。
- [ ] 记录帧缓冲物理地址、aperture 大小、stolen 基址。
- 验收：状态行能读出 `gpu=intel gen=12`，说明 GMD_ID 寄存器能读通。

### P1 MMIO 映射与 GT 识别（本次先做）

- [ ] `gpu.c` 用 `mm_map_phys(BAR0, 窗口, MM_MAP_UC)` 映射寄存器窗口，不再受"必须在 4 GiB 以下"的限制。
- [ ] 读 `GMD_ID`（偏移 `0xD8C`，arch = bit31:22，release = bit21:14，step = bit5:0）得到图形代际。
- [ ] 读 forcewake 应答寄存器（`0xD84` / `0xD88`），记录 GT/媒体域的唤醒状态。
- [ ] 失败必须优雅降级：映射失败或读回全 1，就退回现状（软件 2D），不能影响启动。
- 验收：`[GPU] intel mmio … GMD_ID=0x… gen=12 release=… step=…` 一行出现在日志/状态行。

### P2 GTT 与页表

- [ ] 解析 GTTMMADR（BAR2）与 GGTT 大小（`GTT_SIZE`，Gen12 通常 8 MiB PTE 覆盖 16 GiB）。
- [ ] 在 GGTT 里为帧缓冲建立 64 KiB 页的 PTE（物理地址已知，属性 RW + 本机内存）。
- [ ] 提供一个 `gtt_map(phys, bytes)` 返回 GPU 虚拟地址，供后续命令引用。
- 验收：可以从 ring 里对映射过的缓冲做一次 GPU 写，CPU 回读校验（见 P3）。

### P3 命令提交（ring）

- [ ] 分配 ring buffer（系统内存 + GGTT 映射），写 `RING_TAIL`/`RING_HEAD` 寄存器。
- [ ] 下发一条批处理：`MI_NOOP` + `MI_BATCH_BUFFER_END`，用 `ACTHD` / 中断或轮询判断完成。
- [ ] Gen12 需要先做 forcewake 申请（`0xA188` 一族）与 `MI_ARB_ON_OFF` 之类的最小初始化；只做能跑通批处理所需的部分。
- [ ] 提供 `gpu_submit(batch, bytes)` 同步接口 + 超时保护（超时就标记硬件不可用并退回软件）。
- 验收：批处理完成计数增加，且超时后系统仍正常绘制。

### P4 第一笔硬件 2D

- [ ] 用 blitter 引擎下发 `XY_COLOR_BLT`（纯色填充）写进 GTT 映射的缓冲。
- [ ] CPU 回读像素比对，逐像素等于预期颜色才算通过。
- [ ] 再验证 `XY_SRC_COPY_BLT`（块拷贝），覆盖 `gpu2d_copy_row()` 需要的场景。
- 验收：像素比对全等；失败时自动回退软件路径并打一行原因。

### P5 接进 gpu2d 接缝

- [ ] `gpu2d.c` 新增 `g_backend_intel`，实现 `fill_span` / `copy_row`。
- [ ] 只在"帧缓冲已被 GPU 可见（在 aperture 内）且 P3/P4 自检通过"时启用，其余一律软件。
- [ ] 每帧提交次数要合并：把一帧的 span 收进一条批处理，避免每行一次提交。
- 验收：桌面元素逐像素与软件版一致（截屏比对），帧耗时下降。

### P6 逐步接管

- [ ] 先接管离屏合成缓冲的整屏拷贝（交接动画、壁纸渐显）。
- [ ] 再接管窗口合成里的矩形填充。
- [ ] 保留软件后端作为永久兜底，`gpu2d_backend()` 报告实际在用的那一个。
- 验收：动画不掉帧、无撕裂；拔掉硬件路径（强制软件）画面完全一致。

### P7 正确性与性能

- [ ] 缓存一致性：GPU 写完到 CPU 读之间要 `PIPE_CONTROL`/flush，CPU 写后到 GPU 读之间要 clflush。
- [ ] 存疑项逐条记录：forcewake 超时、引擎 hang、GGTT 与 stolen 重叠。
- [ ] 真机上一组可复现的耗时数字（整屏 fill / 整屏 copy / 一帧合成）。
- 验收：连续跑 30 分钟无 hang；性能数字有前后对比。

## 已知风险

- **没有串口**：所有证据只能靠屏上状态行，因此每一阶段都必须自带一行可读状态。
- **Gen12 没有独立 BLT ring**：blit 命令要提交到合适的引擎，具体引擎与命令长度以真机实测为准；先按"能提交、能回读"最小闭环推进，不追求一次到位。
- **固件状态未知**：GOP 走后可能留下未清的 ring/上下文；P3 之前不做任何会改动显示管线的操作。
- **不能弄坏现在能用的画面**：每个阶段都必须保留软件兜底，硬件自检不过就不启用。
