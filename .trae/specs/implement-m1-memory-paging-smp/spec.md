# M1 基础内存管理器、分页与 SMP 唤醒 B核 Spec

> 对应 README 第19部分里程碑 **M1：基础内存管理器、分页、SMP唤醒B核**。
> 基于 M0（bootstrap-m0-uefi-boot，已完成）成果继续。README 第2部分（SMP AB双核启动）、第3部分（内存管理）为本 spec 的冻结依据。

## Why
M0 已交付最小 UEFI 引导 + A核(BSP)自检 + VGA/串口输出，但系统仍是"裸机停机"状态：无内存管理、无自有页表、单核运行。M1 建立内存管理器与分页基础设施，并按 README 第2部分完成 AB 双核两阶段启动：A核自检完成后唤醒 B核(AP)，B核接管运行，A核进入休眠——为 M4 调度器与后续所有子系统奠定地基。

## What Changes
- **汇编主入口**：新增内核汇编入口 `arch/x86_64/kernel/entry.S`（GNU as 语法）——ExitBootServices 后第一条内核指令为汇编（建栈、清 bss、调 C `kmain`）；B核/AP 复位向导 `arch/x86_64/kernel/ap_trampoline.S`（16位实模式 → 32位保护模式 → 64位长模式，构建为裸二进制内嵌，运行时复制到低内存 0x8000，SIPI 向量 8）
- **物理内存管理器 (PMM)**：位图分配器，从 M0 移交的 UEFI 内存图初始化；`EfiConventionalMemory` 为可用，其余类型保留；显式排除内核镜像(EfiLoaderCode/Data)、共享内存页(0x7E000-0x7FFFF)、AP trampoline 页(0x8000)、各 AP 栈；API：`pmm_alloc/pmm_free/pmm_free_count`。位图原语用 **Rust 实现**（见下）
- **Rust 混合编译落地**（README 16.4：lib/ 为 C/Rust 混合通用库）：新 crate `lib/novuslib-rs/`（no_std、staticlib、**零外部 crate**、panic=abort），实现 PMM 位图原语（置位/清位/测试/寻找连续空闲段），C 侧 `kernel/pmm.c` 经 FFI 调用；panic handler 经串口输出后 abort。工具链：rustup stable + target `x86_64-unknown-none`；若安装不可行则降级纯 C 并在任务中记录、Rust 推迟到 M5 图形模块
- **分页与 VMM**：自建四级页表（PML4）替换固件页表并切换 CR3：恒等映射低 4GiB（2MiB 大页，覆盖内核镜像/VGA/LAPIC MMIO/trampoline/AP栈）+ 物理直映射区（physmap）`0xFFFF800000000000` 覆盖全部 RAM；VMM 基础 API：`vmm_alloc_page/vmm_free_page`；内核堆 `kmalloc/kfree`（页粒度，PMM+physmap 背书，README 3.1 内核堆区）
- **共享内存规范**（README 2.3）：固定预留物理地址 0x7E000 起连续 2 页，冻结结构体：魔数 "NVSH"、版本、自检状态码、CPU 信息、可用内存、AP 上线计数、**A核开关标志**（0=启用自检，为 M8 高级设置预留）、AP 命令字段（SLEEP）、带自旋上限的自旋锁、小环形日志缓冲
- **SMP 唤醒 B核**（README 2.2 步骤 4-7）：`kernel/lapic.c`（MSR 0x1B 检测/使能 LAPIC）、`kernel/smp.c` INIT→SIPI→SIPI 序列（Intel 时序：INIT 后 10ms、首个 SIPI 后 200us 二次 SIPI）；AP 经 trampoline 载入与 BSP 相同 CR3、分配独立栈（PMM 16KiB）、置共享内存上线标志并串口输出 "AP (B核) online"；BSP 有界等待确认（超时按 README 3.4 降级为告警不 panic），随后按 README 步骤 6 A核进入休眠（hlt 循环）
- **A核开关**：共享内存标志默认启用自检；defconfig 增加 `CONFIG_NOVUS_A_CORE_DISABLE=n`，置 y 时 A核跳过自检直接唤醒 B核（验证 README 2.2 步骤 7 路径；M8 设置界面将来改写共享内存标志）
- **构建体系扩展**：Makefile 增加 Rust crate 构建（cargo，产物静态库并入 PE 链接）；`scripts/run.sh` 增加 `-smp 2`（AB 双核）；源文件清单扩展 entry.S/ap_trampoline.S/pmm/vmm/lapic/smp/shared_mem

## Impact
- Affected specs: bootstrap-m0-uefi-boot（消费其移交接口：内存图指针/尺寸/描述符尺寸，签名不变）
- Affected code:
  - 新增 `arch/x86_64/kernel/entry.S`、`arch/x86_64/kernel/ap_trampoline.S`、`kernel/{pmm,mm,vmm,heap,lapic,smp,shared_mem}.c/.h`、`lib/novuslib-rs/`（Cargo.toml + src/lib.rs + FFI 头）
  - 修改 `arch/x86_64/boot/main.c`（移交点改为跳转 `kernel_entry` 汇编入口；A核开关读取）、`kernel/kmain.c`（初始化序列：PMM→分页→堆→SMP→休眠 A核）、`Makefile`、`scripts/run.sh`、`configs/novus_defconfig`
  - 不修改 `README.md`、`LICENSE`；M0 已验证行为（1G 拒绝启动路径）保持回归可用

## ADDED Requirements

