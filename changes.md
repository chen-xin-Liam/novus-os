# Changes

## M9 Win32 兼容层原型

- 新增 `kernel/pe.h` / `kernel/pe.c`：PE32+（x86-64）加载器。按文件偏移读
  头部与节表，校验 MZ/PE 签名、Machine、Magic、节边界与重叠、入口所在节，
  拒绝 DLL、TLS、延迟导入与序号导入；在 `0x50000000` 映射头部、各节
  （`.bss` 清零）、thunk 页、命令行页、64 KiB 栈与返回跳板，应用
  `IMAGE_REL_BASED_DIR64`，按名称填 IAT，最后交给
  `sched_create_user_ex(..., TASK_ABI_WIN32)`。
- 新增 `kernel/win32.h` / `kernel/win32.c`：API 门与模拟注册表。15 个 API
  （控制台输出、tick、`Sleep`、错误码、命令行、模块基址、`Reg*`），每条
  thunk 保存并恢复 RDI 后以 `syscall 40` 进入；`Sleep` 走
  `sched_park` + `f->rip -= 2` 重放。注册表是 16 项扁平表，`REG_SZ`/
  `REG_DWORD`，持久化到 `/registry.cfg`，live 只读根下仅内存态。
- `task_t` 增加 `abi`、`win32_last_error`、`win32_image_base`；调度器新增
  `sched_create_user_ex`，`syscall_dispatch` 对 `TASK_ABI_WIN32` 只放行
  `SYS_WIN32` 与 `SYS_EXIT`。
- 新增样例 `user/win32compat/samples/hello.c`（不用 `<windows.h>` 与 CRT），
  `Makefile` 用 `x86_64-w64-mingw32-gcc` 生成 `out/win32/hello.exe` 并
  `xxd -i` 成 `build/win32_blobs.h`；新增 `CONFIG_NOVUS_WIN32`（默认 y）与
  `CONFIG_NOVUS_WIN32=y` 的 defconfig 项。
- sysinfo 新增 `/exe` 目录（`SYSINFO_DIR_EXE`、`SYSINFO_KIND_EXE`），文件
  管理器增加 `/exe` 标签，`KIND_EXE` 可选中运行。
- 验证：`.reloc` 存在且为 DIR64；导入表为 KERNEL32/ADVAPI32 的 9 个函数；
  QEMU 中从 `/exe` 运行 `hello.exe`，串口依次输出加载行、三条 Win32 输出
  与 `task hello.exe exited with code 0`；`CONFIG_NOVUS_WIN32=n` 仍可构建。
- 文档：新增 `docs/M9 Win32 兼容层原型.md`；README 里程碑 M9 标记完成，并把
  原有的 Win11 桌面/中文说明限定为「M9 的 Win11 桌面/中文部分」。

## M11 阶段 1：会话、双语与安装器

- live 介质启动直接进入 `live` 会话，不再要求用户名/密码；只有安装到可写
  NovusFS 后才进行首次账户创建。安装模式只启动 `setup`。
- 新增中英双语：`tools/langgen/strings*.toml` 生成
  `graphics/nd_lang_dict.c` 与 `apps/src/lang_dict.rs`；`nd_text` /
  `nd_text_width` 在渲染层翻译，`ui::width` 用同一张表测量。
- 新增 `SYSINFO_REQ_LANG (0x030F)` 与 `kernel/lang.c`：切换立即生效，
  已安装系统持久化到 `/language.cfg`，安装器把选择写入新系统盘。
- 安装器改为七页：语言、协议、磁盘、方案、确认、进度、完成/失败；新增 EULA、
  文件系统选择（NovusFS 可用，NTFS/ext4 标注"规划中"并由内核拒绝）。
- 新增 `kernel/usb.c`：xHCI 控制器（命令/事件环、端口复位、Enable Slot、
  Address Device、描述符、SET_CONFIGURATION、Configure Endpoint）与 BOT
  （CBW/CSW、READ CAPACITY、READ10/WRITE10），磁盘注册为 `usb00`。
