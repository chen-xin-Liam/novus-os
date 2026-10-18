---
name: 完善「设置」系统应用（Win11 多页设置 + 真实系统控制）
overview: 把当前只有一个强调色色块和几行数字的 356×250 小面板，做成 Windows 11 设置风格的多页应用：左侧导航（系统 / 个性化 / 应用 / 关于），右侧卡片式内容；并补上内核侧真正可用的控制能力——桌面背景切换、强调色扩展、运行中应用的结束与启动、内存总量与 IPC 明细。
design:
  styleKeywords:
    - Windows 11 Fluent
    - 浅色多页设置
    - 左侧导航 + 卡片
    - 克制蓝强调
  fontSystem:
    fontFamily: Noto Sans CJK SC
    heading:
      size: 20px
      weight: 600
    subheading:
      size: 14px
      weight: 500
    body:
      size: 12px
      weight: 400
  colorSystem:
    primary:
      - "#0067C0"
      - "#0078D4"
      - "#E5F1FB"
    background:
      - "#FFFFFF"
      - "#F7F7F9"
      - "#F3F3F3"
      - "#EEF2F8"
    text:
      - "#1B1B1B"
      - "#5F6368"
      - "#8A9099"
    functional:
      - "#C42B1C"
      - "#107C10"
      - "#B45309"
      - "#E1E5EA"
todos:
  - id: kernel-theme-stats
    content: 内核加壁纸主题接口与内存总量字段，并提升 WM 图元/文本上限
    status: completed
  - id: sched-kill
    content: 新增 sched_kill 与已退出任务回收扫描，sysinfo 增 THEME/KILL 请求
    status: completed
  - id: sinfo-client
    content: 扩展 Rust sinfo（theme/kill/total_kb）并修正三处窗口标题栏高度
    status: completed
  - id: settings-framework
    content: 重写 settings 为多页框架（侧栏+内容区+页脚），先落「系统」页
    status: completed
    dependencies:
      - sinfo-client
  - id: settings-pages
    content: 实现「个性化」「应用」「关于」三页与结束任务二次确认
    status: completed
    dependencies:
      - settings-framework
  - id: verify-settings
    content: "[subagent:code-explorer] 复核任务回收路径，再截图验收四页、结束/重启应用、壁纸切换并跑 wm-churn 回归"
    status: completed
    dependencies:
      - settings-pages
  - id: docs-iso
    content: 更新设置相关文档，重建 ISO 并从光盘复测设置页
    status: completed
    dependencies:
      - verify-settings
---

## 需求概述

把内置「设置」从“一个色块 + 几行只读数字”的最小面板，完善为 Windows 11 设置风格的多页应用，并让每一项设置真正作用于系统（而不是只改画面）。

## 现状问题

- 单页 356×250，只有 4 个强调色块与 6 行只读数字，功能等同于“关于本机”。
- 无导航、无分组、无说明文字，无法承载更多设置项。
- 可控制的东西只有一个强调色；背景、应用管理、系统信息都缺失。
- 三个 Rust 应用的窗口高度仍按 `CH + 25` 传，而 WM 标题栏已是 32px，客户区被裁掉 7px（底部按钮/文字贴边）。

## 目标功能

1. **系统页**：设备信息（名称/架构/分辨率/核心）、运行时间、内存占用条（已用/总量）、任务数、窗口数、IPC 收发与丢弃计数。
2. **个性化页**：8 个强调色（含当前色名）、3 套桌面背景渐变预览（云母蓝/晨雾灰/青瓷），点击即全桌面重绘；一个小型“窗口预览”示意当前配色。
3. **应用页**：列出正在运行的内置应用（文件管理器/任务管理器/设置），每行显示状态与 pid，右侧「结束」按钮可终止该进程（窗口随之关闭）；底部三个「启动」按钮在未运行时可用。
4. **关于页**：版本与里程碑、实现构成（内核 C + 用户态 Rust）、显示与输入说明、以及“设置不落盘、重启恢复默认”的明确提示。
5. **布局**：左侧 168px 导航（系统/个性化/应用/关于，选中项左侧 3px 强调色条 + 浅底），右侧 520px 卡片内容，底部页脚显示版本；窗口 720×470（客户区），标题栏 32px。

