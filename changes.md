# Changes

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