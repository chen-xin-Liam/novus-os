# NovusDefender（`novusd`）内置应用

`novusd` 是常驻用户态安全守护，源码位于 `user/novusd`（独立 `no_std` workspace），最终扁平镜像嵌入 `out/NOVUS.EFI`。它复用 `apps/src/{sys,fs,ipc,sinfo}.rs` 作为 syscall/IPC ABI 的唯一来源，检测热路径由 C11 Aho-Corasick 引擎承担。

## 命令

在图形终端或串口 shell（nsh）中直接输入：

```
novusd status
novusd selftest
novusd scan <path> [--quarantine]
novusd quarantine list
novusd quarantine restore <name>
novusd quarantine delete <name>
novusd update <package>
novusd schedule <path> <ms>
novusd monitor [ms]
novusd sandbox <app>
novusd help
```

| 命令 | 行为 |
| --- | --- |
| `status` | 能力位与降级原因、签名库版本/规则数、uptime、任务/监控/沙盒会话数 |
| `selftest` | 用内置 EICAR 库扫描标准测试串，应返回 `MALICIOUS confidence=100 rule=1` |
| `scan` | 文件走 `scan_file`，目录递归深度上限 8；`--quarantine` 将非干净文件移入隔离区并写 `.orig` 记录 |
| `quarantine` | 基于固定目录枚举；`restore` 复制回原路径并删除记录，`delete` 永久删除 |
| `update` | 校验 `NDSUPD01` 包（CRC32 + SHA256 + 版本递增），原子替换签名库并自动重载 |
| `schedule` | 注册周期扫描（10 ms/tick），结果写入安全日志 |
| `monitor` | 目录元数据快照差分合成文件事件，喂给行为状态机；首次调用只建立基线 |
| `sandbox` | 记录策略、经 sysinfo 启动应用，截止时间到后由守护循环 kill |

`monitor` 与 `schedule` 是守护进程内部状态机，不会重复 spawn。首次任意 `novusd ...` 调用会启动守护；后续调用发现 `APPS_ERUNNING (-100)` 视为成功并复用同一实例。

## 请求协议

内核 spawn 不传 argv，因此采用文件通道：

| 记录 | 路径 | 大小 | 布局 |
| --- | --- | --- | --- |
| 请求 | `/temp/novusd/req.bin` | 256 B | `magic[8]` `seq:u32` `flags:u16` `len:u16` `args[240]` |
| 响应 | `/temp/novusd/rsp.bin` | 2066 B | `magic[8]` `seq:u32` `code:i32` `len:u16` `out[2048]` |

magic 分别为 `NDREQ001` / `NDRSP001`。`flags` bit0 表示 detach（计划用到，当前命令均同步返回）。两者均低于内核单次 I/O 上限 `FS_IO_MAX=4096`。命令参数以空格连接后写入 `args`，超出 240 字节截断。nsh 通过读取上次响应的 `seq+1` 生成序号，轮询响应直到序号匹配或 12 秒超时。

## 落盘位置

| 用途 | 路径 |
| --- | --- |
| 签名库 / 临时 / 备份 | `/ProgramData/Security/NovusDefender/sigdb.bin` / `.tmp` / `.bak` |
| 隔离区 | `/ProgramData/Security/NovusDefender/quarantine` |
| 安全日志 | `/Novus/logs/security/novusd.log` |
| 命令通道 | `/temp/novusd/req.bin`、`rsp.bin` |

以上目录均为 NovusFS 单根 VFS 下已存在或守护启动时递归创建的路径；不使用 Linux 的 `/var`、`/tmp`。

## 能力降级矩阵

| 能力 | 位 | Novus OS 实现 |
| --- | --- | --- |
| 文件事件 | `CAP_FILE_EVENTS` | 关闭；`monitor` 用固定容量（4 目录 × 64 项）快照差分，检测新建/删除/大小或类型变化 |
| 进程追踪 | `CAP_PROC_TRACE` | 关闭；只经 sysinfo 任务表按名称定位被沙盒启动的进程 |
| 系统调用审计 | `CAP_SYSCALL_TRACE` | 关闭；内核无探针，行为规则保留但无事件源 |
| 沙盒 | `CAP_SANDBOX` | 关闭；协作式策略记录 + spawn/kill + 截止时间（默认 1.5 s），无内核强制隔离 |
| 定时器 | `CAP_TIMER` | 开启；`sysinfo REQ_STATS` 的 uptime（`ticks * 10 ms`） |

`status` 会逐项打印关闭原因，不会把降级能力伪装成可用。

## 构建与验证

```bash
# 完整内核镜像（含 novusd blob，自动做 128 KiB 体积检查）
make

# 单独构建 novusd 扁平镜像
make out/apps/novusd.bin

# NovusDefender 自身的宿主回归测试
cd user/novusd && make test

# QEMU 冒烟（启动后输入 novusd status / selftest / scan ...）
bash scripts/test-novusd.sh
```

## 故障排查

- `novusd: daemon did not answer`：守护未启动或请求文件未写入。确认 `session_logged_in()`（FS syscall 需要登录），并用 `stat /temp/novusd/req.bin` 检查请求文件。
- `update failed: -4`：包版本未递增；`-5` 表示 CRC/SHA256 校验失败。
- `scan` 不递归：确认目标模式为 `VFS_MODE_DIR=2`；核心通过 `nd_platform_api::is_dir` 同时识别 POSIX `0x4000` 与 Novus 值。
- 日志为空：日志描述符在守护启动时打开并顺序追加；重启会从头覆盖，属预期的固定容量行为。