## 交互与视觉

- 悬停：导航项浅灰、按钮描边加深；选中：导航项浅蓝底 + 左侧色条。
- 结束任务需二次确认（同一按钮变红显示「确认结束」，3 秒后回落），避免误杀。
- 所有页面保持同一套浅色卡片、12px 正文、1px 边框，不引入圆角（客户端绘图只支持矩形）。

## 技术方案

沿用现有分层：Rust 客户端（`apps/`）通过 IPC 请求内核 `sysinfo` 服务，`sysinfo` 调用 WM/调度器/PM 的接口。设置项是**内核主题值**，一处修改即全桌面重绘。

### 1. 协议扩展（`kernel/sysinfo.h` + `apps/src/sinfo.rs`）

| 请求 | 载荷 | 应答 | 说明 |
| --- | --- | --- | --- |
| 0x0306 THEME | `{u8 id; u8 pad[3]; u32 value}` | 0x0315 `{u32 accent; u32 wallpaper}` | id 0=强调色(rgb)，1=壁纸(0..2)；查询时 value 忽略 |
| 0x0307 KILL | `{u64 pid}` | 0x0310 `{i32 status}` | 0=已终止，-1=无此任务，-2=拒绝（idle/spin） |


`sysinfo_stats_t` 追加 `UINT64 total_kb`（`pmm_total_pages()*4`）与 `UINT32 wallpaper`，保持 8 字节对齐；Rust `Stats` 同步字段顺序。

### 2. 内核侧

- `wm/novus-wm/nwm.c`：新增 `nwm_wallpaper_get/set(UINT32 idx)`，把 `theme.desktop_top/bottom` 设为三套预设之一；与 `nwm_accent_set()` 共用一次“全屏损伤 + 重绘”。预设颜色表放在 `nwm.c` 内（不散落到应用）。
- `kernel/sched.c`：新增 `sched_kill(UINT64 pid)`：
- `sched_find_pid()`（WM 的 liveness sweep 已在用）取任务；
- 当前任务 → 走 `sched_exit_current(0xDEAD)`；其他任务 → 置 `TASK_EXITED`、`ipc_owner_exit(pid)`、`sched_wake_port(pid)` 唤醒等待者；
- `sched_tick()` 增加“回收非当前已退出任务”的扫描（每 40 tick 一次，调用现有 `task_reap()`），否则被 kill 的阻塞任务会永久占槽；
- 拒绝 kill `idle`/`spin`（这两个是内核自检任务，杀掉会让调度器降级）。
- `kernel/sysinfo.c`：新增 `req_kill` / `req_theme`，其余 handler 不动；`req_stats` 补两个字段。

### 3. 客户端

- `apps/src/sinfo.rs`：`theme(port)`、`set_theme(port, id, value)`、`kill(port, pid)`；`Stats` 加 `total_kb`、`wallpaper`。
- `apps/src/ui.rs`：新增 `card(w, x, y, w, h, title)`（1px 边框 + `PANEL` 底 + 标题行）、`nav_item(...)`（含选中色条）、`meter(...)`（进度条）、`toggle_row(...)`；`button()` 增加 `hover`/`danger` 变体参数。
- `apps/src/wm.rs`：新增 `pub const TITLE_H: i32 = 32;`，三个应用窗口高度改为 `CH + wm::TITLE_H`（顺带修掉 7px 裁切）。
- `apps/src/bin/settings.rs`：重写为“页状态机 + 每页一个 draw 函数”，状态 `{page, accent, wallpaper, stats, tasks[8], confirming_pid, confirm_until}`；鼠标命中按页分派；每 60 次 yield 只在「系统/应用」页轮询，避免无谓 IPC。

### 4. 图元预算（关键约束）

WM 每窗上限 `NWM_MAX_SHAPES=28 / NWM_MAX_TEXTS=40`，多页布局会超。方案：

