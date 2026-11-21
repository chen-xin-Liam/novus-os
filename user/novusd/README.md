# NovusDefender / `novusd`

已整体并入 Novus OS 源码树，本目录是唯一的副本。它是一个已集成的内置守护应用，同时保留宿主 Linux 参考实现与回归测试。

- Novus 目标：`rust/nd-platform-novus`（`no_std`、无分配器，复用 `apps` crate 的 syscall/IPC ABI），最终扁平镜像由顶层 Makefile 嵌入 `out/NOVUS.EFI`，在 nsh 中用 `novusd <command>` 调用。
- 宿主参考：`rust/nd-platform-linux` 与 `rust/nd-cli`，用于 `make test` / `make test-e2e`。
- 检测核心：`c/core` + `rust/nd-core`，无平台依赖、固定容量缓冲。

## 快速开始（WSL Debian）

```bash
make build          # C 静态库 + Rust + glibc 全静态链接（宿主 CLI）
make test           # C 单测 + Rust 单测/集成测试
make test-e2e       # 样本扫描 → 检测 → 隔离 → 增量更新
make check-static   # 验证无动态链接
make check-core-purity
make novus-app size-check   # Novus 目标构建 + 体积检查
```

宿主产物：`out/novusd`（单一静态可执行文件）。
Novus 产物：`target/x86_64-unknown-none/release/novusd`，由顶层 Makefile 转换为 `out/apps/novusd.bin`。

## 命令

宿主 CLI：

```bash
out/novusd selftest
out/novusd scan /path --quarantine
out/novusd monitor 1000
out/novusd schedule /path 60000
out/novusd quarantine list
out/novusd quarantine restore <name>
out/novusd update <package.bin>
out/novusd make-update /tmp/pkg.bin 2
out/novusd sandbox /path/to/program
out/novusd status
```

Novus nsh（去掉宿主专属的 `make-update`）：

```
novusd status | selftest | scan <path> [--quarantine]
novusd quarantine list|restore <name>|delete <name>
novusd update <package> | schedule <path> <ms> | monitor [ms] | sandbox <app>
```

## 文档

- 顶层 `docs/NovusDefender 内置应用.md`：命令、协议、路径、能力降级矩阵与排障
- `docs/ARCHITECTURE.md`：整体架构、数据流、并发与降级
- `docs/FFI.md`：C ABI、所有权与错误约定
- `docs/HAL.md`：平台抽象接口与能力矩阵
- `docs/MODULES.md`：每个模块的输入/输出/依赖/测试策略
- `docs/ROADMAP.md`：分阶段路线图
- `docs/NOVUS_PORTING.md`：已完成的 Novus 集成结构与能力边界
- `docs/TESTING.md`：测试分层与端到端流程

## 关键约束

1. `c/core`、`rust/nd-core`、`rust/nd-platform-api` 不含 Linux 头文件、`/proc`、fanotify、eBPF、ptrace、seccomp 或 `std::os::unix`；由 `make check-core-purity` 强制检查。
2. 核心全部使用固定容量缓冲，不依赖分配器，以兼容 Novus OS 用户程序环境。
3. WSL 使用 glibc 全静态（`-C target-feature=+crt-static`），不需要 musl 工具链。
4. Novus 侧路径为 `/ProgramData/Security/NovusDefender` 与 `/Novus/logs/security`，不沿用 Linux 的 `/var`。
