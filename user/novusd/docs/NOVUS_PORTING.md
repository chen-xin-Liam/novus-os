# Novus OS 集成结构（已完成）

本项目已整体移入 Novus OS 源码树：`user/novusd` 是唯一副本，`NovusDefender` 原目录已删除。Novus 侧不再是"待手工合并的骨架"，而是由顶层 Makefile 构建并嵌入内核镜像的常驻应用。

## 目录与依赖

```
user/novusd/                     独立 workspace（宿主测试与 Novus app 共存）
├── c/core/                      C11 检测热路径
├── rust/nd-core/                无平台依赖检测核心（novus feature 切换落盘路径）
├── rust/nd-platform-api/        Hal trait 与固定容量结构（DirEntry name 上限 57）
├── rust/nd-platform-linux/      宿主参考实现
├── rust/nd-platform-novus/      Novus 守护：NovusHal + 命令状态机 + _start
└── rust/nd-cli, nd-tests        宿主 CLI 与回归测试
apps/src/novusd_proto.rs         nsh ↔ 守护的定长请求/响应协议
apps/src/shell/cmds.rs           nsh 的 `novusd` 内建命令
kernel/apps.c                    g_apps[] 末尾追加 novusd
Makefile                         独立 cargo/objcopy 规则与 128 KiB 体积检查
```

`nd-platform-novus` 以 path dependency 依赖 `../../../../apps`，直接复用 `sys`/`fs`/`ipc`/`sinfo`。apps 不反向依赖 novusd，无循环。守护二进制不定义 `#[panic_handler]`，复用 apps crate 的实现。

## 已验证的 ABI

- fd 基数 3（`fd < 3` 才是失败）；`PATH_MAX=255`；路径非空、无内嵌 NUL。
- `readdir` 返回 `1` 表示有条目、`0` 表示结束；`RawDirEntry` 名称最多 57 字节。
- `ipc::lookup` 传 NUL 结尾的 `b"sys\0"`；`spawn_name` 用 `F_SYNC` 并等待 `RSP_OK`。
- uptime 来自 `REQ_STATS/RSP_STATS`，`ticks * 10` ms。
- `APPS_NAMES`、`g_apps[]` 与 `KNOWN_APPS` 均只在末尾追加，spawn ID 稳定。
- 内核 `open(create)` 不截断，日志描述符在守护启动时打开并顺序追加。

## 能力边界

| HAL 能力 | Novus 状态 | 实现/降级 |
| --- | --- | --- |
| 文件读写与目录遍历 | 可用 | syscall 8–15/20/21，分块 ≤ 2048 B |
| 定时器 | `CAP_TIMER` | sysinfo uptime，10 ms/tick |
| 文件事件 | 降级 | 4×64 固定容量目录快照差分，检测增/删/大小或类型变化 |
| 进程追踪 | 降级 | sysinfo 任务表按名称定位，无连续事件流 |
| 系统调用审计 | 降级 | 内核无探针，仅保留行为状态机 |
| 沙盒 | 降级 | 策略记录 + spawn/kill + 截止时间，无内核强制隔离 |
| 命令传参 | 降级 | 文件式请求/响应，无 argv；同一时刻一个请求、响应上限 2066 B |

详细命令、路径与故障排查见仓库顶层 `docs/NovusDefender 内置应用.md`。

## 仍需 OS 侧支持（不改变当前降级行为）

- 内核文件事件通知或回调钩子
- 进程/系统调用审计事件源
- 通用、内核强制的沙盒策略（当前只有 Win32 ABI 限制与 PID 能力雏形）
