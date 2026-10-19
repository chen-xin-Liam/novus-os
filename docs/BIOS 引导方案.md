# Novus OS BIOS 引导方案

状态：MVP 初版已按本方案加入源码与构建目标，待 Linux 工具链和 QEMU 验收。当前实现偏离/限制：stage2 只选精确 1280×800×32 VBE 模式，不搜索最近似模式；BIOS E820 map 按当前 PMM 的 8 GiB bitmap 上限截取；BIOS 尚无 ESP、live root、NKM/NUC 与安装器替代路径。详见 `docs/BIOS 引导.md`。

## 现状核对与关键更正

当前 UEFI 链路为 `efi_main` 收集 CPU、内存与 GOP 信息，执行 stage3 的磁盘准备，调用 `ExitBootServices`，然后以 Microsoft x64 ABI 调用 `kernel_entry`。`entry.S` 安装内核栈、清 `.bss` 并调用 `kmain`；`kmain` 把三个参数原样交给 `pmm_init`。PMM 只把 `EfiConventionalMemory` 页加入可分配集合，其余类型默认保留。

必须以仓库中的实际定义为 ABI 基准：`arch/x86_64/include/uefi.h` 的 `EFI_MEMORY_DESCRIPTOR` 是 **40 字节，不是 48 字节**。字段偏移为 `Type=0`、`Pad=4`、`PhysicalStart=8`、`VirtualStart=16`、`Pages=24`、`Attribute=32`。x64 尺寸为 `4 + 4 + 4×8 = 40` 字节。`pmm_init` 要求 `desc_size >= sizeof(EFI_MEMORY_DESCRIPTOR)`，并按传入的 `desc_size` 跨项迭代；BIOS 应传 `desc_size=40`、`map_size=count*40`，不要自行采用 48 字节。

另有三项实现约束：

- 当前 MinGW 工具链生成 PE/COFF，不能用 GNU ELF 链接脚本直接链接出 BIOS ELF；BIOS 内核必须有独立 ELF 工具链/目标及独立对象目录。
- `lib/novuslib-rs` 目前 UEFI 目标为 MSVC/COFF。BIOS ELF 目标需用 `x86_64-unknown-none` 单独构建静态库，并启用 `bios-ms-abi` feature；host/UEFI 保留默认 C ABI，不能把现有 COFF `.lib` 交给 ELF 链接器。
- `entry.S` 的入口参数约定是 MS x64 ABI（`RCX/RDX/R8`）。BIOS 入口必须明确采用该 ABI；同时在实现阶段核验 `entry.S` 调用 `kmain` 时的 MS x64 32 字节 shadow space 要求，必要时只调整入口汇编，不改变 `kmain`/`pmm_init` 的参数含义。

## 总体启动链

```text
BIOS/SeaBIOS
  -> LBA 0: 512 字节 bootsect，DAP 读 stage2 到物理 0x8000
  -> LBA 1..N: stage2，实模式采集 E820/VBE 并读内核
  -> A20 + GDT + PAE 页表 + EFER.LME + CR0.PG
  -> 64 位 legacy_entry，初始化临时栈及 .bss
  -> legacy_main：构造 EFI_MEMORY_DESCRIPTOR、填 shared_mem、绑定 VBE
  -> kernel_entry(RCX=mmap, RDX=map_size, R8=desc_size)
  -> kmain -> pmm_init -> 现有内核初始化
```

E820 和 VBE 的 BIOS 中断调用都必须在实模式阶段完成。长模式下不能直接调用 `int 15h` 或 `int 10h`；stage2 将结果保存到低内存的 `bios_boot_info`，交给 64 位 C 入口消费。

## 1. E820 与 PMM 内存图

### 描述符构造

每项按 `uefi.h` 原样布局写入：`Type`、零填充的 `Pad`、物理起始地址、`VirtualStart=0`、以 4 KiB 为单位的页数、`Attribute=0`。BIOS 不需要伪造 UEFI 属性位；PMM 当前只读取 `Type`、`PhysicalStart` 和 `Pages`。

| E820 类型 | 合成 EFI 类型 | PMM 结果 |
| --- | --- | --- |
| 1：可用 RAM | `EfiConventionalMemory` (7) | 加入可分配页 |
| 2：保留 | `EfiReservedMemoryType` (0) | 保留 |
| 3：ACPI reclaimable | `EfiACPIReclaimMemory` (9) | 保留；当前 PMM 不回收 |
| 4：ACPI NVS | `EfiACPIMemoryNVS` (10) | 保留 |
| 5：坏内存 | `EfiUnusableMemory` (8) | 保留 |
| 未知/禁用/其他 | `EfiReservedMemoryType` (0) | 保留 |

