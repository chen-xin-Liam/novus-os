> 英文版本：[docs/en/ABI.md](docs/en/ABI.md)

# Novus OS ABI 参考（已实现部分）

> 本文只列**已经开发完毕并冻结**的 ABI；规划中、原型或未落地的能力不列入。
> 事实源：`kernel/trap.c`、`kernel/ipc.h`、`wm/novus-wm/nwm_proto.h`、`kernel/nuc.h`、`security/nkm/nkm.h`、`fs/vfs.h`、`kernel/shared_mem.h`。
> 完整矩阵见 [COMPATIBILITY.md](COMPATIBILITY.md)；变更记录见 [CHANGELOG.md](CHANGELOG.md)；编码规范见 [CODESTYLE.md](CODESTYLE.md)。

---

## 1. 系统调用调用约定

| 项 | 约定 |
|---|---|
| 入口 | `syscall` 指令，向量 `0x100` |
| 调用号 | `rax` |
| 参数 | `rdi`、`rsi`、`rdx`、**`r10`**（第 4 参必须是 `r10`，不是 `rcx`） |
| 返回 | `rax`；负数表示错误，0 或非负表示成功/长度 |
| 用户指针 | 内核不直接解引用，按任务 CR3 逐页复制 |
| 路径上限 | 256 字节 |
| 单次 I/O | 4096 字节 |
| fd 起点 | 3 |
| 未知编号 | 每个编号只报一次日志，然后拒绝 |

---

## 2. 系统调用表（已实现）

唯一分发表：`kernel/trap.c` 的 `g_syscalls[]`。编号稀疏冻结，**只追加，不复用、不重排**。

| 编号 | 名称 | 说明 |
|---|---|---|
| 1 | `yield` | 让出 CPU |
| 2 | `write` | 串口日志输出（**不是文件写**） |
| 3 | `getpid` | 取进程号 |
| 4 | `ipc_send` | 发送 IPC 消息 |
| 5 | `ipc_recv` | 接收消息；阻塞依赖 `syscall; ret` 的 RIP 回退 2 字节重放 |
| 6 | `ipc_lookup` | 按名字查端口 |
| 7 | `ipc_stats` | IPC 统计 |
| 8 | `open` | 打开文件 |
| 9 | `read` | 读文件 |
| 10 | `fs_write` | 写文件 |
| 11 | `close` | 关闭 fd |
| 12 | `stat` | 取文件信息 |
| 13 | `opendir` | 打开目录 |
| 14 | `readdir` | 枚举目录项 |
| 15 | `mkdir` | 建目录 |
| 16 | `power_off` | 关机 |
| 17 | `reboot` | 重启 |
| 18 | `session` | 会话控制 |
| 19 | `setup` | 安装器服务（setup pid 授权） |
| 20 | `unlink` | 删除 |
| 21 | `rename` | 重命名（仅 NovusFS 根支持） |
| 22 | `mount` | 挂载 |
| 23 | `umount` | 卸载 |
| 24 | `mounts` | 查询挂载表 |
| 25 | `update` | 更新服务控制 |
| 26 | `https` | HTTPS 客户端控制 |
| 27 | `console` | 串口控制台读/写 |
| 28–37 | DWM | `vmem_alloc/free/map/grant`、`surface_present`、`wait_vsync`、`set_exclusive`、`set_cursor`、`dwm_query`、`set_mode`（受 `CONFIG_NOVUS_DWM` 门控） |
| 40 | `win32` | Win32 API 门（仅 `TASK_ABI_WIN32` 任务可调用） |
| 41 | `pe32_exec` | 从 VFS 执行 PE32 镜像 |
| 60 | `exit` | 退出进程 |

Win32-ABI 任务只放行 **40** 与 **60**，其余调用由分发器拒绝。

---

## 3. NUC 可执行格式（V1.0 / ABI `0x0100`）

| 字段 | 值 |
|---|---|
| 魔数 | `NUC!`（`4E 55 43 21`） |
| 头长度 | 48 字节（`0x30`），`reserved` 16 字节 |
| `abi_version` | 高字节 major=1，低字节 minor=0 |
| 段 | 最多 16 个；仅 `TEXT` 可执行，其余 NX |
| 重定位 | 支持 `R_X86_64_32`、`R_X86_64_PC32`；类型 3 保留，V1.0 报 `NOVUS_EINVAL` |
| 栈 | 头部请求，内核 clamp 到 64 KiB–4 MiB；栈顶固定 `0xA0000000`，返回跳板 `0x90000000` |
| 入口 | `entry_rip = text.vaddr + entry_point`（`entry_point` 是 `.text` 段内字节偏移） |
| 签名 | 无签名；靠页表隔离（与 NKM 不同） |
| 执行路径 | boot-stage 从 ESP 固定路径 `APPS/DEMO.NUC` 暂存，内核 `nuc_load()` 加载 |

**兼容性**：major ≠ 1 拒绝；minor 高于或低于内核均允许（未知字段忽略）。通用 `./app.nuc` exec 尚未实现。

---

## 4. NKM 内核模块格式（V1.0 / ABI major 1）

| 字段 | 值 |
|---|---|
| 魔数 / 头长 | `NKM!` / 208 字节（`0xD0`） |
| 摘要 | SHA-256，覆盖整文件，但计算前把 `[0x50,0xB0)`（32 B hash + 64 B sig）置零 |
| 签名 | Ed25519 签 32 字节原始摘要 |
| 上限 | segment 1–16、deps ≤8、符号 ≤64、重定位 ≤256、模块 ≤16、根公钥 ≤4 |
| 重定位 | 只支持 64 位绝对重定位；PC32 拒绝 |
| 加载顺序 | 边界检查 → digest → signature → dependencies → map → relocate → init |

