---
name: novus-bsod
overview: 按 Windows BugCheck 原理实现 Novus OS 蓝屏：内核致命异常与手动快捷键触发全屏独占蓝屏，显示错误码、描述、寄存器与 klog 详细日志，Ctrl+Alt+Esc 重启，DEBUG.md 维护错误码对应表。采用方案 A：不改动 SMP 架构，在实际运行内核的 BSP 上实现。
design:
  styleKeywords:
    - Windows 经典蓝屏
    - 深蓝底白字
    - 等宽终端感
    - 高信息密度
    - 全屏独占
  fontSystem:
    fontFamily: 内置 5×7 等宽点阵
    heading:
      size: 32px
      weight: 700
    subheading:
      size: 18px
      weight: 600
    body:
      size: 14px
      weight: 400
  colorSystem:
    primary:
      - "#0000AA"
      - "#000080"
    background:
      - "#0000AA"
      - "#000080"
    text:
      - "#FFFFFF"
      - "#C0C0C0"
    functional:
      - "#FF6B6B"
      - "#4FC3F7"
      - "#808080"
todos:
  - id: bsod-core
    content: 新增 kernel/bsod.h 与 kernel/bsod.c，实现停止码映射、快照、全屏绘制与日志排版
    status: completed
  - id: hook-fatal
    content: 在 trap.c 与 pmm.c 接入致命路径，并把 kernel/bsod.c 加入 Makefile
    status: completed
    dependencies:
      - bsod-core
  - id: hook-hotkey
    content: 在 nd_input.c、hid_boot.c 与 nwm.c 注入全局热键钩子和超时重置
    status: completed
    dependencies:
      - bsod-core
  - id: debug-doc
    content: 编写根目录 DEBUG.md 停止码对应表、字段说明与触发方式
    status: completed
    dependencies:
      - bsod-core
  - id: verify
    content: 用 [subagent:code-explorer] 复核调用链，编译并在 QEMU 验证蓝屏与重启
    status: completed
    dependencies:
      - hook-fatal
      - hook-hotkey
      - debug-doc
---

## 用户要求

在已实现的蓝屏界面中，将顶部标识条与主体标题区改为整体水平居中，仅调整视觉布局，不改变蓝屏触发、停止码映射、日志显示与重启逻辑。

## 产品概述

蓝屏触发后仍保持全屏蓝底白字的经典风格。顶部蓝色横条中的 `:(` 图标、`Novus OS` 文本和 `STOP` 标签作为一组整体居中；主体区域的停止码与错误名作为一组整体居中，使界面更接近 Windows 11 蓝屏的对称视觉。

## 核心功能

- 顶部标识条三元素整体水平居中。
- 移除右上角独立停止码，避免与主体标题重复。
- 主体停止码与错误名宽屏时同排整体居中，窄屏时上下堆叠并各自居中。
- 描述文字、寄存器区、日志区、底部重启提示保持现有布局不变。

## Tech Stack Selection

沿用现有 C 内核与 NovusDisplay 绘制接口，不新增文件、类型、syscall 或构建配置。使用 `nd_text_width()` 计算文本像素宽度，使用现有 `nd_text()`、`nd_frame()`、`nd_fill_rect()` 完成绘制。

## Implementation Approach

在 `kernel/bsod.c` 的 `bsod_draw()` 中只修改顶部蓝条和主体标题绘制：

- 顶部蓝条：把 52 像素方框图标、`Novus OS` 文本、`STOP / KERNEL FAULT` 标签视为一个整体，先计算组合宽度，再从屏幕水平中点左侧起依次绘制。删除原先右上角独立绘制停止码的代码。
- 主体标题：格式化停止码后，用 `nd_text_width()` 分别取得停止码与错误名宽度。宽屏且两者可同排放下时，按固定间距计算组合起点并整体绘制；窄屏时停止码与错误名上下堆叠，每行各自居中。
- 保留故障提示行和两行描述文字的左对齐布局；寄存器、技术信息、日志尾部、底部提示条完全不变。

该改动只增加少量字符串宽度计算，复杂度为常量级，无堆分配、无内核状态变化、无新增日志。

## Implementation Notes

- 只修改 `kernel/bsod.c`，不触碰 `kernel/bsod.h`、`DEBUG.md`、热键状态机和停止码映射。
- 顶部蓝条删除右上停止码后，主体标题是唯一停止码展示位置，需避免重复显示。
- 居中计算需防止低分辨率下出现负坐标；宽屏分支仅在计算宽度加间距不超过可用宽度时使用，否则走窄屏堆叠。
- 不改变 `nd_present()`、`nd_cursor_hide()` 和蓝屏冻结行为。

## Directory Structure

```
project-root/
└── kernel/
    └── bsod.c  # [MODIFY] 调整 bsod_draw() 的顶部标识条和主体标题布局：整体宽度计算、水平居中、宽屏同排与窄屏堆叠；保持其余区域不动。
```

## Agent Extensions

### SubAgent

- **code-explorer**
  - Purpose: 复核 `kernel/bsod.c` 中蓝屏绘制函数的布局改动点，确认没有残留的右上停止码绘制或影响其他区域的改动。
  - Expected outcome: 输出顶部蓝条、主体标题、描述区、寄存器区、日志区和底部提示条的修改边界核对结果，确认仅布局逻辑变化且无功能回归风险。