E820 可用区必须先与 loader 实际占用区做区间切分，再生成互不重叠、按物理地址排序的描述符。重叠描述符会造成危险：PMM 先把所有页标记为已用，随后遇到 Conventional 项便清空对应位，因此仅额外叠加一个 Reserved 描述符并不能覆盖重叠 Conventional 区间。遇到任何保留范围，应从可用区拆出并赋非 Conventional 类型。区间边界按页保守对齐：只有完整位于可用范围内的页才可标 Conventional；部分页归保留。

应从 Conventional 范围切出的活跃区域至少包括：BIOS/实模式栈与 DAP、stage2（可选择始终保留，便于诊断）、长模式初始页表、`bios_boot_info`、EFI 描述符缓冲区、内核镜像的 text/data/bss 与启动栈、共享内存 `0x7E000..0x7FFFF`，以及 VBE framebuffer 所在区间（若 E820 将其报告成可用 RAM）。共享内存结构保持 spec 2.3 FROZEN，尺寸和地址不变。PMM 另行保留 `0x8000` AP trampoline 页；stage2 放置与其重叠时也必须保留该页。

### 建议低内存布局

以下为初始固定布局建议，所有地址均为物理地址；stage2 在使用前应检查 conventional-memory 上界及 EBDA 边界，不能覆盖 BIOS 数据区。若布局不满足检查则停止启动，不猜测可用内存：

| 地址范围 | 用途 |
| --- | --- |
| `0x00000..0x004FF` | IVT/BDA，保留 |
| `0x06000` 附近 | EDD DAP 与临时 BIOS 参数 |
| `0x07000` 以下 | 实模式临时栈，向下增长 |
| `0x08000..0x17FFF` | stage2，限制为最多 64 个扇区（32 KiB） |
| `0x20000..0x25FFF` | 6 页初始四级页表：PML4、PDPT、4 个 PD |
| `0x50000..0x5FFFF` | `bios_boot_info`、原始 E820 项及 EFI 描述符缓冲区 |
| `0x7E000..0x7FFFF` | FROZEN shared memory，PMM 保留 |
| `0x100000` 起 | raw 内核镜像及其运行时 `.bss` |

建议 `bios_boot_info`、E820 数组和最终 EFI 描述符数组分别放在上述 64 KiB 区域的独立页区间；限定最大项数并在溢出时失败。`mmap_addr` 指向最终 descriptor 数组，`map_size=count*40`。此缓冲区在 `pmm_init` 读完前不能被覆盖；若它最终标为 Conventional，只有在 `pmm_init` 返回后才可能被分配，届时已无后续读取。

不要为了模拟整个物理地址空间而人为增加高地址 MMIO 描述符：`pmm_init` 用所有描述符的最高结束地址算 `ram_top`，随后 `mm_init` 按 `ram_top` 建 physmap；虚构很高的 reserved 范围可能导致无意义的大量页表分配。仅保留真实 E820 范围与明确的低内存/启动占用区；E820 未描述的页在 PMM 中天然保持 reserved。

### 最低内存自检与 shared memory

沿用 UEFI 的 2 GiB 门槛：对 E820 type 1 的有效 RAM 做溢出安全的长度求和，并在切分启动器保留区域后确认最终 Conventional 总量不少于 `0x80000000`。低于门槛则写 `SHM_STATUS_FAIL` 并停机；成功时以 `shared_mem_bootstage_init` 初始化固定 8 KiB 块，再通过 `shared_mem_bootstage_set_cpu` 填 CPU vendor、brand、逻辑处理器数、可用字节和 2 MiB 对齐的 `ram_top_bytes`，最后写 `SHM_STATUS_PASS`。CPUID 收集可复用 `main.c` 的 leaf 0、`0x80000002..4`、leaf `0x0B`/leaf 1 回退逻辑。

## 2. VBE 显示

stage2 在实模式调用 VBE controller/mode-info 服务，枚举支持的模式，优先选满足 **1280×800、32 bpp、线性 framebuffer** 的模式；用 `AX=4F02` 设置模式，并在 `BX` 设置 LFB bit 14。`0x118` 不是可移植的 1280×800×32 保证值（常见实现中它甚至代表其他分辨率/色深），必须以 mode list 和返回的 mode-info 为准。找不到目标模式时可选最接近且 ND 支持的 32 bpp 模式，否则进入 VGA 文本回退。

