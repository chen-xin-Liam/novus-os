# Novus OS 贡献指南

感谢参与 Novus OS 开发。本项目是一套从零实现的 x86_64 UEFI 操作系统，内核主体为 C，用户态应用为 Rust `no_std`，并包含自研文件系统、图形栈、窗口管理器、NUC 可执行格式、NKM 模块与 Win32 兼容层。

提交改动前请先阅读 [README.md](README.md)、[开发模板说明](templates/README.md) 和 `docs/` 下对应子系统的设计文档。大型改动建议先开 Issue 讨论方案，再提交 PR。

## 1. 环境准备

### 必需工具

- x86_64 交叉工具链：`x86_64-w64-mingw32-gcc`、`objcopy`
- Rust 与 `rustup`，以及以下 target：

  ```sh
  rustup target add x86_64-pc-windows-msvc
  rustup target add x86_64-unknown-none
  ```

- `make`、`gcc`、`bison`、`flex`、`libelf-dev`
- 镜像构建工具：`xorriso`、`mtools`、`grub-pc-bin`、`xxd`
- QEMU、OVMF 与 Python 3.8+（运行与回归测试）

在 Ubuntu/Debian 上可参照 `devcontainer.json` 安装依赖：

```sh
sudo apt update
sudo apt install -y build-essential gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
  binutils-mingw-w64-x86-64 mingw-w64 xorriso mtools grub-pc-bin \
  qemu-system-x86 ovmf bison flex libelf-dev
```

最低内存 2 GiB，推荐 8 GiB。若交叉工具链前缀不同，可用 `make CROSS=<prefix>` 覆盖。WSL 中若 `cargo` 不在 `PATH`，可先执行 `source "$HOME/.cargo/env"`。

## 2. 构建与运行

```sh
make                 # 构建 out/NOVUS.EFI
make run             # 构建并用 QEMU 启动，默认 MEM=4G
make iso             # 构建 out/novus-live.iso
make run-iso         # 从 ISO 启动
make disk-img        # 构建 out/novus-disk.img
make check-rootfs    # 校验 ISO、ESP 与系统盘目录树
make clean           # 清理 build/、out/、esp/
```

主机端工具：

```sh
make tools
make cppcc
make cppcc-demo
cargo test --manifest-path user/cppcc/Cargo.toml
```

`make install DEVICE=/dev/sdX` 会向块设备写入镜像，仅在确认设备名后使用。

## 3. 测试要求

最小验证：

```sh
make
make check-rootfs
```

涉及启动、UI 或交互的改动，请运行完整回归：

```sh
bash scripts/display-selftest.sh
python3 -m unittest discover -s scripts -p qemu_smoke_test.py
make disk-img
bash scripts/qemu-smoke.sh
bash scripts/qemu-smoke.sh --no-root --output out/qemu-smoke-no-root
```

全量洁净构建可使用：

```sh
make clean
make iso
python3 scripts/inspect_img.py out/novus.img
python3 scripts/check_rootfs.py out
```

`qtest.sh` 覆盖 IDE、AHCI、NVMe 和 USB 四种存储后端。`qtest.sh` 与 `scripts/build-all.sh` 目前包含仓库和工具链的绝对路径；若本机路径不同，请按实际情况调整后再运行，不要提交机器特定的绝对路径。

QEMU 截图基线位于 `scripts/baselines/qemu/{disk,no-root}`。缺失或不匹配都会导致测试失败；只有在 UI 有意变更时才可使用 `--update-baseline`，并必须人工检查生成的无损 RGB PNG 后再提交基线。

## 4. 按改动类型提交

### 内核、文件系统、图形与安全子系统

- 保持改动聚焦；新增模块应同步更新 `Kconfig`、`configs/novus_defconfig`、顶层 `Makefile` 和相关文档。
- 驱动目前主要编译进内核；动态模块使用 NKM 格式，必须遵循 [NKM 二进制格式规范](<docs/NKM 内核模块二进制格式规范 V1.0.md>)，且不得绕过 Ed25519 签名校验。
- NUC、NKM、IPC 等 ABI 或二进制格式发生变更时，必须提交 ABI 变更说明并升级版本号；参见 [NUC 格式规范](<docs/NUC 可执行二进制格式规范 V1.0.md>)。
- 不要提交私钥、证书或种子等敏感文件。

