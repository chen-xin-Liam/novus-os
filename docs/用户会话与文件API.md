# 用户会话与文件 API

## 文件系统

NovusFS 的 syscall 复用现有磁盘格式、WAL 和 Merkle 实现，不改变 inode/superblock 版本。内核选择启动时挂载的首个 NovusFS 磁盘分区；没有真实分区时文件 API 不可用。selftest ramdisk 会在自检结束时卸载，不作为用户数据盘。账号创建要求持久磁盘。

| 编号 | 名称 | 参数 | 返回值 |
|---|---|---|---|
| 8 | `open` | `rdi=路径`, `rsi=flags`（bit 0 创建） | fd，失败为 -1 |
| 9 | `read` | `rdi=fd`, `rsi=用户缓冲区`, `rdx=长度` | 字节数，单次最多 4096 |
| 10 | `fs_write` | `rdi=fd`, `rsi=用户缓冲区`, `rdx=长度` | 字节数，单次最多 4096 |
| 11 | `close` | `rdi=fd` | 0 或 -1 |
| 12 | `stat` | `rdi=路径`, `rsi=输出结构指针` | 0 或 -1 |
| 13 | `opendir` | `rdi=路径` | 目录 fd，失败为 -1 |
| 14 | `readdir` | `rdi=目录 fd`, `rsi=输出结构指针` | 1 有记录、0 到末尾、-1 错误 |
| 15 | `mkdir` | `rdi=路径` | 0 或 -1 |

fd 0-2 保留；应用 fd 从 3 开始，每进程最多 16 个，任务退出时随任务结构回收。路径最多 255 字节，支持 NovusFS 已有的 `/`、`.` 和 `..` 解析。用户指针需落在 user 页，写入用户缓冲区时还需有页级写权限。未登录时普通文件 syscall 被拒绝。

底层当前仍是 11 个直接块加一个单级间接块，最大文件大小约 2.04 MiB；不支持 seek、truncate、unlink 或 owner/rwx 权限。`apps/src/fs.rs` 提供 no_std Rust wrapper。fileman 的“磁盘”页可浏览目录；“建测试目录”和“写入往返测试”分别验证 `mkdir/readdir` 与 `open/write/close/open/read`。

## 用户会话

syscall 18 `session` 接收 `session_req_t`：`op/name_len/pass_len/pad`、`username[32]`、`password[64]`。操作：0 查询是否已有账号、1 首次 setup、2 login、3 logout。成功返回 0；错误返回负值。普通应用经 `apps_launch()` 启动前必须已登录。

账号保存在根目录 `/users.db`，记录为固定长度用户名、每账号 16 字节 salt 和 32 字节摘要，不写入明文密码。摘要使用 1024 轮 SHA-256；salt 由 RDTSC、PID、调度 tick 和启动内计数混合生成。用户名限 ASCII 字母、数字、下划线和连字符；密码为 4-63 个可打印 ASCII 字符。连续 5 次登录失败后锁定 3000 scheduler ticks（校准周期 10 ms，约 30 秒）。登录态仅保存在内存，重启后需重新登录。

注销结束 fileman、taskman、settings、netman 和 staged `demo`，保留 session manager。会话窗口的关机/重启调用 syscall 16/17，内核串口打印 `[POWER]` 日志；ACPI 不可用时调用返回错误并保持系统运行。

运行验收应使用带可写 NovusFS 分区的 `disk-img`，而不是只读/内存型 ISO 镜像。当前账号记录无迁移需求；无效或旧版本记录会被拒绝，需从磁盘镜像重新格式化后再首次建号。文件权限模型尚未实现，登录门禁不能替代文件 owner/rwx 隔离。