`bios_boot_info` 保存 `PhysBasePtr`、宽高、pitch、bpp、颜色通道 mask、VBE 版本及可用 framebuffer 字节数。VBE 2.0/3.0 pitch 字段位置不同，应按版本读取。现有 `nd_display_t.pitch` 的单位是每行像素数，而 VBE pitch 通常以字节给出；仅当 pitch 可被 4 整除且 `pitch_bytes/4 >= width` 时转换为像素 stride。只接受现有 ND 能直接绘制的 RGB/BGR 8-bit 通道布局，其他 mask 不应错误地按 BGR 猜测。

在 `graphics/nd_hal.c` 增加 `nd_capture_bios(...)`，在 `.data` 中填充 `g_disp`，保留与 `nd_capture_boot` 一致的限制：有效尺寸、32 bpp、可访问 framebuffer 范围以及 RGB/BGR 标记。接口建议显式接收 `fb_size` 和颜色格式（或通道 mask），pitch 单位明确，而不只接收 `base/width/height/pitch/bpp`。目前内核恒等映射低 4 GiB；第一版必须拒绝 framebuffer 末端超过 4 GiB 的模式。无可用 LFB 时令 backend 为 `none`，现有 `vga.c` 文本输出继续工作。

BIOS C 入口在 `nd_init` 之前调用 `nd_capture_bios`。`g_disp` 已被特意放入 `.data`，可跨 `kernel_entry` 的 `.bss` 清零保留。`CONFIG_NOVUS_DISPLAY=n` 时不调用捕获逻辑；不论显示是否可用，COM1/VGA 都应提供故障输出。

## 3. stage1、stage2 与长模式入口

### stage1 / bootsect

`arch/x86_64/boot/bios/bootsect.S` 是 512 字节 BIOS 启动扇区：首字节为短跳转和 NOP，初始化段寄存器及栈，保存 BIOS 提供的启动盘号 `DL`，用 `INT 13h AH=42` 和位于低内存的 DAP 从 LBA 1 读取 stage2 到 `0000:8000`。读取失败用 `INT 10h` teletype 显示 `NO STAGE2` 后关中断停机；成功远跳转到 `0000:8000`。

采用 MBR 兼容的代码区上限 446 字节，偏移 446..509 保留 64 字节分区表，偏移 510..511 写 `0x55AA`。Superfloppy 镜像将分区表清零；将来增加 MBR 分区布局时再填表，不改变前 446 字节代码约束。stage2 扇区数由共享布局头中的 `STAGE2_SECTORS` 管理，打包器必须校验实际尺寸并按该值填充。

### stage2

`stage2.S` 使用纯汇编 `.code16`，不依赖 C 的 `-m16`。进入后立即保存 `DL`，规范化 `DS/ES/SS`，确认 EDD 支持，采集 E820（验证 `SMAP` 签名、返回长度和 continuation token，并限定最大项数），再查询 VBE 并记录结果。所有 BIOS 中断都在此阶段完成。

读取 1 MiB 处的内核时要注意：经典 16-byte DAP 的 segment:offset 缓冲区地址不能通用地寻址 1 MiB 以上，且有 64 KiB 边界限制。不可假设每台 BIOS 都支持 24-byte DAP 的 64-bit flat buffer pointer。建议第一版分块读到低内存 bounce buffer，再用受控的 unreal/protected-mode copy 搬到 `KERNEL_LOAD=0x100000`；如果采用 64-bit DAP 直接读，必须先探测/验证支持并保留 bounce fallback。扇区数向上取整，最后一扇区的填充不属于 ELF 文件内容。

完成数据采集和镜像装载后，按序启用 A20（先端口 `0x92`，必要时键盘控制器备用路径并验证）、加载 stage2 自带 GDT、设置 `CR4.PAE`、装入低内存页表的物理地址到 `CR3`、设置 `EFER.LME`、设置 `CR0.PE|PG`，最后远跳到 64 位代码段。页表以 2 MiB 页建立 0..4 GiB identity map，供内核镜像、栈、共享内存、VGA、低位 VBE LFB 和 BIOS 数据访问。开启长模式前确认 CPUID 长模式位；不支持时串口/屏幕报错并停机。

长模式目标应是链接符号 `legacy_entry`，而非直接跳到普通 C 函数。stage2 当前跳到固定 `KERNEL_LOAD`，因此链接脚本必须断言 `legacy_entry == KERNEL_LOAD`，并把 `.text.boot` 放在镜像首字节。入口设置临时栈、清零 `.bss` 后再按 MS x64 ABI 调用 `legacy_main`。这样 `legacy_main` 可安全初始化 BSS 中的数据；之后它调用现有 `kernel_entry(mmap_addr, map_size, desc_size)`，由 `kernel_entry` 按既有路径再次清 BSS、换正式 BSP 栈并进入 `kmain`。两次清零是有意的；`g_disp` 在 `.data`，不会被清除。