### Requirement: 汇编内核主入口
系统 SHALL 以汇编（GNU as）作为内核主入口：Boot 阶段 ExitBootServices 后跳转汇编 `kernel_entry`，由汇编建立栈并调用 C `kmain`；AP 复位入口同样为汇编 trampoline。

#### Scenario: 长模式接管
- **WHEN** Boot 阶段完成 ExitBootServices 并跳转 `kernel_entry`
- **THEN** 汇编设置 64KiB 对齐内核栈、清除 bss 后调用 kmain，串口出现内核初始化首行输出，无三重故障

### Requirement: 物理内存管理器
系统 SHALL 提供位图式 PMM：以 M0 移交的 UEFI 内存图为初始化输入，可用页（EfiConventionalMemory）建位图，保留页不可分配；分配/释放均经带自旋上限的自旋锁保护。

#### Scenario: PMM 初始化与统计
- **WHEN** kmain 调用 pmm_init(内存图)
- **THEN** 串口输出总页数/可用页数/保留页数统计行；连续分配-释放 N 页后 free_count 恢复原值（自检内建测试）

#### Scenario: 保留区不可分配
- **WHEN** PMM 初始化完成
- **THEN** 内核镜像、共享内存页(0x7E000-0x7FFFF)、trampoline 页(0x8000)在位图中标记为已用

### Requirement: Rust 混合编译
构建系统 SHALL 将 `lib/novuslib-rs` no_std staticlib（零外部依赖）并入内核 PE 链接；PMM 位图原语由 Rust 实现并经 FFI 供 C 调用；Rust panic 经串口报告后 abort。

#### Scenario: FFI 调用验证
- **WHEN** kmain 执行 PMM 自检分配测试
- **THEN** 测试路径实际调用 Rust 位图函数（串口打印 Rust 模块版本行），构建产物链接成功无重复符号冲突

### Requirement: 分页与虚拟内存
系统 SHALL 建立自研四级页表并切换 CR3：恒等映射低 4GiB + physmap（0xFFFF800000000000 起映射全部 RAM，2MiB 页）；提供 vmm_alloc_page/vmm_free_page 与页粒度 kmalloc/kfree。

#### Scenario: 页表切换生效
- **WHEN** kmain 完成页表构建并写 CR3
- **THEN** 串口输出 "paging enabled, CR3=0x..."，切换后 physmap 读回测试（经 physmap 读内核镜像首字节与恒等地址一致）通过，系统继续运行无异常

#### Scenario: 内核堆分配
- **WHEN** kmain 执行 kmalloc/kfree 模式化自检（分配→写入模式→读回校验→释放）
- **THEN** 校验通过且释放后 PMM free_count 恢复

### Requirement: 共享内存规范
系统 SHALL 在固定物理地址 0x7E000 预留 2 页共享内存，结构体布局冻结（魔数 NVSH、版本、自检状态、CPU 信息、内存信息、AP 上线计数、A核开关标志、AP 命令字段、日志缓冲、自旋锁），供 A核/B核通信；访问经带自旋上限的锁保护（README 3.4：超时降级告警，不死锁不 panic）。

#### Scenario: 双核一致视图
- **WHEN** AP 上线读取共享内存
- **THEN** AP 读到的 CPU 信息与内存信息与 A核自检写入值一致（串口镜像输出比对）

### Requirement: SMP 唤醒 B核
系统 SHALL 实现 INIT-SIPI-SIPI 唤醒序列：LAPIC 经 MSR 0x1B 检测使能；B核经 trampoline 进入长模式，加载与 A核相同的 CR3，使用独立 PMM 分配栈，置共享内存 AP 上线标志；B核完成初始化后 A核进入休眠（README 2.2 步骤 4-6）；A核开关标志被置位时跳过 A核自检阶段（步骤 7）。

#### Scenario: 双核上线
- **WHEN** `make run MEM=4G`（-smp 2）
- **THEN** 串口依次出现："[SMP] Waking AP (B核)..." → AP 行 "Novus AP (B核) online" → 共享内存 AP 计数=2 → "[SMP] A核 entering sleep (B核 in charge)" → "[HALT]"；QEMU 保持稳定无重启

#### Scenario: A核开关关闭
- **WHEN** defconfig 置 `CONFIG_NOVUS_A_CORE_DISABLE=y` 后 `make run`
- **THEN** A核自检输出行被跳过，B核仍被唤醒并正常上线（回归场景）

#### Scenario: M0 回归不破坏
- **WHEN** `make run MEM=1G`
- **THEN** 仍触发低于 2GiB 拒绝启动路径

## 边界说明（冻结决策）
1. 中断/IDT、调度器、进程属于 M4；M1 全程关中断运行，页表切换后不设 page fault handler（映射完备，不预期缺页）
2. 内核停留在恒等映射地址，不做 higher-half 重定位（M4+ 决策）；不做用户态页表隔离
3. Merkle/WAL 内存分区按 README 3.1 仅在分区规划文档与 PMM 预留注释中体现，数据结构属 M2/M3
4. OOM 管理器、弹窗提示属后续里程碑（依赖 GUI）；M1 仅提供 PMM 余量统计
5. B核"接管全部任务"的完整语义（任务/中断迁移）依赖 M4 调度器；M1 的 B核运行 ap_main 最小循环后待命
6. AP 数量固定唤醒 1 个（B核）；通用多 AP 枚举唤醒留待 M4
7. Rust 侧仅限 lib/ 通用库边界内的位图原语；若 x86_64-unknown-none 工具链安装失败，降级纯 C 实现、Rust 推迟至 M5，并在 tasks.md 记录
