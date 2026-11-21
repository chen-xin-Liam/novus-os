# 模块划分：输入 / 输出 / 依赖 / 测试策略

| 模块 | 输入 | 输出 | 依赖 | 测试策略 |
| --- | --- | --- | --- | --- |
| C 扫描热路径 `c/core` | 字节块、规则表、长度、容量 | 命中偏移/规则号、启发式分数、模糊指纹、摘要 | 无系统依赖 | C 极简 harness：空输入、边界长度、重叠命中、跨块命中、坏 CRC、已知 SHA256/CRC 向量、EICAR 正例与良性负例 |
| Rust FFI 封装 `ffi.rs` | C 指针、长度、句柄 | 安全扫描接口、RAII 句柄、错误映射 | C core | 通过 Rust 集成测试间接覆盖空指针/错误码/释放；引擎池耗尽路径 |
| 特征库管理 `signature.rs` | 签名包字节（v1） | 可加载 DB、wildcard 展开后的精确 DB | CRC/SHA256、C DB 接口 | 正例库、CRC 损坏、版本不符、wildcard 分段展开测试 |
| 检测引擎 `detection.rs` | 命中集合、启发式分、模糊匹配 | 判定、置信度、规则号、分类 | C 热路径、特征库 | EICAR 正例、良性负例、阈值边界、变种相似度测试 |
| 扫描编排 `scanner.rs` / `scheduler.rs` | 路径、事件、定时任务 | 扫描报告、优先级队列、背压 | HAL、检测引擎、隔离区、日志 | 目录树扫描（EICAR + 良性）、优先级出队、队列满拒绝 |
| 启发式 `heuristic.rs` | 样本字节 | 0..100 风险分 | C `heuristic.c` | 可疑字符串/头部/良性文本评分区间测试 |
| 变种识别 `fuzzy.rs` | 样本字节、已知指纹库 | 最近变种 id 与相似度 | C `fuzzy.c` | 相同样本 100%、不同样本低于阈值 |
| 行为监控 `behavior.rs` | `RawEvent`（进程/文件/系统调用） | Allow / Deny / Quarantine、风险累计 | HAL 事件源、日志 | 事件序列回放、逐级升级到 Deny、风险衰减 |
| 沙盒控制 `sandbox.rs` | 可疑程序路径、策略字节 | 会话 id、被监控进程、退出结果 | HAL 进程/沙盒接口 | 策略编码、会话生命周期、能力不足时的错误路径 |
| 增量更新 `update.rs` | 更新包、当前版本 | 新版本、原子替换、回滚点 | SHA256/CRC、HAL 文件接口 | 合法包、旧版本拒绝、CRC 损坏、回滚 |
| 隔离区 `quarantine.rs` | 命中文件、元数据、恢复/删除请求 | 隔离记录、恢复文件、删除结果 | HAL 文件系统、日志 | stash → restore 往返一致性、delete、容量上限 |
| 日志告警 `logging.rs` | 结构化事件、级别 | 环形日志、去重统计、平台输出 | HAL 输出接口 | 去重窗口、级别过滤、事件不丢 |
| Linux 平台后端 `nd-platform-linux` | 内核事件、procfs、定时器 | HAL 统一事件与文件操作 | libc syscall / std，仅平台目录 | fanotify 不可用时轮询降级、能力探测、status 输出 |
| Novus 平台骨架 `nd-platform-novus` | Novus syscall / IPC | no_std HAL 实现与可编译镜像 | 只读参考 Novus OS | `make novus-app size-check`：目标编译 + 128 KiB 预算 |
| CLI 与守护入口 `nd-cli` | 命令参数、配置文件 | scan / selftest / monitor / schedule / quarantine / update / sandbox / status | Rust 核心与平台层 | 命令解析、退出码、端到端样本流程 |

## 依赖方向

```
nd-cli -> nd-platform-linux -> nd-platform-api
       -> nd-core (ffi -> C static lib)
nd-platform-novus -> nd-platform-api + nd-core -> C static lib (freestanding)
```

核心不反向依赖任何平台 crate；平台 crate 之间互不相识。