- `kernel/pci.c` 支持 64 位 BAR：按 BAR 对读取基址与长度，新增
  `pci_bar_assign()`；固件把 xHCI 的 BAR0 放在 4 GiB 以上时，驱动把它搬进
  32 位 PCI 空洞并读回能力寄存器确认。
- 引导阶段从引导卷的设备路径解析 USB 端口并发布到共享内存；内核据此给
  正在启动的盘打 `BLKDEV_FLAG_BOOT_ONLY`，向导拒绝它作为目标。
- 新增 `docs/多语言、会话与 M11 输入输出.md`，记录已完成与未完成部分。
- 多显示器 / EDID / 镜像尚未实现，仍为单输出 GOP 路径。

## M11 阶段 1 续：多输出显示与窗口重绘

- `graphics/nd_hal.c` 枚举固件提供的全部 GOP 输出（上限 4 个），读取 EDID
  首选时序与数字/模拟链路，控制器支持时切到首选模式；第一个输出作主屏，
  其余输出镜像同一画面（`nd_mirror_rect()`，字节序不同时逐像素交换红蓝）。
- `nd_outputs()` 按"每个帧缓冲一个条目"报告，固件里指向同一显存的第二个
  头衔不再被当成一块显示器；设置 → 系统新增"显示输出"卡片。
- `kernel/sysinfo.c` 新增 `SYSINFO_REQ_DISPLAY (0x0318)` /
  `SYSINFO_RSP_DISPLAY (0x0319)`，客户端 `sinfo::display()`。
- 修复窗口重绘缺陷：CLEAR/SHAPE/TEXT 不再逐条触发重绘，改由帧尾 INVALIDATE
  一次性绘制——此前一帧 50 余条消息会让半帧状态闪现（关窗后下方窗口内容
  只剩一半）。
- `nd_selftest` 增加 3 项镜像字节序往返检查（37 项全过）。

## 验证

- 标准安装（AHCI 目标）端到端通过；英文安装通过；安装后磁盘启动读取
  `[LANG] loaded: en` 并进入首次账户设置；live 启动出现
  `[SESSION] live session: no account required`。
- USB：`[USB] BAR0 moved to 0x00000000F0000000`、`disk usb00 capacity
  0x0000000000100000 sectors`，安装到 U 盘 `begin on usb00` →
  `partition table written` → `ESP written` → `system image written` →
  `verify root ok` → `install complete`；从该 U 盘启动得到
  `[BOOT] live medium is on USB` 与 `is the boot medium`，Merkle 校验 PASS。
- 未创建 git commit。

## M10 UI 统一与图形安装向导

- 新增 Novus Fluent 令牌单一来源 `tools/themegen/theme.toml`，生成
  `graphics/nd_theme.[ch]` 与 `apps/src/theme.rs`；`make theme` 重新生成，
  支持浅色、深色、跟随时间（19:00–07:00 深色）三种模式。
- `nvgui` 的主题改由 `nd_tokens()` 填充；壳层在版本变化时重建控件树，窗口管理器
  标题栏按当前主题绘制；`sysinfo` 主题回复尾部追加 `mode/resolved/version`。
- Novus-WM 显示列表新增圆角填充/圆角描边（op 6/7，半径在 `pad`），
  `apps/src/ui.rs` 升级为组件库（page/card/nav/row/button/ghost/progress/empty）。
- 设置、文件管理器、任务管理器、网络与登录界面改用角色色并轮询主题版本；
  设置页增加主题模式选择、强调色与"打开安装程序"入口。
- 引导阶段把 `ESP.IMG` 暂存到 reserved pages，并在共享内存保留尾部发布
  `esp_img_base/esp_img_size/install_flags`；菜单 3 与 `INSTALL.YES` 改为设置
  `boot_mode=INSTALL` 后进入图形安装，文本安装保留在 `CONFIG_NOVUS_INSTALLER_TEXT`。