- 把上限提到 `40 / 48`（BSS 增量约 8 窗 × 3KB，可接受）；
- 设置页按“侧栏 + 2 张卡片 + 最多 5 行”设计，实测每页 ≤ 34 图元、≤ 40 文本；
- 验证时在串口检查是否出现 `NWM_EFULL`（当前不打印，可在 `req_shape`/`req_text` 溢出分支加一条限流日志，属于可观测性增强）。

### 5. 性能

- 每次交互只重绘本窗口（`clear + 一帧图元`），一次约 30 条 IPC，按现有 `send()` 的 `EBUSY` 重试即可；
- 轮询从 60 次 yield 改为“仅系统/应用页”，设置页空闲时零 IPC；
- 壁纸/强调色切换是一次全屏合成（约数毫秒），只在点击时发生。

### 6. 兼容与回滚

- 协议只增不改：旧的 STATS/ACCENT/SPAWN 语义不变；`Stats` 追加字段后 fileman/taskman 不读新字段，不受影响。
- 不动文件管理器/任务管理器除 `CH + TITLE_H` 之外的任何逻辑。
- `scripts/wm-churn.sh` 的坐标依赖窗口位置，设置窗口尺寸变化只影响它自己的关闭坐标（脚本只关 fileman/taskman/settings 三处，设置窗口改到 720×470 后需同步脚本中的关闭点）。

## 设计风格

Windows 11 设置（Settings）的浅色多页布局：左侧窄导航、右侧卡片式内容、克制蓝色强调、大量留白。整体沿用 M9 已落地的 Fluent 浅色令牌，避免出现第二套配色。

## 布局结构

- 窗口客户区 720×470，标题栏 32px（由 WM 绘制）。
- 左侧导航 168px：顶部 40px 放「设置」字样，下面 4 个导航项（系统/个性化/应用/关于），每项 40px 高，选中项浅蓝底 + 左侧 3px 强调色条，图标用 8×8 实心方块代替。
- 右侧内容 520px，四周留 20px：页标题 20px 加粗 + 说明 12px 灰色，下面是 1~2 张卡片（1px `#E1E5EA` 边框、`#F7F7F9` 底、圆角由 4 个 1px 直角替代）。
- 页脚：底部分隔线 + 左侧「Novus OS 设置」，右侧版本号，12px 灰色。

## 各页内容

**系统**：卡片「设备」（名称 / 架构 x86_64 / 显示 1280×800 / 核心 2）；卡片「资源」（运行时间、内存条 + 已用/总量、任务数、窗口数、IPC 收发/丢弃）。
**个性化**：卡片「颜色」（8 个 40×28 色块，选中描边 + 名称）；卡片「背景」（3 个 120×48 渐变预览卡，选中 2px 描边）；底部一张 200×90 的窗口配色预览。
**应用**：卡片「正在运行」（每行：名称 + pid + 状态，右侧 56×22「结束」按钮，进入确认态变红）；卡片「可启动」（三个 100×26 按钮，运行中置灰）。
**关于**：卡片「版本」（版本号、里程碑 M1..M9 一行摘要）；卡片「说明」（内核 C / 用户态 Rust / 设置不落盘）。

## 交互

- 悬停 80ms 变色；点击导航立即切页并整窗重绘；色块/背景点击即应用（全桌面重绘）。
- 结束任务两段式：第一次点击变红「确认」，3 秒内再点才执行，超时自动回落。
- 键盘：Tab 暂不接管（客户端未实现焦点链），保留鼠标操作。

## SubAgent

- **code-explorer**
- Purpose: 复核 `sched.c` 的任务回收路径（`g_reap` 与 `task_reap` 的调用点）与 `nwm.c` 的 liveness sweep，确认新增 `sched_kill()` 后不会出现“已退出任务占槽不回收”或“窗口悬挂”。
- Expected outcome: 一份“kill 一个阻塞任务 / kill 一个正在运行的任务 / kill 设置自身”三种情形的状态迁移清单，作为实现与验收对照。