偏移与长度由 `security/nkm/nkm.c` 的 `_Static_assert` 固定。当前仅一把开发公钥，无运行时轮换。

---

## 5. IPC 与 Novus-WM 协议

### 5.1 内核 IPC

| 项 | 值 |
|---|---|
| 消息头 | 32 字节，magic `0x4E495043`（`NIPC`） |
| 校验 | FNV-1a checksum |
| 载荷 | ≤ 128 字节 |
| 队列 / 端口 | 每端口 8 条；静态端口表 16 个；任务收件箱端口 = pid |
| 命名端口起点 | `IPC_PORT_BASE = 0x1000`（避免与 pid 冲突） |
| 标志 | `IPC_F_SYNC 0x1`、`IPC_F_REPLY 0x2`、`IPC_F_BLOCK 0x4` |
| 返回码 | `0` 成功；`-1` EBADF、`-2` EBADMSG、`-3` EMSGSIZE、`-4` EBUSY、`-5` ENOENT、`-6` EAGAIN、`-7` ERETRY、`-8` ETIMEDOUT |

### 5.2 Novus-WM（代码版本 `0x00020000` / 2.0）

端口 `"nwm"`；shell 为 `NWM_PID_SHELL = 0`（本地内联分发）。

| 类别 | 消息号 |
|---|---|
| 请求 v1.x | `0x0101`–`0x0110`：CREATE/DESTROY/MOVE/RESIZE/RAISE/MINIMIZE/MAXIMIZE/RESTORE/FOCUS/DRAW/TITLE/EFFECT/CLEAR/SHAPE/TEXT/INVALIDATE |
| 请求 v2 | `0x0111`–`0x0116`：SURFACE_CREATE/DESTROY、FLIP_PRESENT、BLT_SUBMIT、SET_EXCLUSIVE、THUMBNAIL |
| 应答 | `0x0001` OK、`0x0002` ERR |
| 事件 | `0x0201`–`0x0206`：CREATED/MOUSE/KEY/FOCUS/RESIZE/CLOSE |

| 上限 | 值 |
|---|---|
| 窗口 | 10 |
| 每窗图元 / 文字 | 120 / 160 |
| 单段文字 / 标题 | 96 字节 / 24 字节 |
| 帧协议 | `CLEAR` → `SHAPE`/`TEXT` → **`INVALIDATE`**（缺 INVALIDATE 会半帧闪烁） |
| 模态标志 | `NWM_F_CREATE_MODAL = 0x1` |

v2 追加消息不改变 v1.x 语义：v2 客户端用 `SYS_VMEM_ALLOC` 分配显存对象，只把句柄交给 WM，像素不跨 IPC 边界。

---

## 6. 文件与目录 ABI

| 项 | 值 |
|---|---|
| 根 | 单一 `vfs_root()`；挂载表在路径前缀切换后端 |
| 挂载项 | 固定 8 项；路径绝对且唯一；`/` 不可卸载；跨挂载 `rename` 拒绝 |
| 路径上限 | 256 字节；不做 `..` 穿越消除；目录无 `..` 条目 |
| 文件类型位 | `VFS_MODE_FILE=1`、`VFS_MODE_DIR=2` |
| 后端 | NovusFS（可写）、FAT（只读 8.3）、ISO9660/Joliet（只读）、ext4（读+mkfs）、NTFS（仅枚举值，无后端） |
| 元数据 | `/.novusfs/mounts.json` ≤4 KiB / ≤8 条；运行时 `mount`/`umount` 不回写 |

**未实现**：`seek`、`truncate`、owner/rwx 权限、跨后端 `rename`。

---

## 7. 其他已冻结常量

| 项 | 值 |
|---|---|
| 共享内存 | 8 KiB @ `0x7E000`，NVSH v1.1；尾部 carve-out 有 4 KiB 日志环（冻结字段未动） |
| 任务 | `SCHED_MAX_TASKS=10`，每任务 64 KiB ring-0 栈，默认 4 tick 时间片 |
| 内核堆 | `kmalloc` 4 KiB 粒度，最多 256 MiB / 256 条记录 |
| 内置应用 blob | < 128 KiB，链接地址 `0x400000`；栈 64 KiB–4 MiB |
| 输入事件 | `ND_KEY_*` 位于 `0x110000` 之上（`ND_KEY_BASE`）；字符键直接携带 Unicode 码点 |
| sysinfo 请求 | `LANG 0x030F`、`DISPLAY 0x0318`、`RSP_DISPLAY 0x0319`、`NCSI 0x031A` |
| 图形上限 | 4 路 GOP 输出（镜像）；114,397 个 Unicode 字形 |

---

## 8. 变更规则

冻结格式（NUC/NKM/NUP/INS/IPC/更新/rootfs/共享内存/系统调用）的任何字段、偏移、编码或语义变更必须走 ABI 变更 RFC 并升主版本，同时同步六件套：规范文档、内核实现、宿主工具、测试夹具、版本号、兼容性说明。

只追加字段/消息号/调用号属于次版本变更；纯文字勘误必须标注"未改变字段偏移与 ABI 主版本"。详见 [CHANGELOG.md](CHANGELOG.md) 的高亮规则。