- 新增 `kernel/install.[ch]` 与 `SYS_SETUP=19`：候选盘判定、GPT、ESP/系统镜像
  分步复制、NovusFS 挂载与 Merkle 校验、setup pid 授权；`blkdev_release()` 归还
  分区视图槽位。
- 新增 `apps/src/install.rs` 与六页向导 `apps/src/bin/setup.rs`；应用表新增 `setup`，
  启动改为按名字解析（`apps_launch_name`、`SYSINFO_REQ_SPAWN_NAME`），
  新窗口创建即取得焦点。
- 新增 `scripts/run-install-test.sh`、`docs/Novus UI 设计语言规范 V1.0.md`、
  `docs/图形安装向导设计与实现.md`；重新生成中文字模与主题文件。
- 更正 README 中"SMP AB双核"为"SMP AP双核"。

## 验证

- 构建：`make` + `make iso` 通过（mingw 交叉 gcc，Rust 应用以 dev 用户与
  `~/.cargo` 构建）。
- 安装：QEMU q35 + OVMF，ISO 菜单选择 3 后 `setup` 启动，键盘走完六页；
  串口输出 `partition table written` → `ESP written` → `system image written` →
  `verify root ok` → `install complete`。
- 启动：从安装后的目标盘单独启动，`[MRKL] ... PASS`、`NovusFS mounted`、
  `launching session`。
- 主题：`-rtc base=localtime` 下 AUTO 解析为深色，桌面、任务栏、登录窗口同步切换。
- 修复：`nd_tokens()` 首次调用时懒构建调色板。此前浅色模式启动时 `g_current`
  全零，桌面被画成黑色（深色模式恰好因自动解析触发构建而正常）。
- 修复：瓦片标题统一用 1x 字号。2x 汉字每字 32 px，五个字的名称在 96 px 瓦片里
  会被裁掉。
- 修复：候选盘判定允许 Novus 自身布局（恰好 ESP + NovusFS 两分区）覆盖重装；
  失败页的"重试"因此可用，其他分区表仍拒绝。
- 未创建 git commit。

## cppcc M7 文档与构建集成

- 新增 `user/cppcc/README.md`，说明实际支持的 C++ 子集、构建/CLI 用法、syscall builtin 和已知限制。
- 新增 `docs/M7 cppcc 实现说明.md`，记录直接 AST 驱动后端、x86_64 codegen、NUC V1.0 字段映射与 boot-stage 加载路径。
- 修订 `docs/M7 cppcc 实施方案.md` 的状态和与实际 codegen 不同的 IR、栈、syscall 与 `_start` 描述；保留其历史规划用途。
- README 第 7 部分及顶层摘要改为 x86_64 原生机器码加载执行，并注明当前 fixed-path staging 与尚未实现的通用 shell exec。
- 将 M8、WM 文档中的 M7 暂缓语境标注为历史；NKM 规范明确 cppcc 输出 NUC、`nkmgen` 负责测试 NKM 镜像。
- 新增 `user/cppcc/examples/hello.cc`，调用 `novus_write` 输出 `Hello, Novus!`。
- Make/Kconfig 增加 `CONFIG_USER_CPPCC`；`make`/`make cppcc` 构建 host 工具，`make cppcc-demo` 生成 `out/cppcc/hello.nuc`。
- `make run` 在启动 `scripts/run.sh` 前将 smoke NUC staging 为 `esp/APPS/DEMO.NUC`；ESP 镜像规则打包同一路径。cppcc 启用时优先占用 boot-stage 固定 demo 槽。
- 更新 README 第 7 部分的执行模型，并将 M8/WM/NKM/M7 历史说明改为与当前实现一致；NUC V1.0 格式规范未修改。

## 验证

- cppcc Rust 文件静态诊断：无错误。
- 已添加 NUC 字节布局、Hello 生成、控制流/函数调用以及语法错误行列测试。
- 本机缺少 `cargo`、`rustc`、`make`、MinGW、`qemu-system-x86_64` 和 `xxd`；`cargo test`、实产物字节检查和 QEMU 启动未执行。
- 仓库未发现 `nucdump`。
- 未创建 git commit。