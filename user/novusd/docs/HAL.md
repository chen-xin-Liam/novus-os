# HAL 契约与能力矩阵

## 能力位

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `CAP_FILE_EVENTS` | `0x01` | 实时文件事件可用 |
| `CAP_PROC_TRACE` | `0x02` | 进程行为追踪可用 |
| `CAP_SYSCALL_TRACE` | `0x04` | 系统调用探针可用 |
| `CAP_SANDBOX` | `0x08` | 内核级隔离可用 |
| `CAP_TIMER` | `0x10` | 定时触发可用 |

`Hal::cap()` 返回当前位掩码；`Hal::degrade_reason(cap)` 返回缺失该能力时的人类可读原因（静态字节串）。

## 接口方法

```rust
fn fs_open(&mut self, path: &[u8], create: bool) -> Result<u32, i32>;
fn fs_read(&mut self, fd: u32, buf: &mut [u8]) -> Result<usize, i32>;
fn fs_write(&mut self, fd: u32, buf: &[u8]) -> Result<usize, i32>;
fn fs_close(&mut self, fd: u32);
fn fs_stat(&mut self, path: &[u8]) -> Result<FileStat, i32>;
fn fs_opendir(&mut self, path: &[u8]) -> Result<u32, i32>;
fn fs_readdir(&mut self, fd: u32) -> Result<Option<DirEntry>, i32>;
fn fs_mkdir(&mut self, path: &[u8]) -> Result<(), i32>;
fn fs_unlink(&mut self, path: &[u8]) -> Result<(), i32>;
fn fs_rename(&mut self, from: &[u8], to: &[u8]) -> Result<(), i32>;

fn cap(&self) -> u32;
fn degrade_reason(&self, cap: u32) -> &'static [u8];

fn events_next(&mut self, ev: &mut RawEvent) -> Result<bool, i32>;
fn now_ticks(&self) -> u64;
fn msleep(&mut self, ms: u32);

fn proc_start(&mut self, path: &[u8], policy: &[u8]) -> Result<u32, i32>;
fn proc_kill(&mut self, pid: u32) -> Result<(), i32>;

fn sandbox_create(&mut self, policy: &[u8]) -> Result<u32, i32>;
fn sandbox_destroy(&mut self, id: u32);

fn log(&mut self, level: u8, msg: &[u8]);
```

所有集合类型均为定长结构（`FileStat`、`DirEntry`、`RawEvent`），不使用动态容器；返回值用 `i32` 错误码。

## 降级矩阵

| 能力 | Linux 探测方式 | 不可用时的行为 | Novus OS 现状 |
| --- | --- | --- | --- |
| `CAP_FILE_EVENTS` | `fanotify_init` syscall | 目录快照轮询 + `/proc` 采样 | 无事件 syscall，固定为轮询 |
| `CAP_PROC_TRACE` | `/proc/self/status` 的 `CapEff` 位 19 | 仅做 `/proc` 进程集合采样 | 无对应设施 |
| `CAP_SYSCALL_TRACE` | `bpf()` syscall 探测 `EPERM` | 保留行为规则状态机，无内核事件 | 无探针 |
| `CAP_SANDBOX` | `CapEff` 位 21 (`CAP_SYS_ADMIN`) | 清理环境变量的策略模式执行 | 仅有 ABI/PID 能力雏形 |
| `CAP_TIMER` | std 定时器 | 始终可用 | 协作式 tick / sysinfo uptime |

降级原因由平台后端以静态字节串提供，`novusd status` 直接打印。
