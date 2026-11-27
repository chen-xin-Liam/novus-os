> 英文版本：[docs/en/CONTRIBUTING.md](docs/en/CONTRIBUTING.md)

# Novus OS 贡献指南

感谢参与 Novus OS 开发。

Novus OS 是 x86_64 UEFI 桌面操作系统：内核主体为 C，内置应用为 Rust `no_std`，并包含自研 NovusFS、NovusDisplay、Novus-WM、NUC 可执行格式、NKM 内核模块格式与 Win32 兼容层。项目采用 GPLv2，最低内存 2 GiB（引导拒绝线），推荐 4–8 GiB（早期文档以 4 GiB 为工作线），并坚持“不依赖外部开源项目、全部自研”的约束。

本文按“先跑起来 → 再理解结构 → 然后动手 → 最后验证”的顺序组织。新手建议先读 [§2.2 第一次构建](#22-第一次构建逐步走通)，再按改动类型查 [§10 常见开发任务配方](#10-常见开发任务配方recipes)；遇到启动失败或测试不过时查 [§11 故障排查手册](#11-故障排查手册troubleshooting)。

> **规范优先级**：遇到文档与代码不一致时，以当前源码、冻结格式规范和实际构建结果为准。`README.md` 的部分章节保留了历史规划，例如 `system/`、`drivers/`、`user/shell/`、`user/micropython/` 和 `make menuconfig`，这些与当前工作树并不一致。若修正文档或发现冲突，请在 PR 中说明。

大型或跨子系统改动建议先开 Issue 讨论设计；涉及 ABI、安全、启动顺序或资源格式的改动，必须在 PR 中给出兼容性说明。

---

## 1. 开始之前

### 1.1 必读资料

按贡献方向选择阅读：

| 方向 | 必读 |
|---|---|
| 所有贡献者 | [README.md](README.md)、[开发模板说明](templates/README.md)、[changes.md](changes.md)、[LICENSE](LICENSE) |
| 内核启动、驱动、中断 | [中断体系](<docs/中断体系.md>)、[Debug 级接口手册 V1.0](<docs/Novus OS Debug 级接口手册 V1.0.md>) |
| 文件系统、目录树、镜像 | [根目录结构与挂载规范](<docs/根目录结构与挂载规范.md>)、[用户会话与文件 API](<docs/用户会话与文件API.md>) |
| 终端与内置应用 | [终端与命令解释器设计](<docs/终端与命令解释器设计.md>)、[M8 内置系统应用](<docs/M8 内置系统应用.md>)、[设备管理器](<docs/设备管理器.md>) |
| GUI、主题、字体、中文 | [Novus-WM 与 IPC 协议规范](<docs/Novus-WM 与 IPC 协议规范 V1.0.md>)、[Novus UI 设计语言规范](<docs/Novus UI 设计语言规范 V1.0.md>)、[中文支持与 Win11 桌面](<docs/中文支持与 Win11 桌面.md>) |
| NUC / NKM / 安装包 / 快捷方式 | [NUC 格式规范](<docs/NUC 可执行二进制格式规范 V1.0.md>)、[NKM 格式规范](<docs/NKM 内核模块二进制格式规范 V1.0.md>)、[NUP 格式规范](<docs/NUP 应用安装包格式规范 V1.0.md>)、[INS 格式规范](<docs/INS 快捷方式文件格式规范 V1.0.md>) |
| cppcc / Nuomi / Win32 | [M7 cppcc 实现说明](<docs/M7 cppcc 实现说明.md>)、[cppcc 构建与端到端验证](<docs/cppcc 构建与端到端验证.md>)、[Mochi 语言规范](<docs/Mochi语言规范.md>)、[M9 Win32 兼容层原型](<docs/M9 Win32 兼容层原型.md>)、[user/cppcc/README.md](user/cppcc/README.md) |
| 安装器、会话、多语言、更新 | [图形安装向导设计与实现](<docs/图形安装向导设计与实现.md>)、[多语言、会话与 M11 输入输出](<docs/多语言、会话与 M11 输入输出.md>)、[分级增量更新体系规范](<docs/Novus OS 自研分级增量更新体系完整规范.md>) |

`docs/M7 cppcc 实施方案.md` 是历史方案，权威口径以 `user/cppcc/README.md` 和实现说明为准。`.codebuddy/`、`.trae/` 下的内容是工程过程资料，不是项目规范。

### 1.2 真实源码地图

```text
arch/x86_64/       UEFI 入口、引导阶段、内核入口、AP 跳板、汇编异常与工具
kernel/            调度、系统调用、NUC 加载、内存、中断、设备、网络、Win32 加载器
fs/                VFS、NovusFS、FAT、ISO9660、ext4 与挂载配置
security/          CAP、TLS、更新校验、NKM 加载器
graphics/          NovaDisplay、控件库、桌面壳层、字体、主题、IME、壁纸
wm/novus-wm/       内核态合成器与窗口协议；nwm_proto.h 是协议真源
apps/              Rust no_std 用户态运行时、内置应用、nsh、Nuomi 解释器
templates/         GUI 应用、nsh 命令、Nuomi 脚本模板
user/              Nuomi_pl 参考实现、cppcc、win32compat、ssl
tools/             主机端镜像/打包/签名/生成器与 tools.mk
scripts/           构建辅助、镜像检查、自检、QEMU 测试
configs/           当前真正生效的 defconfig
assets/            壁纸等资源
docs/              设计与冻结格式规范
```

注意：根目录没有 `system/`、`drivers/`、`user/shell/`、`user/micropython/`；内置应用位于 `apps/src/bin/`，shell 位于 `apps/src/shell/`，网卡等驱动位于 `kernel/`。MicroPython 目前只在 README 中作为路线图出现，没有实现源码或构建目标，贡献前必须先讨论设计。

### 1.3 文档导航：按任务读什么

仓库文档较多且部分保留历史规划。下表是**按目的**的最短阅读路径，避免一上来通读 100 KB 规范：

| 你想做的事 | 先读 | 再读 |
|---|---|---|
| 理解系统怎么分层、数据怎么流 | [ARCHITECTURE.md](ARCHITECTURE.md) | [Novus-WM 与 IPC 协议规范](<docs/Novus-WM 与 IPC 协议规范 V1.0.md>) |
| 知道哪些功能已完成、哪些还没做 | [ROADMAP.md](ROADMAP.md) §2/§4 | [changes.md](changes.md) |
| 判断某个"功能"是不是真的实现了 | [SECURITY.md](SECURITY.md) §5 + 相应源码 | [ROADMAP.md](ROADMAP.md) §0.3 漂移清单 |
| 改启动/驱动/中断 | [ARCHITECTURE.md](ARCHITECTURE.md) §2 | [中断体系](<docs/中断体系.md>)、[Debug 级接口手册](<docs/Novus OS Debug 级接口手册 V1.0.md>) |
| 改 GUI 应用/主题 | [templates/README.md](templates/README.md) | [Novus UI 设计语言规范](<docs/Novus UI 设计语言规范 V1.0.md>) |
| 改 nsh 命令 | [终端与命令解释器设计](<docs/终端与命令解释器设计.md>) | [templates/shell-command/README.md](templates/shell-command/README.md) |
| 改编译器/语言 | [user/cppcc/README.md](user/cppcc/README.md)、[M7 cppcc 实现说明](<docs/M7 cppcc 实现说明.md>) | [C++11 支持矩阵](<docs/C++11 支持矩阵.md>) |
| 准备 PR | 本文 [§9](#9-代码风格与提交规范) | 本文 [§13 PR 描述模板](#13-pr-描述模板) |

三类文档要区别对待：

1. **冻结格式规范**（NUC/NKM/NUP/INS/IPC/更新）：字段与版本不可私改，改则升版本并同步六件套。
2. **设计说明文档**（`docs/` 下大部分）：描述已实现的行为，若与代码冲突应提交勘误，而不是修改代码去迁就旧文档。
3. **工程过程资料**（`.codebuddy/`、`.trae/`）：记录当时的计划与决策，不是规范；其中的 pending 项可能早已完成。

---

## 2. 环境准备

### 2.1 必需工具

- x86_64 交叉工具链：`x86_64-w64-mingw32-gcc`、`objcopy`
- Rust 与 `rustup`，以及两个 target：

  ```sh
  rustup target add x86_64-pc-windows-msvc   # 内核 Rust 静态库：COFF
  rustup target add x86_64-unknown-none      # 内置应用：扁平 NUC blob
  ```

- `make`、`gcc`、Python 3、`xxd`
- QEMU、OVMF、`qemu-img`（运行和回归测试）
- `bison`、`flex`、`libelf-dev`（devcontainer 依赖清单）

Ubuntu/Debian 可参照 `devcontainer.json` 安装：

```sh
sudo apt update
sudo apt install -y build-essential gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
  binutils-mingw-w64-x86-64 mingw-w64 xorriso mtools grub-pc-bin \
  qemu-system-x86 ovmf bison flex libelf-dev python3 xxd
```

> `xorriso`、`mtools`、`grub` 出现在 devcontainer 中，但当前顶层镜像由自研 `mkfat`/`mkiso` 生成，它们不是普通 `make` 的直接依赖。

按需安装的可选工具：

| 场景 | 依赖 |
|---|---|
| `wm-churn.sh` | `socat` |
| 更新服务/E2E/证书 | `openssl` |
| 壁纸重采样 | Python `Pillow` |
| 字体/IME 生成器 | GNU Unifont hex 源文件、ibus 拼音表；源文件不随仓库分发 |
| Windows QEMU 验收 | PowerShell 7；部分发行版需用 `subst` 挂载 ASCII 盘符 |

WSL 中若 `cargo` 不在 `PATH`：

```sh
source "$HOME/.cargo/env"
# 若仍找不到，可显式加入工具链目录，例如：
# export PATH="$HOME/.rustup/toolchains/stable-x86_64-unknown-linux-gnu/bin:$HOME/.cargo/bin:$PATH"
```

交叉工具链前缀不同时可用 `make CROSS=<prefix>` 覆盖。

### 2.2 第一次构建：逐步走通

以下命令在干净工作树上依次执行。每一步都有可观察的"成功标志"，任一步失败就停在那里排查，不要跳到下一步。

```sh
# ① 确认工具齐全（缺哪个装哪个）
x86_64-w64-mingw32-gcc --version
rustc --version && cargo --version
rustup target list --installed | grep -E 'x86_64-pc-windows-msvc|x86_64-unknown-none'
qemu-system-x86_64 --version

# ② 主机侧快速自检：不依赖 QEMU，先排除工具链问题
bash scripts/display-selftest.sh
bash scripts/irq-selftest.sh

# ③ 内核与应用构建
make
# 成功标志：out/NOVUS.EFI 存在，链接无 undefined symbol

# ④ 镜像结构与目录树校验
make check-rootfs
# 成功标志：ISO 主描述符/Joliet、ESP 8.3 名、字体与 .novusfs 元数据全部通过

# ⑤ 启动到桌面（headless，串口日志写到终端）
make run
# 成功标志见下方"启动日志关键标记"
```

`make run` 启动后，串口应依次出现这些标记（VNC 只监听 `127.0.0.1:5901`）：

```text
[BOOT] UEFI boot started
[SHM] shared memory verified (NVSH v1.1)
[BOOT] display bound
[BOOT] power and storage probed
[BOOT] device table built
[BOOT] ipc ready
[BOOT] filesystem selftests done
[BOOT] mounts applied
[M3] integrity: PASS        （live 无系统分区时可能是 NOT BUILT）
[NW] NCSI connected         （需要 QEMU 用户态网络）
[SESSION] live session: no account required
[ND] demo desktop painted
[HALT] entering the scheduler
```

如果串口没有输出，先确认 `CONFIG_NOVUS_DEBUG_SERIAL=y` 且 QEMU 没把串口重定向到别处；如果画面停在启动 splash，看最后一条 `nd_boot_stage` 文案定位卡在哪一步。更系统的排查见 [§11](#11-故障排查手册troubleshooting)。

**首次改动推荐的最短闭环**（不要一上来改内核）：

1. 用 `templates/gui-app/` 单独编译一个窗口应用，确认工具链正常。
2. 按 [§10.1](#101-加一条-nsh-内建命令) 加一条 `nsh` 命令，`make` 后在终端里执行它。
3. 按 [§10.2](#102-新增一个-gui-应用) 把应用接进系统，跑一次完整 QEMU 冒烟。

---

## 3. 配置与构建

### 3.1 配置机制

当前没有 Kconfig 解析器，也没有 `make menuconfig`：

- `configs/novus_defconfig` 是真正被 `Makefile` 通过 `-include` 读取的配置源。
- 顶层 `Kconfig` 只是声明式占位文件，新配置项必须手工同步到 defconfig。
- `y`/`n` 仅转换为 C 宏 `1`/`0`；源码列表本身固定，功能门控要写 `#if CONFIG_NOVUS_*`。
- Kconfig 中的 `depends on` 不会自动校验，需要人工保证，例如 `IRQ → SCHED`、`GUI → DISPLAY`、`WM → GUI + IPC`。
- `CONFIG_USER_MOCHI` 和 `CONFIG_USER_CPPCC` 都可能占用 `APPS/DEMO.NUC` 槽位；当前工作树没有 `user/mochi` 子模块。
- `E2E=1` 是独立 make 变量，会加入测试根证书和 HTTPS 探针；所有相关 `make` 调用必须一致带 `E2E=1`，不能混用对象。
- `CONFIG_NOVUS_NTP=y` 默认开启：DHCP 拿到地址后用 DNS 解析 `ntp.aliyun.com`（失败回退 `203.107.6.88`），经 UDP/123 每小时校准软件时钟；串口日志标记为 `[NTP]`。

### 3.2 常用命令

```sh
make                 # 默认：out/NOVUS.EFI
make run             # 构建并启动 QEMU，默认 MEM=4G
make iso             # out/novus-live.iso
make run-iso         # 从 ISO 启动，可附加第二块磁盘
make disk-img        # out/novus-disk.img
make check-rootfs    # 校验 ISO/ESP/系统盘目录树与字体
make clean           # 删除 build/、out/、esp/
```

主机工具与专用目标：

```sh
make tools                 # mkfat、mkiso、mkfsnovus、mkdisk、wpkg、novupkg
make cppcc                 # C++98 子集 → NUC 编译器
make cppcc-demo            # 生成 out/cppcc/hello.nuc
make nkm-test-module       # 重新生成 NKM 测试模块与内嵌头
make wpkg-img              # 生成壁纸包
make theme                 # 重新生成主题令牌
make novus-img             # 生成 out/novus.img
make esp                   # 生成 out/esp.img 与 out/esp-tpl.img
make iso-force             # 无条件重建 ISO
```

`make install DEVICE=/dev/sdX` 会依赖 `disk-img` 并用 `dd` 覆盖目标设备的全部数据。脚本要求显式 `DEVICE` 和手工输入 `yes`；请反复确认设备名。测试安装建议使用 `scripts/run-install-test.sh`，默认只写 `/tmp/novus-install-target.img`。

### 3.3 可覆盖变量

| 变量 | 默认值 | 用途 |
|---|---|---|
| `CROSS` | `x86_64-w64-mingw32-` | 交叉工具链前缀 |
| `MEM` | `4G` | `make run`/`run-iso` 的客体内存 |
| `E2E` | `0` | 是否编入 HTTPS E2E 测试锚与探针 |
| `HOSTCC` | `gcc` | 主机工具编译器 |
| `HOSTCFLAGS` | `-O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE` | 主机工具编译参数 |
| `SYS_SIZE_MB` | `8` | NovusFS 系统镜像 |
| `ESP_TPL_SIZE_MB` | `16` | 安装模板 ESP |
| `ESP_SIZE_MB` | `31.5` | live ESP（支持小数 MiB） |
| `DISK_SIZE_MB` | `64` | 完整磁盘镜像 |
| `NKM_SEED` | 固定种子 | 测试密钥种子；随意更换会导致与内核内嵌公钥不匹配 |
| `DEVICE` | 无 | `make install` 的目标，必须显式提供 |
| `NOVUS_NIC` | `e1000` | `scripts/run.sh` 的 QEMU 网卡模型 |
| `DISPLAY` | 空 | 非空时 `run.sh`/`run-iso.sh` 使用 gtk，否则 headless |

### 3.4 产物与体积

| 产物 | 说明 |
|---|---|
| `out/NOVUS.EFI` | UEFI PE32+ 内核，包含 AP trampoline 与 Rust 静态库；入口是 `efi_main` |
| `out/apps/<name>.bin` | 内置应用扁平镜像，`xxd` 后生成 `k_app_<name>` 符号 |
| `out/novus.img` | 8 MiB NovusFS 系统镜像，含 `/Novus/fonts/font.bin` |
| `out/esp-tpl.img` | 16 MiB 安装模板 FAT ESP |
| `out/esp.img` | 31.5 MiB live FAT ESP |
| `out/novus-live.iso` | ISO 9660 + Joliet + El Torito + isohybrid |
| `out/novus-disk.img` | 默认 64 MiB，GPT + ESP + NovusFS |
| `out/qemu-smoke*` | QEMU 冒烟截图、串口日志、结果 JSON |

El Torito 的 sector count 只有 16 位、单位 512 字节，因此启动项必须严格小于 32 MiB；live ESP 取 31.5 MiB 是安全余量（`mkfat` 的 size 参数支持小数 MiB）。FAT ESP 只支持 8.3 短名，镜像内资源命名必须遵守。构建产物位于 `build/`、`out/`、`esp/` 和各处 `target/`，不得提交。

---

## 4. 测试与验证

### 4.1 必跑与按类型追加

| 改动 | 最小必跑 | 追加验证 |
|---|---|---|
| 任意改动 | `make`、`make check-rootfs` | PR 中列出实际执行过的命令 |
| 内核启动、调度、IRQ | 上行 + `bash scripts/irq-selftest.sh` | QEMU 冒烟；必要时 `scripts/irq-qemu-test.ps1` |
| FS、目录树、镜像打包 | 上行 + `python3 scripts/inspect_img.py out/novus.img` | `make disk-img`、QEMU 冒烟、`scripts/font_smoke.sh` |
| GUI、主题、字体、IME、多语言 | 上行 + 完整 QEMU 冒烟 | `scripts/wm-churn.sh 4G 20`、生成器 `--check`、截图人工核验 |
| 窗口/应用生命周期 | 完整 QEMU 冒烟 | `scripts/wm-churn.sh 4G 20 [--iso]` |
| 存储驱动 | `make iso` | `bash qtest.sh`，人工检查 `[MOUNT]` 标记 |
| 安装器 | `make iso` | `scripts/run-install-test.sh 256`；英文用 `EN=1`，桌面路径用 `FLOW=desktop` |
| 更新、TLS、CA | 常规构建 | `tools/updatesrv/selftest.py`、`novupkg selftest`、E2E |
| NKM/NUC/ABI | 常规构建 | `nkmgen selftest`、`make nkm-test-module`、格式规范要求的相关测试 |
| cppcc | `cargo test --manifest-path user/cppcc/Cargo.toml` | `make cppcc-demo` + `make run` 检查串口标记 |
| Nuomi 实现 | 宿主参考实现测试 + `nsh` 实测 | `user/Nuomi_pl/test-linux.sh`、`python3 user/Nuomi_pl/tests/nuc/run-tests.py` |
| 发版/大重构 | 全量洁净重建 | 下节完整命令 |

### 4.2 标准回归命令

```sh
# 主机单测/自检
bash scripts/display-selftest.sh
bash scripts/irq-selftest.sh
python3 -m unittest discover -s scripts -p qemu_smoke_test.py
python3 -m unittest discover -s scripts -p novusimg_test.py
cargo test --manifest-path user/cppcc/Cargo.toml

# 镜像结构与全量洁净构建
make clean
make iso
python3 scripts/inspect_img.py out/novus.img
python3 scripts/check_rootfs.py out

# QEMU 启动与交互
make disk-img
bash scripts/qemu-smoke.sh
bash scripts/qemu-smoke.sh --no-root --output out/qemu-smoke-no-root

# 应用生命周期（需要 socat）
scripts/wm-churn.sh 4G 20
```

工具自检：

```sh
make tools
build/tools/novupkg selftest
(cd tools/nkmgen && cargo run --quiet -- selftest)
python3 tools/updatesrv/selftest.py
python3 tools/themegen/themegen.py --check
python3 tools/langgen/langgen.py --check
```

QEMU 冒烟支持 `QEMU_BIN`、`OVMF_CODE`、`OVMF_VARS` 或 `--qemu`、`--code`、`--vars` 指定路径；测试使用临时固件变量和 `-snapshot`，不修改源镜像。VNC 只监听 `127.0.0.1:5901`，端口占用会直接失败。

截图基线位于 `scripts/baselines/qemu/{disk,no-root}`。缺失或不匹配均失败；**普通回归不得带 `--update-baseline`**。只有 UI 有意变更时才更新，并必须逐张人工检查脚本生成的无损 RGB PNG 后提交。

### 4.3 含机器特定路径的脚本

以下脚本内含 `/home/dev/...` 或固定 OVMF 路径，换机时必须先调整；不要把个人路径提交进仓库：

- `qtest.sh`
- `scripts/build-all.sh`
- `scripts/build-img.sh`
- `scripts/rebuild-kernel.sh`
- `tools/updatesrv/e2e-run.sh`
- `scripts/run.sh`、`scripts/run-iso.sh`、`scripts/font_smoke.sh`、`scripts/wm-churn.sh`、`scripts/run-install-test.sh`

`qtest.sh` 依次测试 IDE、AHCI、NVMe、USB xHCI，失败不会中断脚本；必须人工查看 `/tmp/b_*.log` 中的 `[MOUNT]` 标记。仓库没有 CI，未执行的检查必须在 PR 描述中明确说明。

### 4.4 日志、标记与调试入口

内核没有 `log_print` 分级接口（见 Debug 手册 12.0 差异声明），实际输出是 `kputs()` 同时镜像到 VGA、COM1、启动日志环和（可选）framebuffer 控制台。所以**串口日志就是主要调试手段**。

常见日志前缀与含义：

| 前缀 | 来源 | 看什么 |
|---|---|---|
| `[BOOT]` | `kernel/kmain.c` | 初始化阶段信标；最后一条信标就是卡住的调用 |
| `[SHM]` / `[M3]` / `[MRKL]` | 共享内存 / Merkle | 交接区校验、系统分区完整性结论 |
| `[ND]` / `[CRTC]` | NovusDisplay | 帧缓冲绑定、合成器、回扫探测、自检 |
| `[NOVS]` / `[WAL]` | NovusFS | 超级块、事务与校验告警（多为 WARN 降级） |
| `[BLK]` / `[USB]` / `[NET]` / `[DNS]` / `[NCSI]` | 驱动与网络 | 设备枚举、链路、DHCP/DNS/联网检测 |
| `[SMP]` / `[PMM]` / `[HEAP]` | 调度/物理内存/内核堆 | AP 唤醒、页分配告警、重复释放 |
| `[APP]` | `apps/` 用户态 | `no_std` 应用的 panic 与分段落点 |
| `#PF` / `fault` | trap 处理 | 用户态异常；单次 nxprobe 自测属预期 |

调试手段按侵入性从低到高：

1. **串口日志分段落点**：用户态用 `sys::write(b"...\n")`，内核用 `kputs()`/`kputs_hex()`。
2. **启动阶段日志面板**：splash 直接读共享内存 4 KiB 环，可在图形界面出现前看到固件日志。
3. **主机自检**：`display-selftest.sh`、`irq-selftest.sh`、`dwm-selftest.sh` 把内核源码编到宿主上跑，适合定位纯算法问题。
4. **QEMU monitor / HMP**：`qemu-smoke.sh` 用 HMP 截屏并抓串口；`ci_smoke.py` 输出结构化 JSON。
5. **VNC 交互**：`127.0.0.1:5901`；headless 下 `mouse_move` 不驱动客户机光标、`sendkey` 不可靠，交互验收优先用 QMP/绝对路径指针协议。
6. **生成器 `--check`**：主题/语言/字体生成物过期时先重生成，别在生成文件里直接改。

> 不要在公开日志里输出内核地址、栈回溯、密钥材料或可复现攻击 payload（[SECURITY.md](SECURITY.md) §6）。

---

## 5. 内核、驱动与底层开发

### 5.1 启动与初始化顺序

1. UEFI 入口 `efi_main`：串口/控制台、块设备、内存自检（低于 2 GiB 直接拒绝）、`stage3_prepare/menu/load/finish`、`ExitBootServices()`。
2. `kernel_entry()`：建立 64 KiB BSP 栈、清 `.bss`、调用 `kmain()`；`kmain()` 不允许返回。
3. `kmain()`：PMM → MM → heap → 设备/块层 → IPC → NovusFS/VFS → NKM → 更新与 CA → LAPIC/SMP → 调度/IRQ/NUC 任务/语言/会话/网络/WM → `sched_start()`。

`kmain()` 的初始化顺序是硬约束：NKM 依赖 heap/PMM；NovusFS 自检必须在 AP 唤醒前完成，避免第二核心同时访问块层和 WAL；PCI BAR 探测需在存储驱动之后、网络栈之前。

### 5.2 新增 C 源或驱动

- 所有 C 文件必须显式加入顶层 `Makefile` 的 `SRC`；汇编加入 `ASM_SRC`；新目录加入 `INC`。当前没有源码自动发现。
- 添加配置开关时同步修改 `Kconfig`、`configs/novus_defconfig`、代码中的 `#if CONFIG_NOVUS_*` 和文档。
- 块设备用 `blkdev_register()`，设备对象必须是静态存储且注册后长期有效；块层对外统一为 512 字节扇区。
- 网卡用 `net_register()`，探测函数加入 probe 集合并在 `net_init()` 中调用；首个 UP 的设备成为活动链路。
- 中断用 `irq_register(line, flags, isr, device, name)`；每个 GSI 最多 4 个处理函数，共享线路必须同触发属性。
- ISR 中禁止 `sti`、睡眠、调度、分配内存、调用协议栈和打印日志；只清自己的设备中断源，EOI 由框架统一处理。
- 设备管理器只观测 `devtab_refresh()` 生成的设备树，应用不应自行扫硬件。
- NKM 可解析的内核符号必须加入 `nksym_table`；取地址同时防止链接器丢弃函数。
- 新驱动初始化调用点按硬件类别插入 `kmain()`，不能只加源文件。

AP trampoline 是特例：16 位裸二进制必须小于 4 KiB、无重定位，最后 24 字节是 CR3、AP 栈指针和 `ap_main` 三个 64 位槽；SIPI 失败应降级单核，不得 panic 或死锁。

### 5.3 系统调用 ABI

- 唯一分发表是 `kernel/trap.c` 的 `g_syscalls[]`；编号稀疏且冻结，不得复用、重排或“整理”。
- 调用约定：`rax` = 调用号，`rdi/rsi/rdx/r10` 传参（第 4 参必须是 `r10`），`syscall` 进入，负返回值表示错误。
- 内核不能直接解引用用户指针，必须按任务 CR3 逐页复制。
- 文件路径上限 256 字节，单次 I/O 上限 4096 字节，fd 从 3 开始。
- `ipc_recv` 的阻塞实现依赖“返回帧 RIP 回退 2 字节重放 syscall”，`syscall; ret` 的指令布局属于 ABI。
- Win32 兼容进程只允许 `SYS_WIN32(40)` 和 `SYS_EXIT(60)`；调度器记录 ABI 类型，分发器强制隔离。
- 用户侧头文件 `user/Nuomi_pl/include/novus_syscall.h` 必须与内核表同步，不能让工具链长期写裸数字。

### 5.4 进程、内存与栈

- 每个 NUC 进程一个任务、一个 CR3、一个独立内核栈和 trap frame；`SCHED_MAX_TASKS=10`，每任务 64 KiB ring-0 栈，默认时间片 4 ticks。
- `kmalloc()` 是页粒度分配器：按 4 KiB 向上取整，最多 256 MiB，最多 256 条分配记录；不要当通用小对象 slab 使用。
- 共享内核堆窗口为 `0xFFFF810000000000` 起始的 256 MiB，所有进程 CR3 共享、supervisor-only。
- PMM 用固定 256 KiB 位图，约覆盖 8 GiB 物理页；DMA 缓冲仍需显式走 PMM。
- NUC 栈底由 header 请求并被 clamp 到 64 KiB～4 MiB；栈顶固定 `0xA0000000`，返回跳板 `0x90000000`。
- NUC 最多 16 个 segment，仅 `TEXT` 可执行，其余 NX；只支持 `R_X86_64_32` 和 `R_X86_64_PC32`；无外部导入。NUC 不签名，靠页表隔离；NKM 必须签名。

### 5.5 文件系统与根目录

- VFS 只有一个 syscall 可见根 `vfs_root()`；挂载表负责在路径前缀切换后端，不形成多棵独立根树。
- 目录树与挂载点的唯一真源是 `fs/novusfs/rootfs_layout.h`，`docs/根目录结构与挂载规范.md` 是可读说明。
- 挂载表固定 8 项；挂载路径必须绝对且唯一，`/` 不允许卸载；跨挂载 `rename` 拒绝。
- VFS 不做 `..` 穿越消除，目录实现不应新增 `..` 条目；全路径 256 字节。
- `/.novusfs/mounts.json` 不超过 4 KiB、8 条，路径来自白名单；运行时 `mount/umount` 不回写，否则破坏 Merkle 校验。
- NovusFS 是可写根：双超级块、64 块 WAL、预留 Merkle 区、块位图、inode 表、数据区；格式冻结，不要移动已有块。
- FAT 与 ISO9660 都是只读后端：FAT 仅 8.3 短名，ISO 优先 Joliet；两者的写/删/改名直接失败。
- 当前 VFS 还支持 ext4；NTFS 只有枚举值，没有后端。

---

## 6. 安全、NKM 与冻结格式

- CAP 模型在规范和调试手册中定义为 128 位掩码，但当前系统调用表未暴露完整 `cap_check` 路径。新增权限检查时必须同步收敛实现、ABI 与文档，不能把规范文本当作已落地实现。
- NKM 是 Ring0 模块，普通加载必须 Ed25519 签名；签名覆盖“除 `[0x50,0xB0)` 置零外”的整文件 SHA-256 摘要。
- 格式限制：`NKM!`、ABI major 1、208 字节头、1–16 segment、deps ≤ 8、符号 ≤ 64、重定位 ≤ 256、模块 ≤ 16、根公钥 ≤ 4；只允许 64 位绝对重定位。
- 加载顺序：边界检查 → digest → signature → dependencies → map → relocate → init；不得调整。
- `NKM_FLAG_ALLOW_UNSIGNED` 仅用于管理员本次会话的显式调试，不得作为常规兼容方案，不得持久化信任。
- 偏移和长度由 `security/nkm/nkm.c` 的 `_Static_assert` 固定；改布局必须同步内核加载器、`nkmgen` 和规范。
- `nkm_exit()` 必须释放内存、IRQ 与设备注册；被其他模块依赖时拒绝卸载。

冻结格式/协议清单：

| 规范 | 变更要求 |
|---|---|
| NUC | ABI major 1；字段/语义变化需 ABI 变更评审并升版本 |
| NKM | 格式与签名算法冻结；同步静态断言、签名工具、规范和文档 |
| NUP / INS | 任何格式扩展必须升 Version |
| Novus-WM IPC | 代码常量为 `NWM_PROTO_VERSION 0x00020000`（2.0，surface redirection / Mini-DWM；文档标题仍写 V1.0）；版本、消息号、载荷布局不得私自改动 |
| Debug 接口 | 参数级与错误码级冻结 |
| 更新补丁体系 | 三大补丁体系永久冻结 |
| 根目录布局 | 以 `fs/novusfs/rootfs_layout.h` 为准，改布局要同步所有镜像校验 |

任何 ABI 变更 PR 都必须同时更新：规范文档、内核实现、宿主工具、测试夹具、版本号和兼容性说明。文本勘误要明确标注“未改变字段偏移和 ABI 主版本”。

---

## 7. GUI、窗口与内置应用

### 7.1 图形栈和协议

- `graphics/` 分层：帧缓冲 HAL → 2D 软件渲染器 → 控件库 → 桌面壳层；像素格式为 `0xAARRGGBB`，离屏合成保证只呈现完整帧。
- `wm/novus-wm/` 是内核态合成器；客户端只通过 `ipc_send/ipc_recv/ipc_lookup` 与端口 `"nwm"` 通信，无共享内存、无内核头依赖。
- IPC 消息头 32 字节，magic `0x4E495043`，checksum 为 FNV-1a；载荷 ≤ 128 字节，端口队列 8 条，静态端口表 16 个，任务收件箱端口等于 pid。
- 每窗显示列表硬限：**120 个图元、160 段文字**，单段文字 ≤ 96 字节，标题 ≤ 24 字节，系统最多 10 窗。旧文档中的 40/48 只是早期建议预算，不要写成协议上限。
- 一帧必须以 `CLEAR` 开始，随后 `SHAPE`/`TEXT`，最后 `INVALIDATE`；缺少 `INVALIDATE` 会导致半帧闪烁。`IPC_EBUSY` 必须重试。
- 客户端只在事件或主题变化后 `draw()` + `win.invalidate()`；空闲调用 `sys::yield_now()`，因为任务不会被抢占。
- 文本宽度必须用 `nd_text_width()`/`ui::width()`，禁止按“字节数 × 6”估算；中文需走码点边界和字体索引链路。
- 颜色只用语义令牌 `ui::bg()`、`card_bg()`、`row_sel()`、`text()`、`dim()`、`accent()`、`line()`，不得写字面色值。

### 7.2 新增 GUI 应用

当前内置应用共 8 个：`fileman`、`taskman`、`settings`、`netman`、`session`、`setup`、`nsh`、`devmgmt`。新增应用按 [模板说明 §1](templates/README.md) 完成：

1. 源码：`apps/src/bin/<name>.rs`。
2. Cargo 声明：在 `apps/Cargo.toml` 添加 `[[bin]]`。注意当前 `setup` 未显式声明，靠 Cargo 自动发现；新应用建议显式声明以避免歧义。
3. 构建清单：在 `Makefile` 的 `APPS_NAMES` 追加名称，生成 `out/apps/<name>.bin` 和 `k_app_<name>`。
4. 内核表：在 `kernel/apps.c` 的 `g_apps[]` **末尾**追加记录。数组索引被旧版 sysinfo `/apps` 和客户端复用，不能插到中间；新代码应优先使用 `apps_launch_name()`。
5. 桌面入口（如需）：`graphics/nd_demo.c` 的开始菜单、磁贴、任务栏回调，以及 `graphics/nvgui.c`/`nvgui.h` 的图标类型。

命名只能包含字母、数字和下划线，**不能含 `-`**，否则 `k_app_<name>` 不是合法 C 标识符。只有 `session`/`setup` 是 bootstrap 应用；其它应用必须已登录，同名实例唯一。启动后确认 `out/apps/<name>.bin` 小于 128 KiB。

### 7.3 用户态运行时约束

`apps/` 是 `no_std`、无分配器、无第三方依赖环境：

- 禁止 `Vec`、`String`、`Box`，使用固定数组/切片；所有缓冲自行开在栈或静态区。
- `panic` 只向 COM1 打印 `[APP] panic` 并 `exit(3)`，没有回溯；调试靠分段落点 `sys::write(b"...\n")`。
- 扁平镜像由 `cargo build --release --target x86_64-unknown-none` → `objcopy -O binary` → `xxd -i` 生成，链接地址固定 `0x400000`。
- 单应用 blob 必须小于 128 KiB；栈范围 64 KiB～4 MiB；`apps.stamp` 只跟踪 `apps/src/**`、`apps/Cargo.toml`、`apps/link.ld`、`apps/.cargo/config.toml`。

### 7.4 nsh 与命令

`apps/src/shell/` 分工：`path.rs` 展开绝对路径、`parse.rs` 分词与重定向解析、`mod.rs` 维护 Shell 状态和管道、`cmds.rs` 实现内建命令、`update.rs`/`crypto/` 处理更新与签名。

新增内建命令两步：

1. 在 `apps/src/shell/cmds.rs` 的其它 `cmd_*` 之后、`BUILTINS` 之前加入函数。
2. 在 `BUILTINS` 表中注册 `("name", cmd_name)`。

函数签名：

```rust
fn cmd_xxx(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32
```

约定：

- 参数已展开变量、glob 和引号；`MAX_ARGS=40`，单参数 `WORD_MAX=256`。
- `stdin` 是完整管道/重定向输入，上限 4 KiB，不是流。
- 输出只能走 `out.write(..)`，禁止 `println!`，否则管道和 `$(...)` 捕获不到。
- 返回值就是 `$?`：`0` 成功、`127` 未找到、其它由命令自定义。
- 改环境用 `shell.set_var(..)`；启动应用用 `sinfo::spawn_name(..)`，没有 fork/exec。
- 管道最多 8 段，顺序捕获执行；`$(...)` 嵌套深度 ≥ 3 失败。
- `>` 通过 `unlink + open(O_CREAT)` 截断，`>>` 读旧内容再整体写回，均受 4 KiB 限制。
- `dir`/`copy`/`del` 兼容别名默认关闭；不要依赖它们编写新脚本。

### 7.5 Nuomi 两条实现线

- `user/Nuomi_pl/` 是宿主 C 参考实现与 NUC 工具链，带 GC，`nuomi -c` 只能由这套工具链完成；语言规范以 `user/Nuomi_pl/docs/Nuomi语言规范.md` 为准。
- `apps/src/nuomi.rs` 是 nsh 内置的 `no_std`、无堆、无 libm 移植；脚本无需编译或注册：`nuomi file.nuomi`、`nuomi -e "(+ 1 2)"`。

设备内刻意限制：

| 限制 | 值 |
|---|---|
| 单脚本 | 6 KiB |
| bump arena | 256 KiB，其中环境区 128 KiB 可回收，无 GC |
| 求值深度 | `MAX_DEPTH=1500`，约 500 层函数调用 |
| 单次实参 | 24 个 |
| `let`/参数绑定 | 24 个 |
| 向量/列表字面量 | 64 个元素 |
| 格式化缓冲 | 1024 字节 |

平台差异：无 JSON/HTTP/进程/窗口原语；无 `input`，`key` 恒为 `nil`；`pow` 仅整数指数，`sqrt` 自算。循环里反复 `cons` 会耗尽 arena。

### 7.6 cppcc、Win32 与路线图

- **cppcc**：主机端 Rust C++98 子集编译器，不链接内核、不依赖 LLVM/外部汇编器。支持单翻译单元、POD struct、指针/一维定长数组、受限预处理；不支持类、模板、异常、STL、浮点、函数指针、间接调用、多 TU 和动态链接，最多 6 参数。入口在 `user/cppcc/`，用 `cargo test`、`make cppcc-demo` 验证。样例只通过 boot-stage 固定读取 `APPS/DEMO.NUC`，通用 `./app.nuc` exec 尚未实现。
- **Win32 兼容层**：原型阶段，PE32+ 映射到 `0x50000000`，首批 15 个控制台/计时/Sleep/注册表 API。内核侧在 `kernel/pe.c`、`kernel/win32.c`，镜像加入独立的 `g_win32[]`，绝不能挪动 `g_apps[]` 索引。扩 API 要同步 thunk 编码、`SYS_WIN32=40`、`task_t.abi` 白名单和注册表布局。GUI、文件、堆、线程、异常、TLS、DLL 导入仍未实现。
- **Mochi**：规范存在，但当前工作树无 `user/mochi` 子模块；相关 make target 仅在源码存在时定义。
- **MicroPython**：仅路线图，没有源码和构建目标；贡献前先开 Issue 讨论自研边界与设计。

---

## 8. 生成物与资源

生成物大多已提交仓库，普通构建不需要重新生成；修改源文件后必须重新生成并提交结果。

| 源 | 生成命令 | 产物 |
|---|---|---|
| `tools/themegen/theme.toml` | `python3 tools/themegen/themegen.py` 或 `make theme` | `graphics/nd_theme.[ch]`、`apps/src/theme.rs` |
| `tools/langgen/strings*.toml` | `python3 tools/langgen/langgen.py` | `graphics/nd_lang_dict.c`、`apps/src/lang_dict.rs` |
| Unifont hex | `python3 tools/fontgen/fontgen.py --bmp unifont.hex --upper unifont_upper.hex` | `graphics/nd_font_data.bin`、`nd_font_uni.c`、`apps/src/unicode_data.rs` |
| `tools/imegen/pinyin.txt` | `python3 tools/imegen/imegen.py --gen` | `graphics/nd_ime_dict.c` |
| `assets/` 壁纸 | `python3 tools/wpgen/wpgen.py`、`make wpkg-img` | `assets/backgrounds-screen/`、`out/wallpapers.pkg` |
| 更新根证书 | `python3 tools/cabundle/cabundle.py --pem tools/cabundle/roots.pem --out security/novu/cabundle.c` | `security/novu/cabundle.c` |
| NKM 测试种子 | `make nkm-test-module` | `security/nkm/nkm_test_module.h`、`out/test.nkm` |

要求：

- `apps/src/theme.rs` 等生成文件头部明确写着“改 TOML 后重新生成，不要直接编辑”。
- 新增中文文案通常需要重生成字体数据；否则只能显示回退字形。
- 主题、语言和字体生成器支持 `--check`，可在 CI 缺失时用于本地检查生成物是否过期。
- E2E 测试证书由 `tools/updatesrv/gen_e2e_certs.sh` 本地生成，只影响 `cabundle_e2e.c`；生产 `cabundle.c` 不能被测试根污染。
- 禁止提交私钥、证书私钥、`NKM_SEED` 轮换产生的私有材料；`.gitignore` 已覆盖 `private/`、`tools/updatesrv/*.key`、`security/tls/*.key`、`user/ssl/*.key`。

---

## 9. 代码风格与提交规范

仓库目前没有统一的 `clang-format`、`rustfmt.toml`、`.editorconfig`、CI、CODEOWNERS 或 issue/PR 模板。因此：

- 遵循所在文件的既有风格和命名；不要为个人偏好做大规模无关注释/格式化改动。
- C 与主机工具代码不引入新的 `-Wall -Wextra` 警告；自检脚本按 `-Werror` 要求处理。
- Rust 使用 2021 edition；内置应用 profile 为 `panic=abort, opt-level=s, lto=true, strip=true`。尽量运行 `cargo fmt`，避免不必要的 `unsafe`。
- 不引入外部开源依赖；如确有必要，先在 Issue 中论证自研替代方案和许可证边界。Unifont 字体嵌入例外是当前明确记录的例外。
- 一个 PR 只做一件事；标题和描述说明动机、实现方式、兼容性影响、测试方法与未执行项。
- 更新相关 README、`docs/`、`changes.md`。`changes.md` 的惯例是“主题标题 → 精确到路径的要点 → 验证结果”，并如实列出未运行项。
- 不要提交 `build/`、`out/`、`esp/`、`target/`、`*.o`、OVMF VARS 副本、临时日志或机器特定绝对路径。

### PR 合并前检查

```text
[ ] make 与 make check-rootfs 通过
[ ] 按改动矩阵执行了相关单测/自检/QEMU 测试
[ ] 生成物由源文件重新生成，--check 无漂移
[ ] 镜像、blob、栈、IPC、显示列表等限制未被突破
[ ] ABI/协议变更已升版本，并同步内核、工具、测试和文档
[ ] 未破坏启动顺序、挂载规则、签名链或按名启动索引
[ ] README/docs/changes.md 已按真实实现更新
[ ] 无构建产物、私钥、机器绝对路径或未说明的临时改动
[ ] PR 描述列出实际执行命令；未执行的检查已明确说明
```

## 10. 常见开发任务配方（Recipes）

每节都是"照做即可"的最短路径。完成后回到 [§4.1](#41-必跑与按类型追加) 找对应的必跑检查。

### 10.1 加一条 nsh 内建命令

1. 在 `apps/src/shell/cmds.rs` 的其它 `cmd_*` 之后、`BUILTINS` 之前写函数：

   ```rust
   fn cmd_hello(shell: &mut Shell, cmd: &Cmd, stdin: &[u8], out: &mut dyn Sink) -> i32 {
       let _ = (shell, stdin);
       out.write(b"hello\n");
       0
   }
   ```

2. 在 `BUILTINS` 注册：`("hello", cmd_hello)`。`help`、`which`、管道和重定向会自动生效。
3. 约定：输出只能走 `out.write()`（用 `println!` 会丢管道）；返回值即 `$?`；启动应用用 `sinfo::spawn_name()`，没有 fork/exec。
4. 验证：`make`，在 QEMU 终端里执行 `hello`、`hello | cat`、`$(hello)` 三条路径。
5. 参考实现见 [templates/shell-command/](templates/shell-command/README.md)。

### 10.2 新增一个 GUI 应用

1. 复制 `templates/gui-app/` 的 `src/main.rs`，按 [模板说明](templates/README.md) 改 4 处：
   `apps/src/bin/<name>.rs`、`apps/Cargo.toml` 的 `[[bin]]`、`Makefile` 的 `APPS_NAMES`、`kernel/apps.c` 的 `g_apps[]`。
2. `g_apps[]` 只能在**末尾**追加：旧版 sysinfo `/apps` 与客户端复用数组索引，插到中间会让所有应用错位；新代码优先用 `apps_launch_name()`。
3. 名字只能是字母、数字、下划线；含 `-` 会生成非法 C 标识符 `k_app_<name>`。
4. 每帧以 `CLEAR` 开始、`SHAPE`/`TEXT` 填充、以 `INVALIDATE` 结束；`IPC_EBUSY` 必须重试；空闲时 `sys::yield_now()`。
5. 文本宽度用 `ui::width()`，颜色用语义令牌，不要写字面色值。
6. 验证：`out/apps/<name>.bin` < 128 KiB；`make` + 完整 QEMU 冒烟 + `scripts/wm-churn.sh 4G 20`。

### 10.3 新增 C 源、驱动或块/网卡设备

1. 把 `.c` 加入顶层 `Makefile` 的 `SRC`（汇编进 `ASM_SRC`，新目录进 `INC`）——没有自动发现。
2. 新配置开关同步改 `Kconfig`、`configs/novus_defconfig`、代码 `#if CONFIG_NOVUS_*`、文档四处。
3. 按设备类型接入：
   - 块设备：`blkdev_register()`，对象为静态存储且注册后常驻；对外统一 512 B 扇区。
   - 网卡：`net_register()`，探测函数加入 `net_init()` 的 probe 集合；首个 UP 的设备成为活动链路。
   - 中断：`irq_register(line, flags, isr, device, name)`；每 GSI 最多 4 个 handler，共享线路触发属性必须一致。
4. ISR 只清自己的中断源；禁止 `sti`、睡眠、调度、分配、协议栈调用和打印日志，EOI 由框架统一处理。
5. 在 `kmain()` 按硬件类别插入初始化调用点；只加源文件不会生效。
6. 若模块需要调用内核内部函数，符号必须进 `nksym_table`（取地址同时防止链接器丢弃）。
7. 验证：`make`、`bash qtest.sh` 人工看 `[MOUNT]`、`scripts/irq-selftest.sh`。

### 10.4 新增系统调用

1. 在 `kernel/trap.c` 分配**新编号**（只追加，不复用/重排），加入 `g_syscalls[]` 并实现 `sys_*`。
2. 用户指针不得直接解引用，按任务 CR3 逐页复制；长度与边界显式检查。
3. 同步用户侧头文件 `user/Nuomi_pl/include/novus_syscall.h` 与 `apps/src/sys.rs`，不要让工具链长期写裸数字。
4. 若涉及权限，先确认 [SECURITY.md](SECURITY.md) §5 的能力状态——CAP 目前是规范目标而非已落地实现。
5. 验证：`kernel/trap.c` 的未知编号告警不出现；新增拒绝非法输入的回归用例；`docs/Novus OS Debug 级接口手册 V1.0.md` 同步。

### 10.5 新增 IPC 端口服务

1. 端口名写在服务端与客户端共用头文件；内核 IPC 端口表静态 16 项，任务收件箱端口等于 pid。
2. 消息头 32 B、magic `0x4E495043`、FNV-1a checksum、载荷 ≤ 128 B、队列 8 条；超过 `IPC_PAYLOAD_MAX` 的内容改用 v2 surface 句柄，不要拆包。
3. 阻塞接收依赖 `syscall; ret` 的指令布局（返回帧 RIP 回退 2 字节重放），改 ABI 前先确认这条假设仍成立。
4. 当前没有端口强鉴权，不要把新端口当作安全边界；涉及安全的能力先开 Issue。
5. 验证：服务端 selftest + 一个跨进程客户端用例 + `scripts/wm-churn.sh`（若与窗口生命周期相关）。

### 10.6 修改生成物（主题/语言/字体/IME/壁纸）

1. 改源文件（`tools/themegen/theme.toml`、`tools/langgen/strings*.toml`、`tools/imegen/pinyin.txt` 等），**不要直接编辑生成文件**。
2. 重新生成并提交产物，例如 `make theme`、`python3 tools/langgen/langgen.py`、`make wpkg-img`。
3. 用 `--check` 确认无漂移；新增中文文案通常需要重生成字体数据，否则只能显示回退字形。
4. 验证：`scripts/font_smoke.sh` + QEMU 截图人工核验（中英混排、码点边界、主题明暗）。

### 10.7 新增 NKM 内核模块

1. 用 `tools/nkmgen` 生成模块；格式 `NKM!`、ABI major 1、208 B 头、1–16 segment、deps ≤ 8、符号 ≤ 64、重定位 ≤ 256、模块 ≤ 16、根公钥 ≤ 4。
2. 签名覆盖"除 `[0x50,0xB0)` 置零外"的整文件 SHA-256；只允许 64 位绝对重定位，PC32 会被拒绝。
3. 加载顺序固定：边界检查 → digest → signature → dependencies → map → relocate → init。
4. `nkm_exit()` 必须释放内存、IRQ 与设备注册；被其它模块依赖时拒绝卸载。
5. 验证：`make nkm-test-module`、`(cd tools/nkmgen && cargo run --quiet -- selftest)`；不要提交 `NKM_SEED` 轮换产生的私有材料。

---

## 11. 故障排查手册（Troubleshooting）

| 症状 | 最可能的原因 | 先做什么 |
|---|---|---|
| `make` 报 `undefined symbol k_app_*` | 应用加了 `[[bin]]` 但没进 `APPS_NAMES` 或 `g_apps[]` | 按 [§10.2](#102-新增一个-gui-应用) 核对四处注册 |
| `out/apps/*.bin` 超过 128 KiB | 引入 `Vec/String` 或大静态缓冲 | 回到固定数组；检查 `apps.stamp` 是否触发重建 |
| 启动停在 splash、最后一条是"映射内存" | 页表/帧缓冲映射失败或帧缓冲在 4 GiB 以上 | 看 `[ND]` 行；确认 `mm_map_phys(..., MM_MAP_UC)` 路径 |
| 串口无任何输出 | `CONFIG_NOVUS_DEBUG_SERIAL=n` 或 QEMU 串口重定向 | 确认 defconfig 与 `scripts/run.sh` |
| 桌面出现白/黑横带、半帧闪烁 | 显示列表缺 `INVALIDATE`，或自绘绕开合成缓冲 | 帧尾补 `INVALIDATE`；只画 `nd_canvas()` |
| 中文显示方框 | 字模未包含该码点，或生成物过期 | 重生成字体数据；检查路径是否按码点边界截断 |
| `check-rootfs` 报目录数不符 | `rootfs_layout.h` 与 `fsinfo`/`check_rootfs.py` 计数漂移 | 以 `rootfs_layout.h` 为真源同步（见 ROADMAP §0.3） |
| QEMU 网卡拿不到地址 | 网卡模型不匹配，或 DHCP 超时重发循环状态未清零 | 用 `NOVUS_NIC=e1000/e1000e/virtio-net/rtl8139` 逐个试；`igb` 在 QEMU 下不置 LU 属已知行为 |
| `[MRKL] MISMATCH` | Merkle 根与系统分区不一致，或运行期回写了 `mounts.json` | 检查是否改动受保护元数据；看 A 核自检日志 |
| QEMU 冒烟基线不匹配 | UI 有意变更未更新基线，或截图含动态内容 | 普通回归**不要**带 `--update-baseline`；有意变更后逐张人工核验再提交 |
| `make check-lint` 新增告警 | 本地 clippy/rustfmt 与基线不一致 | 只修新增项；不要整体重格式化 |
| `qtest.sh` 看似通过但磁盘没挂上 | 脚本用 `|| true` 吞掉失败 | 人工 grep `/tmp/b_*.log` 的 `[MOUNT]` 标记 |
| 生成器 `--check` 报 stale | 改了 TOML 没重生成，或生成文件被手改 | 重生成并提交；不要手改生成文件 |
| Win32 样例退出码 0 但无输出 | thunk/API 白名单未覆盖该调用 | 查 `kernel/win32.c` 的 API 表与 `SYS_WIN32` 白名单 |
| `nuomi` 报 `out of memory` | bump arena 用尽（循环里反复 `cons`） | 减少数据量或改写为尾递归/就地更新 |

**定位顺序建议**：主机自检 → `make` → `make check-rootfs` → QEMU 串口 → QEMU 截图 → 交互脚本。越靠前越快，不要在镜像没过校验时就去查 UI。

---

## 12. 常见问题（FAQ）

**Q：为什么 `apps/` 不能用 `Vec`、`String`、`Box`？**
A：内置应用是 `no_std` 无分配器的扁平 NUC 镜像，直接编进内核。所有缓冲开在栈或静态区；`panic` 只向 COM1 打印并 `exit(3)`，没有回溯。

**Q：为什么 nsh 的管道不是并行的？**
A：第一阶段有意简化：没有 fork/exec，管道是顺序捕获（≤8 段，4 KiB 缓冲）。这是已知限制，升级为真正并发管道需要进程模型增强，见 ROADMAP Phase 1。

**Q：为什么没有 `make menuconfig`？**
A：`Kconfig` 是声明式占位，真正被 `Makefile` 读取的是 `configs/novus_defconfig`；`y/n` 只转 C 宏 `1/0`，`depends on` 不会自动校验，需要人工保证（如 `IRQ → SCHED`、`GUI → DISPLAY`）。完整解析器是后续里程碑。

**Q：为什么 README 说有 `system/`、`drivers/`，实际没有？**
A：README 保留了早期规范规划。当前真实源码地图见 [§1.2](#12-真实源码地图)；遇到冲突按 [§0 规范优先级](#novus-os-贡献指南) 处理，并欢迎提交勘误 PR。

**Q：QEMU 通过就代表真机没问题吗？**
A：不代表。已知的 QEMU/真机差异包括：帧缓冲缓存属性（WB/UC 别名导致真机花屏）、JPEG 解码器工具链相关 UB、igb 在 QEMU 下不置 `STATUS.LU`。涉及显示/解码/网卡的改动必须说明真机验证情况。

**Q：为什么不能随便 `--update-baseline`？**
A：截图基线是回归门禁。普通回归更新基线等于把回归变成"自证清白"；只有 UI 有意变更时才允许，且必须逐张人工检查脚本生成的无损 RGB PNG。

**Q：为什么不直接把规范里的 CAP/Secure Boot 当成已实现来写代码？**
A：[SECURITY.md](SECURITY.md) §5 明确列出当前能力边界：CAP 最小权限模型、Secure Boot、端口强鉴权、密钥轮换、硬件 TRNG 都是规范目标。以规范文本替代实现会形成虚假安全承诺。

**Q：为什么有些自检是 WARN 而不是 ERROR？**
A：项目原则是"轻微错误不 panic"。NovusFS/VFS/ext4 自检失败、Merkle 结论异常都会降级继续启动。但**降级不等于通过**——PR 描述里必须写明日志里出现过哪些 WARN。

**Q：贡献者需要跑完所有测试吗？**
A：按 [§4.1](#41-必跑与按类型追加) 的改动矩阵跑最小必跑项。仓库没有托管 CI，未执行的检查必须在 PR 中明确说明；不要假装跑过。

---

## 13. PR 描述模板

仓库没有 PR 模板，请按下面的结构填写。中英文均可，但每一项都要有内容。

````markdown
## 动机
修改前的具体问题是什么？关联 Issue / ROADMAP 缺口 ID（如 G-F02）。

## 实现方式
- 改了哪些文件、关键函数与数据流（路径级，不要只写"优化了 GUI"）
- 是否新增/修改了 ABI、配置开关、冻结格式？

## 兼容性影响
- [ ] 无 ABI/协议/磁盘格式变化
- [ ] 有变化：已升版本并同步规范、内核、宿主工具、测试夹具、版本号、兼容性说明
- [ ] 启动顺序 / 挂载规则 / 签名链 / g_apps[] 索引未受影响

## 测试方法
实际执行过的命令（逐条列出，含参数）：
```
make
make check-rootfs
bash scripts/irq-selftest.sh
...
```
验证结果（日志标记、截图路径、JSON 报告）：

## 未执行的检查
明确列出没跑的项与原因（缺硬件、缺工具、环境不匹配等）。

## 文档
- [ ] README / docs / changes.md 已按真实实现更新
- [ ] 若修正了旧文档冲突，说明以哪份文件为准
- [ ] 生成物已由源文件重新生成，`--check` 无漂移

## 仓库卫生
- [ ] 无 build/out/esp/target/*.o、私钥、机器绝对路径或临时改动
````

**提交粒度**：一个 PR 只做一件事。"顺手修个拼写"和"新增系统调用"请分开，否则回滚与审查都会变困难。

本项目采用 GPLv2：提交者保留版权，所有修改源码必须按 GPLv2 条款开源。详见 [LICENSE](LICENSE) 与 [COPYING](COPYING)。

第三方设备驱动可按 [DRIVER-LICENSE.md](DRIVER-LICENSE.md) 以闭源方式分发（GPLv2 第 10 节额外权限），但不覆盖对项目源码的修改；闭源驱动同样必须通过 NKM 签名校验、只使用冻结 ABI，且不免除安全要求。
