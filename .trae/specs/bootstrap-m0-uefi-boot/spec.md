# M0 最小 UEFI 引导与 A核 BSP 基础启动 Spec

> 对应 README 第19部分里程碑 **M0：最小UEFI引导，A核BSP基础启动，VGA文本输出**。
> 后续里程碑 M1~M10 将各自建立独立 spec，不在本 spec 范围内。

## Why
Novus OS 全新起步，工作区仅有 README 技术规范与 LICENSE。按里程碑顺序首先交付 M0：可构建、可在 QEMU+OVMF 上运行验证的最小 UEFI 引导 + A核(BSP)基础自检 + 文本输出，为 M1（内存管理、分页、SMP 唤醒 B核）奠定引导骨架。

## What Changes
- 新建自研 UEFI 头文件 `arch/x86_64/include/uefi.h`：最小 EFI 系统表 / 启动服务 / 简单文本输出协议 / 内存描述符等定义。依据 UEFI 规范自行定义，**不引入 GNU-EFI / EDK2 等外部项目**（README 约束：全部自研）
- 新建 `arch/x86_64/boot/`：UEFI 引导入口 `efi_main`，通过 ConOut 打印 "Novus OS" 开机横幅与各阶段自检状态文字
- 新建 `kernel/`：A核 BSP 早期阶段（`kmain`，ExitBootServices 之后接管）、VGA 文本驱动（直接写 0xB8000 文本缓冲区）、COM1 串口调试输出
- 基础自检（A核 BSP 职责，README 2.2 步骤 1~3 的 M0 子集）：
  - CPUID 探测：厂商、处理器信息、逻辑核数（仅探测记录；唤醒 B核属于 M1）
  - GetMemoryMap 扫描与可用内存统计
  - 可用内存 < 2GiB 时拒绝启动（README 1.2 冻结规则），输出错误并停机
- 新建构建体系雏形（README 第16部分目录骨架的 M0 子集）：顶层 Makefile、最小 Kconfig / `configs/novus_defconfig`、`scripts/` 运行脚本
- 验证工具链安装（apt：build-essential、qemu-system-x86、ovmf，按需 gcc-mingw-w64）：构建与运行工具，不属于 OS 自身代码，不违反自研约束

## Impact
- Affected specs: 无（首个 spec）
- Affected code:
  - 新增 `arch/x86_64/boot/`、`arch/x86_64/include/`、`kernel/`、`scripts/`、`configs/`、`Makefile`、`Kconfig`
  - 不修改现有 `README.md`、`LICENSE`

## ADDED Requirements

### Requirement: 最小 UEFI 引导
系统 SHALL 以自研 UEFI 应用形式启动：代码全部自研，不链接、不包含 GNU-EFI / EDK2 / gnu-efi 头文件或库。

#### Scenario: QEMU+OVMF 成功启动
- **WHEN** 执行 `make run` 在 QEMU + OVMF 固件下启动
- **THEN** 屏幕显示 "Novus OS" 开机横幅与各阶段自检状态文字，最终系统进入稳定停机循环，无三重故障、无重启

### Requirement: A核 BSP 基础自检
引导阶段 SHALL 在 A核(BSP) 上完成最小自检并通过 ConOut 逐行输出状态：CPU 厂商/品牌信息、逻辑核数、内存图扫描与可用内存总量。

#### Scenario: 内存低于 2GiB 拒绝启动
- **WHEN** 内存图统计得到可用内存 < 2GiB（如 QEMU `-m 1G`）
- **THEN** 输出内存不足错误信息并停止启动，不进入后续 BSP 阶段

#### Scenario: 自检通过
- **WHEN** 自检无严重错误（如 QEMU `-m 4G`）
- **THEN** 自检结果全部输出后，取得最终内存图并调用 ExitBootServices，跳转 A核 BSP 早期阶段 `kmain`

### Requirement: VGA 文本输出
A核 BSP 早期阶段 SHALL 通过自研 VGA 文本驱动直接写 0xB8000 文本缓冲区输出启动信息（清屏、光标定位、字符+属性打印）；同时 SHALL 通过 COM1 (0x3F8) 串口镜像输出相同内容，作为自动化验证通道。

#### Scenario: 串口验证输出
- **WHEN** QEMU 以 `-serial stdio` 运行至 BSP 阶段
- **THEN** 宿主终端看到 BSP 阶段逐行输出（CPU 信息、内存总量、核数、"VGA output initialized" 等）

### Requirement: 构建体系雏形
顶层 SHALL 提供 `make`（构建 .efi 产物）、`make run`（QEMU+OVMF 启动验证）。目录布局遵循 README 16.2 骨架的 M0 子集。Kconfig 仅放最小占位配置项，完整 menuconfig 属于后续里程碑。

## 边界说明（冻结决策）
1. ExitBootServices 之后 OVMF 固件处于 GOP 图形模式，0xB8000 文本缓冲区在 OVMF 下不一定可见；**QEMU 自动验证以串口输出为准**，屏幕可见输出以 ExitBootServices 之前的 ConOut 阶段为准，VGA 直写路径面向真实硬件文本模式。帧缓冲像素级文本渲染（自研字体）属于 M5 NovusDisplay 范围
2. 本 spec 不实现：分页/内存管理器、唤醒 B核 AP、NovusFS、Merkle 校验、NKM、系统调用、图形栈（分别属于 M1~M5+）
3. .efi 构建方案择优：优先 mingw-w64 工具链直出 PE32+；若不可用则 GNU ld 产出裸二进制 + 自研 PE 封装，实现时确定并在代码中固化