串口早期打点可在 BIOS C 入口的 `serial_init` 之前直接初始化 COM1 或使用有界的 UART 初始化/输出 helper，输出 `[BIOS] legacy boot`、`[BIOS] long mode`、`[BIOS] E820`、`[BIOS] entering kernel`。进入 kmain 后使用现有串口驱动。所有轮询需有超时，避免无 UART 的机器卡死。

## 4. raw 内核镜像与链接

`NOVUS.EFI` 是 PE32+ EFI 应用，SeaBIOS 不会像 UEFI 固件那样加载它。BIOS 目标必须单独链接一个 ELF 中间文件，再通过 `objcopy -O binary` 产生平坦 raw 镜像。链接 VMA/LMA 从 `KERNEL_BASE=0x100000` 开始；stage2 将镜像搬到同一物理地址。链接脚本顺序为 `.text`、`.rodata`、`.data`、`.bss (NOLOAD)`，定义 `__bss_start__`、`__bss_end__`、`legacy_entry` 和 `kernel_entry`。`.bss` 不写进 raw 文件，但镜像元数据必须给出其运行时尾地址，便于验证最大占用和保留范围。

raw 目标对象集合使用现有 `SRC` 的内核/图形/WM/文件系统/安全模块，加 `arch/x86_64/boot/bios/legacy.c` 与 BIOS 入口汇编；排除 `arch/x86_64/boot/main.c`、`stage3.c` 和 UEFI 专属 stage3 代码。`entry.S` 可复用，但要做 ELF 链接和 ABI 验证。`-mcmodel=small` 与 1 MiB 低半区链接地址匹配；还需使用 freestanding、no red-zone、no stack protector、no PIC 等现有内核约束。链接脚本必须断言镜像和 `.bss` 均不越过 32-bit 可寻址范围及 stage2 约定的最大加载边界。

建议使用 host ELF GCC 加 `-mabi=ms`，输出到单独的 `build/bios/`，避免 PE/COFF 对象污染或误复用。raw C 内核继续使用 MS x64 ABI，从而保留 `kernel_entry` 的 RCX/RDX/R8 契约；Rust crate 用 ELF target 并以 `bios-ms-abi` feature 显式标注 Win64 FFI。host/UEFI Cargo 构建保持默认 C ABI。不能只切换 ELF 输出格式而不检查 Rust/C 调用约定、panic hook、入口 shadow space 和栈对齐。

## 5. 磁盘镜像布局

第一版用无文件系统、无分区表的 superfloppy raw 磁盘镜像，SeaBIOS 直接从 LBA 0 执行签名有效的启动扇区。stage2 使用 EDD LBA 读取后续固定位置的镜像，不解析 FAT/GPT。

| LBA | 内容 | 说明 |
| --- | --- | --- |
| 0 | bootsect，512 字节 | 446 字节以内代码 + 64 字节空分区表 + `0x55AA` |
| 1..N | stage2 | `STAGE2_SECTORS=N`，填充到整扇区 |
| N+1..N+K | `novus-kernel.bin` | `KERNEL_LBA=N+1`，`K=ceil(kernel_bytes/512)` |
| N+K+1.. | 可选保留区 | 第一版不使用 |

所有共同常量集中在 `arch/x86_64/boot/bios/boot_layout.h` 并由 bootsect、stage2、C 入口和 host 打包器共用。stage2 区固定为 64 扇区，即 `N=64`、内核从 LBA 65 开始；stage2 尾部的 `NVB2` 头记录实际 kernel sector count 和 byte size。若 stage2 超过 64 扇区则汇编 `.org` 失败，不静默覆盖内核。打包器校验 bootsect 恰为 512 字节、签名为 `55 AA`、stage2 扇区数一致、镜像长度足够、LBA 算术不溢出，并按 512 字节填充。ISO/El Torito 作为后续可选工作，不进入首个可启动目标。

## 6. 构建目标与双引导隔离

增加 `CONFIG_BIOS_BOOT`（Kconfig 和当前被 Makefile 直接读取的 defconfig 均声明），以及 `make bios-img`、`make run-bios`。`bios-img` 独立构建 bootsect、stage2、raw ELF、bin 和 `out/bios.img`；`scripts/run-bios.sh` 使用 SeaBIOS 默认固件，例如：

```sh
qemu-system-x86_64 -machine pc -smp 2 -m 4G \
  -drive file=out/bios.img,format=raw \
  -serial stdio -no-reboot
```

