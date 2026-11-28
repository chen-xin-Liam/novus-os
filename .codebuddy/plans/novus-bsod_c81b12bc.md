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

## User Requirements
### Product Overview
删除启动过程中标题为「Novus OS · 启动诊断」的全屏 45 秒报告页，使桌面构建启动后直接进入桌面，不再因日志未落盘而停留等待拍照取证。

### Core Features
- 移除全屏启动诊断页的绘制、保持与重新绘制逻辑。
- 移除仅为该页面服务的报告文本装配、串口诊断打印和相关静态缓冲。
- 保留启动后期的 USB 输入轮询、日志介质重试与刷盘，避免影响 USB 键鼠枚举和日志写入。
- 保留正常串口自检日志、蓝屏能力和窗口管理器的通用全屏重绘接口。

## Tech Stack Selection
沿用现有 Novus OS 自由站立 C 内核与 NovusDisplay 图形栈，不新增依赖、不新增 syscall、不改 ABI、不改构建脚本。

## Implementation Approach
采用最小删除策略：移除 `nd_demo_report_draw()` / `nd_demo_report()` 及其声明，删除 `kernel/kmain.c` 中仅用于诊断报告的 `rep_line()` / `diag_strip()` / `diag_report()` 和 45 秒循环。把该循环替换为一次有界的 `usb_hid_poll()`，随后执行 `usblog_retry()` 与 `usblog_flush(1)`，再继续启动桌面。USB 端口遍历的续跑仍由调度启动后的 `nd_input_poll()` 调用 `usb_hid_poll()` 承担，因此不需要保留页面等待。

关键决策：不删除 `nwm_repaint_all()`，因为它是绕过 WM 写屏后的通用恢复接口；不删除 `nd_input_report()`，因为它是串口取证信息，不属于屏幕页面；不批量清理历史文档中的截图记录，因为这些是历史验证证据。

## Implementation Notes (Execution Details)
- `graphics/nd_demo.c` 删除 `nd_demo_report_draw()` 和 `nd_demo_report()`；`graphics/nd.h` 删除两者声明与注释。
- `kernel/kmain.c` 删除 `char ib[256]`、报告装配 helper、`[BOOT] diag:` / `[BOOT] report drawn` / `[BOOT] report page skipped` 打印，以及 `char rest/rep/pd/tmp` 缓冲。
- 删除 45 秒 `nd_wait_ms(250)` 循环；用一次 `usb_hid_poll()` 保持端口推进，再做最终 `usblog_retry()` 和 `usblog_flush(1)`。
- `nd_input_banner()` 及 `ban_lit()` / `ban_num()` 在报告装配删除后无调用方，可一并从 `graphics/nd_input.c` 与 `graphics/nd_input.h` 删除；`nd_input_report()` 保留。
- 调整 `kernel/kmain.c` 中 PS/2 报告注释，避免继续声称结果会写入桌面状态条。
- 不使用动态内存，不增加启动路径上的额外遍历；删除后启动关键路径减少一次 45 秒阻塞和约 1600 字节报告文本装配。
- 保留 `docs/启动顺序与启动速度规范.md` 中的通用原则，只把 B-5 标记为已移除，并说明 USB 轮询改由输入 tick 承担。

## Architecture Design
本次为单点删除，不改变系统分层和数据流。启动流程从「首帧 → 诊断报告装配 → 条件性 45 秒页面 → 桌面」简化为「首帧 → 一次 USB 轮询与日志落盘重试 → 桌面」。

## Directory Structure
```text
project-root/
├── graphics/
│   ├── nd_demo.c        # [MODIFY] 删除全屏启动诊断页绘制与保持函数。
│   ├── nd.h             # [MODIFY] 删除 nd_demo_report / nd_demo_report_draw 声明和注释。
│   ├── nd_input.c       # [MODIFY] 删除仅服务诊断横幅的 nd_input_banner 与 ban_lit/ban_num。
│   └── nd_input.h       # [MODIFY] 删除 nd_input_banner 声明。
├── kernel/
│   └── kmain.c          # [MODIFY] 删除报告 helper、缓冲与 45 秒循环；保留一次 USB 轮询和日志重试/刷盘；调整注释。
├── docs/
│   └── 启动顺序与启动速度规范.md # [MODIFY] 更新 B-5，说明全屏诊断页已移除及替代轮询路径。
└── wm/novus-wm/         # [NO CHANGE] nwm_repaint_all() 与相关注释保留为通用写屏恢复接口。
```

## Agent Extensions
### SubAgent
- **code-explorer**
  - Purpose: 在修改完成后复核所有被删符号的残留引用、启动路径副作用和文档一致性。
  - Expected outcome: 确认无悬空声明、无未定义符号、USB 轮询与日志重试仍存在、串口日志不再出现诊断页标记。