### 新增 GUI 应用

按 [模板说明 §1](templates/README.md) 完成四处注册：

1. 源码放入 `apps/src/bin/<name>.rs`。
2. 在 `apps/Cargo.toml` 增加 `[[bin]]`。
3. 在顶层 `Makefile` 的 `APPS_NAMES` 中追加名称。
4. 在 `kernel/apps.c` 的 `g_apps[]` **末尾**追加记录；数组索引会被复用，不能插在中间。

应用名只能使用字母、数字和下划线，不能包含 `-`，因为构建系统会生成 `k_app_<name>` 符号。若需出现在开始菜单或桌面磁贴，还要更新 `graphics/nd_demo.c` 与 `graphics/nvgui.c`。

### 新增 `nsh` 内建命令

按 [模板说明 §2](templates/README.md) 将命令函数加入 `apps/src/shell/cmds.rs`，并注册到 `BUILTINS`。命令应只读取 `cmd.argv`、`stdin`，只通过 `out.write(..)` 输出，并以返回值表示 `$?`。不要使用 `println!`，否则无法被管道或命令替换捕获。

### Nuomi 脚本

脚本由 `nsh` 内置解释器执行，无需编译或注册。请遵守脚本大小、对象总量、递归深度等限制，详见 [模板说明 §3](templates/README.md)。

### 生成物

主题、字体和输入法字典等由 `tools/` 下的生成器产出，部分结果会提交进仓库。修改配置后请运行对应生成命令并提交生成结果，不要只改生成物而遗漏源文件：

```sh
python3 tools/themegen/themegen.py
```

## 5. 关键约束

- `apps/` 是 `no_std`、无分配器环境：不要使用 `Vec`、`String`、`Box`，请使用固定数组和切片。
- 单个应用镜像必须小于 128 KiB（`nuc_spawn_blob()` 的缓冲上限）；NUC 栈范围为 64 KiB～4 MiB。
- 每帧显示列表最多 40 个图形、48 段文字；只在事件或主题变化后重绘并调用 `invalidate()`，空闲时调用 `sys::yield_now()`。颜色应使用主题令牌，不写死色值。
- 终端命令最多 40 个参数、单个参数 256 字节、管道输入 4 KiB。
- Nuomi 脚本上限为 6 KiB，对象总量 256 KiB，递归约 500 层；该解释器没有 GC 与 `input`。
- ISO El Torito 镜像上限为 32 MiB，新增壁纸、字体或资源时留意镜像体积。
- 项目坚持自研、不依赖外部开源项目；引入新依赖或新增大体积资源前请先讨论。
- 构建产物（`build/`、`out/`、`esp/`、`target/`）不得提交。

## 6. 代码风格与 PR 检查

仓库目前没有统一的 `clang-format`、`rustfmt.toml` 或 CI 配置。请遵循所在文件的既有风格，并保证：

- C 与主机工具代码不引入新的 `-Wall -Wextra` 警告；自检脚本按 `-Werror` 要求处理。
- Rust 代码使用 2021 edition，尽量执行 `cargo fmt`；避免不必要的 `unsafe`。
- 一个 PR 只做一件事；标题和描述说明动机、实现方式、兼容性影响与测试方法。
- 更新相关 README、`docs/`、设计文档或 `changes.md`。
- 在提交说明中列出实际执行过的构建和测试命令；未运行的检查请明确说明。
- PR 合并前确认以下清单：

```text
[ ] make 与 make check-rootfs 通过
[ ] 相关 QEMU / 单元测试通过
[ ] 新增或变更的资源符合镜像体积限制
[ ] ABI、协议和文档已同步更新
[ ] 无构建产物、私钥或机器特定绝对路径
```

本项目采用 GPLv2：提交者保留版权，所有修改源码必须按 GPLv2 条款开源。详见 [LICENSE](LICENSE) 与 [COPYING](COPYING)。