UEFI 的 `make`、`make run`、OVMF、`NOVUS.EFI`、现有 PE 对象与 stage3 源清单保持不变。BIOS 目标不能成为默认 `all` 的依赖，不能改写 `out/NOVUS.EFI`，也不能复用 PE 对象目录。可以提供独立配置开关，但基础 `make run` 默认行为必须完全不变。

## 7. UEFI 专属功能边界

内核的 `kmain → pmm_init → mm_init → heap_init` 可复用，只要 BIOS 提供正确的 descriptor ABI、shared memory 和显示状态。NovusFS、PMM/MM/Heap、ND 离屏 selftest、IPC 等内核自测不依赖 UEFI 服务。

但 UEFI `stage3.c` 承担 ESP/FAT 读取、live root 加载、Merkle 启动期验证、NKM/NUC 应用暂存、安装菜单及安装器等职责。BIOS MVP 不含这些能力时，应将 shared memory 对应字段保持为定义的空/未知状态（如 Merkle unknown、image base/size 为 0），不伪造 PASS；`kmain` 已有无 live root / 无 staged app 的降级分支。`novusfs_selftest` 仍可验证内存盘。若后续要求 BIOS 启动磁盘上的实际系统分区、签名模块、应用或安装器，需另立 BIOS 磁盘读取/分区与验证方案，不属于“只加入口”的工作。

## 8. 分阶段实施与验收

1. **最小串口链路**：实现 bootsect、stage2、A20、页表和 64 位转移；先由 `legacy_entry` 打印阶段标记，再调用内核。QEMU `-machine pc` 下确认 LBA 读取和长模式入口稳定。
2. **EFI map 兼容**：实现 E820 转换、2 GiB 检查、shared memory 和 `kernel_entry` 参数传递。串口确认 PMM、MM、Heap selftest PASS；特别检查 Conventional/Reserved 不重叠及 boot buffer 不被过早分配。
3. **VBE 图形**：选择并设置真实报告为 1280×800×32 的 LFB 模式，加入 RGB/BGR、pitch、地址上界校验；否则确认 VGA fallback，不因图形缺失影响启动。
4. **镜像与回归**：`make bios-img`、运行 BIOS QEMU；随后独立执行现有 `make run` 的 OVMF 回归，确认 UEFI 路径未变。
5. **可选 ISO**：单独评估 El Torito BIOS 启动映像及光盘介质上的 INT 13h 行为；不阻塞 raw 磁盘 MVP。

## 文件清单

### 本设计阶段

- 新增：`docs/BIOS 引导方案.md`、`docs/BIOS 引导.md`、`arch/x86_64/boot/bios/boot_layout.h`、`bootsect.S`、`stage2.S`、`legacy.c`、`legacy.h`、`legacy_entry.S`、`bios.lds`、`tools/mkboot/mkboot.c`、`scripts/run-bios.sh`。
- 修改：`Makefile`、`Kconfig`、`configs/novus_defconfig`、`graphics/nd.h`、`graphics/nd_hal.c`、`arch/x86_64/kernel/entry.S`、`lib/novuslib-rs` ABI 标注、`README.md`。
- 验收状态：编辑器诊断通过；汇编、ELF 链接、镜像打包、QEMU BIOS 启动及 UEFI 回归均尚未运行。

### 后续实现预计新增

- `arch/x86_64/boot/bios/boot_layout.h`
- `arch/x86_64/boot/bios/bootsect.S`
- `arch/x86_64/boot/bios/stage2.S`
- `arch/x86_64/boot/bios/legacy.c`，必要时 `legacy.h`
- `arch/x86_64/boot/bios/legacy_entry.S`
- `arch/x86_64/boot/bios/bios.lds`
- `tools/mkboot/mkboot.c`（或等价 host 工具）
- `scripts/run-bios.sh`

### 后续实现预计修改

- `Makefile`、`tools/tools.mk`：隔离的 ELF/raw 构建与 `bios-img`、`run-bios` 目标。
- `Kconfig`、`configs/novus_defconfig`：声明 BIOS 构建选项；当前 Makefile 直接读 defconfig，二者需同步。
- `graphics/nd.h`、`graphics/nd_hal.c`：增加 BIOS framebuffer 捕获入口。
- `lib/novuslib-rs` 的 Cargo feature/FFI 声明：BIOS 目标用 ELF 静态库和 `bios-ms-abi`，host/UEFI 继续默认 ABI。
- `README.md`、新增 `docs/BIOS 引导.md`、`changes.md`：完成实现后记录用法、差异和验证结果。
- `arch/x86_64/kernel/entry.S` 或最小 ABI shim：仅当 ELF 链接或 MS x64 调用约定核验要求调整时修改；UEFI 路径需回归。